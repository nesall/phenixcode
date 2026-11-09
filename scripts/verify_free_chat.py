"""Check free-chat context isolation and attachment handling with local fixtures.

Run with --binary pointing to a built core executable. All configuration,
indexed code, and databases are disposable. Providers use localhost exclusively.
"""

import argparse
from contextlib import closing
import http.server
import json
import os
from pathlib import Path
import queue
import socket
import sqlite3
import subprocess
import tempfile
import threading
import time
import urllib.error

from verify_reasoning_serialization import ROOT, request_json


def verify(binary, tokenizer):
    captured = queue.Queue()
    embeddings = []
    project_marker = "PROJECT_CONTEXT_SENTINEL_830572"
    upload_marker = "USER_ATTACHMENT_SENTINEL_491807"
    thought = "Fixture reasoning"

    class Provider(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/embeddings":
                embeddings.append(body)
                inputs = body["content"]
                content = json.dumps([{"embedding": [[1.0, 0.0, 0.0]]} for _ in inputs])
                content_type = "application/json"
            else:
                captured.put(body)
                anthropic = self.path == "/messages"
                native = ({"role": "assistant", "content": [
                    {"type": "thinking", "thinking": thought, "signature": "fixture-signature"},
                    {"type": "text", "text": "Mock answer"}]} if anthropic else {
                        "role": "assistant", "content": "Mock answer", "reasoning_content": thought})
                if not body["stream"]:
                    response = {**native, "stop_reason": "end_turn"} if anthropic else {
                        "choices": [{"message": native, "finish_reason": "stop"}]}
                    content, content_type = json.dumps(response), "application/json"
                else:
                    if anthropic:
                        events = []
                        for index, block in enumerate(native["content"]):
                            events.extend([{"type": "content_block_start", "index": index, "content_block": block},
                                           {"type": "content_block_stop", "index": index}])
                        events.extend([{"type": "message_delta", "delta": {"stop_reason": "end_turn"}},
                                       {"type": "message_stop"}])
                    else:
                        events = [{"choices": [{"delta": {"reasoning_content": thought}}]},
                                  {"choices": [{"delta": {"content": "Mock answer"}, "finish_reason": "stop"}]}]
                    content = "".join("data: " + json.dumps(event) + "\n\n" for event in events)
                    if not anthropic:
                        content += "data: [DONE]\n\n"
                    content_type = "text/event-stream"
            data = content.encode()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="phenixcode-free-chat-") as directory:
            work = Path(directory)
            code = work / "code_context_probe.py"
            code.write_text('def code_context_probe():\n    """Return the project-specific fixture marker."""\n'
                            f'    return "{project_marker}"\n', encoding="utf-8")
            mock_url = f"http://127.0.0.1:{server.server_port}"
            providers = []
            for style in ["openai", "anthropic"]:
                for streaming in [False, True]:
                    identifier = style + ("-stream" if streaming else "-full")
                    providers.append({"id": identifier, "name": identifier, "model": identifier,
                        "api_url": mock_url + ("/messages" if style == "anthropic" else "/completion"),
                        "api_key": "fixture", "api_style": style, "stream": streaming, "context_length": 4096,
                        "reasoning": {"history_replay": 3, "parameters": [{"name": "reasoning_effort",
                            "value_type": "scalar", "values": ["low", "high"], "default": "low"}]}})
            small = {**providers[0], "id": "small", "model": "small", "context_length": 512}
            catalog = {"generation_providers": providers + [small], "embedding_providers": [{
                "id": "fixture", "model": "fixture", "api_url": mock_url + "/embeddings", "api_key": "fixture"}]}
            settings = {
                "tokenizer": {"config_path": str(tokenizer)},
                "embedding": {"current_api": "fixture"},
                "generation": {"current_api": providers[0]["id"], "enabled_providers": [p["id"] for p in providers] + ["small"],
                    "auto_router": {"enabled": False}, "timeout_ms": 5000, "default_max_tokens": 128,
                    "excerpt": {"enabled": False}},
                "source": {"project_id": "free-chat-fixture", "project_title": "Free chat fixture",
                           "paths": [{"type": "file", "path": str(code)}]},
                "chunking": {"nof_min_tokens": 5, "nof_max_tokens": 100},
                "database": {"sqlite_path": str(work / "metadata.db"), "index_path": str(work / "vectors.index"),
                             "vector_dim": 3, "max_elements": 100},
                "logging": {"log_to_console": False, "log_to_file": False,
                            "logging_file": str(work / "app.log"), "diagnostics_file": str(work / "diagnostics.log")},
            }
            (work / "providers.json").write_text(json.dumps(catalog))
            (work / "settings.json").write_text(json.dumps(settings))
            env = {**os.environ, "EMBEDDER_REGISTRY": str(work / "registry.sqlite"),
                   "EMBEDDER_METRICS": str(work / "metrics.sqlite")}
            common = [str(binary), "--config", str(work / "settings.json"), "--providers-config", str(work / "providers.json"),
                      "--no-startup-tests"]
            embedded = subprocess.run(common + ["embed"], cwd=work, env=env, capture_output=True,
                                      input="y\n", text=True, timeout=30,
                                      creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            assert embedded.returncode == 0 and embeddings, embedded.stdout + embedded.stderr
            with closing(sqlite3.connect(work / "metadata.db")) as inspection:
                assert inspection.execute("SELECT COUNT(*) FROM chunks").fetchone()[0] > 0, "Fixture code was not indexed"
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            info = work / "instance.json"
            command = common + ["serve", "--yes", "--port", str(port), "--info-file", str(info), "--appkey", "fixture"]
            with (work / "process.log").open("w") as log:
                process = subprocess.Popen(command, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                base = None
                try:
                    deadline = time.monotonic() + 25
                    while time.monotonic() < deadline and process.poll() is None:
                        try:
                            base = f"http://127.0.0.1:{json.loads(info.read_text())['port']}"
                            request_json(base + "/api/health")
                            break
                        except (OSError, ValueError, KeyError):
                            base = None
                            time.sleep(0.05)
                    if base is None:
                        raise RuntimeError("Startup failed: " + (work / "process.log").read_text(errors="replace"))

                    checks = 0
                    question = "What does code_context_probe return?"
                    def chat(provider, label, messages=None, **options):
                        nonlocal checks
                        body = {"messages": messages or [{"role": "user", "content": question}],
                            "targetapi": provider, "session_id": label, "response_id": label + "-response",
                            "attachments": [], "sourceids": [str(code)], "ctxratio": 1.0, **options}
                        before = len(embeddings)
                        response = request_json(base + "/api/chat", body)
                        events = [json.loads(line[6:]) for line in response.splitlines()
                                  if line.startswith("data: ") and line != "data: [DONE]"]
                        assert any(event.get("content") == "Mock answer" for event in events), response
                        outgoing = captured.get(timeout=3)
                        assert "free_chat" not in outgoing and outgoing["reasoning_effort"] == "low", outgoing
                        metadata = next(event for event in events if event.get("type") == "context_sources")
                        assert metadata["model"] == provider and metadata.get("replay_id"), metadata
                        if options.get("free_chat"):
                            assert len(embeddings) == before, "Free chat invoked the embedding provider"
                            assert project_marker not in json.dumps(outgoing), outgoing
                            assert "helpful software developer assistant" not in json.dumps(outgoing), outgoing
                            assert not any("Searching for relevant" in event.get("content", "") or
                                           "using text grep" in event.get("content", "") for event in events), events
                            assert "".join(event["content"] for event in events if event.get("type") == "reasoning_delta") == thought
                        else:
                            assert len(embeddings) > before, "Indexed normal chat did not exercise embedding retrieval"
                        checks += 1
                        print("PASS", label)
                        return outgoing, metadata, events

                    for provider in providers:
                        identifier = provider["id"]
                        for mode in [False, None]:
                            options = {} if mode is None else {"free_chat": mode}
                            outgoing, _, _ = chat(identifier, identifier + ("-omitted" if mode is None else "-normal"), **options)
                            assert project_marker in outgoing["messages"][-1]["content"], outgoing
                            assert "helpful software developer assistant" in outgoing["messages"][-1]["content"]
                        for attached_only in [False, True]:
                            history = [{"role": "system", "content": "Be helpful."}, {"role": "user", "content": "Earlier question"},
                                       {"role": "assistant", "content": "Earlier answer"}, {"role": "user", "content": question}]
                            outgoing, metadata, _ = chat(identifier, identifier + "-plain-" + str(attached_only), history,
                                                        free_chat=True, attachedonly=attached_only)
                            expected = history[1:] if provider["api_style"] == "anthropic" else history
                            assert outgoing["messages"] == expected, outgoing
                            assert not metadata["sources"], metadata
                            if provider["api_style"] == "anthropic":
                                assert outgoing["system"] == "Be helpful."
                            outgoing, metadata, _ = chat(identifier, identifier + "-upload-" + str(attached_only),
                                free_chat=True, attachedonly=attached_only,
                                attachments=[{"filename": str(code), "content": upload_marker},
                                             {"filename": "notes.txt", "content": "USER_NOTES_MARKER"}])
                            prompt = outgoing["messages"][-1]["content"]
                            assert upload_marker in prompt and "USER_NOTES_MARKER" in prompt and question in prompt, prompt
                            assert metadata["sources"] == [str(code), "notes.txt"], metadata

                        _, seed_meta, _ = chat(identifier, identifier + "-project-seed")
                        history = [{"role": "user", "content": question},
                                   {"role": "assistant", "content": "Mock answer", "replay_id": seed_meta["replay_id"]},
                                   {"role": "user", "content": "Follow up"}]
                        outgoing, free_meta, _ = chat(identifier, identifier + "-project-seed", history, free_chat=True,
                                                      response_id=identifier + "-free-response")
                        assert outgoing["messages"][1] == {"role": "assistant", "content": "Mock answer"}, outgoing
                        history.extend([{"role": "assistant", "content": "Mock answer", "replay_id": free_meta["replay_id"]},
                                        {"role": "user", "content": "Another follow up"}])
                        outgoing, _, _ = chat(identifier, identifier + "-project-seed", history, free_chat=True,
                                             response_id=identifier + "-free-next")
                        assert thought in json.dumps(outgoing["messages"][3]), outgoing
                        assert outgoing["messages"][-1]["content"] == "Another follow up"

                    large = "uploadword " * 10000
                    outgoing, _, events = chat("small", "large-upload", free_chat=True,
                                               attachments=[{"filename": "large.txt", "content": large}])
                    assert "uploadword" in outgoing["messages"][-1]["content"]
                    assert len(outgoing["messages"][-1]["content"]) < len(large)
                    assert any("truncated" in event.get("content", "") for event in events)
                    try:
                        request_json(base + "/api/chat", {"messages": [{"role": "user", "content": question}], "free_chat": "true"})
                        raise AssertionError("Accepted a non-boolean free_chat flag")
                    except urllib.error.HTTPError as error:
                        assert error.code == 400
                    print(f"{checks} free-chat and normal-chat requests plus flag validation passed.")
                finally:
                    if process.poll() is None and base:
                        try:
                            request_json(base + "/api/shutdown", {}, {"X-App-Key": "fixture"})
                        except OSError:
                            pass
                    try:
                        process.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--tokenizer", type=Path, default=ROOT / "assets" / "bge_tokenizer.json")
    args = parser.parse_args()
    verify(args.binary.resolve(), args.tokenizer.resolve())
