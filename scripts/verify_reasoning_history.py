"""Verify reasoning replay through /api/chat using disposable local fixtures only."""

import argparse
import http.server
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import tempfile
import threading
import time

from verify_reasoning_serialization import ROOT, request_json


def verify(binary, tokenizer):
    captured = queue.Queue()
    thinking = "Private reasoning about invalid input and error handling."

    def message(family):
        if family == "mistral":
            return {"role": "assistant", "content": [
                {"type": "thinking", "thinking": [{"type": "text", "text": thinking}]},
                {"type": "text", "text": "Answer"}]}
        if family == "anthropic":
            return {"role": "assistant", "content": [
                {"type": "thinking", "thinking": thinking, "signature": "opaque-signature"},
                {"type": "text", "text": "Answer"}]}
        return {"role": "assistant", "content": "Answer", "reasoning_content": thinking}

    class Provider(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            captured.put(body)
            family = body["model"].split("-")[0]
            native = message(family)
            if not body["stream"]:
                response = {**native, "stop_reason": "end_turn"} if family == "anthropic" else {
                    "choices": [{"message": native, "finish_reason": "stop"}]}
                payload = json.dumps(response).encode()
            else:
                if family == "anthropic":
                    events = []
                    for index, block in enumerate(native["content"]):
                        field = "thinking" if block["type"] == "thinking" else "text"
                        start = {**block, field: ""}
                        start.pop("signature", None)
                        events.extend([
                            {"type": "content_block_start", "index": index, "content_block": start},
                            {"type": "content_block_delta", "index": index,
                             "delta": {"type": field + "_delta", field: block[field]}},
                        ])
                        if "signature" in block:
                            events.append({"type": "content_block_delta", "index": index,
                                           "delta": {"type": "signature_delta", "signature": block["signature"]}})
                        events.append({"type": "content_block_stop", "index": index})
                    events.extend([{"type": "message_delta", "delta": {"stop_reason": "end_turn"}},
                                   {"type": "message_stop"}])
                else:
                    deltas = ([{"content": native["content"][:1]},
                               {"content": [{"type": "text", "text": "An"}]}, {"content": "swer"}]
                              if family == "mistral" else [
                                  {"reasoning_content": "Private "}, {"reasoning_content": thinking[len("Private "):]},
                                  {"content": "An"}, {"content": "swer"}])
                    events = [{"choices": [{"delta": delta}]} for delta in deltas]
                    events.append({"choices": [{"delta": {}, "finish_reason": "stop"}]})
                # Deliberately fragment both the JSON and CRLF event boundaries.
                payload = ("".join("data: " + json.dumps(event) + "\r\n\r\n" for event in events) +
                           ("" if family == "anthropic" else "data: [DONE]\r\n\r\n")).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream" if body["stream"] else "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            for offset in range(0, len(payload), 7):
                self.wfile.write(payload[offset:offset + 7])
                self.wfile.flush()

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="phenixcode-replay-") as directory:
            work = Path(directory)
            providers = []
            for family, replay in [("mistral", 3), ("deepseek", 5), ("anthropic", -1), ("off", 0), ("legacy", None)]:
                for streaming in [False, True]:
                    identifier = family + ("-stream" if streaming else "-full")
                    provider = {"id": identifier, "model": identifier, "name": identifier,
                                "api_url": f"http://127.0.0.1:{server.server_port}/completion", "api_key": "fixture",
                                "api_style": "anthropic" if family == "anthropic" else "openai",
                                "context_length": 32000, "stream": streaming}
                    if replay is not None:
                        provider["reasoning"] = {"history_replay": replay}
                    providers.append(provider)
            catalog = {"generation_providers": providers, "embedding_providers": [{
                "id": "fixture", "model": "fixture", "api_url": f"http://127.0.0.1:{server.server_port}/embedding",
                "api_key": "fixture"}]}
            settings = {
                "tokenizer": {"config_path": str(tokenizer)}, "embedding": {"current_api": "fixture"},
                "generation": {"current_api": providers[0]["id"], "auto_router": {"enabled": False},
                               "enabled_providers": [p["id"] for p in providers], "timeout_ms": 5000,
                               "default_max_tokens": 256, "excerpt": {"enabled": False}},
                "source": {"project_id": "fixture", "project_title": "Replay fixture", "paths": []}, "chunking": {},
                "database": {"sqlite_path": str(work / "metadata.db"), "index_path": str(work / "vectors.index"),
                             "vector_dim": 3, "max_elements": 100},
                "logging": {"log_to_console": False, "log_to_file": False,
                            "logging_file": str(work / "app.log"), "diagnostics_file": str(work / "diagnostics.log")},
            }
            (work / "settings.json").write_text(json.dumps(settings))
            (work / "providers.json").write_text(json.dumps(catalog))
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            info = work / "instance.json"
            command = [str(binary), "--config", str(work / "settings.json"), "--providers-config", str(work / "providers.json"),
                       "--no-startup-tests", "serve", "--yes", "--port", str(port), "--info-file", str(info), "--appkey", "fixture"]
            env = {**os.environ, "EMBEDDER_REGISTRY": str(work / "registry.sqlite"), "EMBEDDER_METRICS": str(work / "metrics.sqlite")}
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

                    for provider in providers:
                        identifier = provider["id"]
                        limit = provider.get("reasoning", {}).get("history_replay", 0)
                        limit = 255 if limit == -1 else limit
                        history = []
                        for turn in range(8):
                            history.append({"role": "user", "content": f"Question {turn}"})
                            response_id = identifier + str(turn)
                            body = {"messages": history, "session_id": identifier, "response_id": response_id,
                                    "targetapi": identifier, "attachedonly": True, "sourceids": [],
                                    "attachments": [{"filename": "fixture.txt", "content": "Local context"}]}
                            response = request_json(base + "/api/chat", body)
                            events = [json.loads(line[6:]) for line in response.splitlines()
                                      if line.startswith("data: ") and line != "data: [DONE]"]
                            reasoning_events = [event for event in events if event.get("type") == "reasoning_delta"]
                            assert "".join(event["content"] for event in reasoning_events) == thinking, (identifier, reasoning_events)
                            assert all("Private" not in event.get("content", "") for event in events
                                       if event.get("type") != "reasoning_delta"), response
                            assert "opaque-signature" not in response, response
                            assert any(event.get("content") in ["Answer", "An"] for event in events), response
                            outgoing = captured.get(timeout=3)
                            assistant_messages = [m for m in outgoing["messages"] if m["role"] == "assistant"]
                            enriched = [m for m in assistant_messages if "Private" in json.dumps(m)]
                            assert len(enriched) == min(turn, limit), (identifier, turn, outgoing)
                            assert "replay_id" not in json.dumps(outgoing), outgoing
                            if identifier.startswith("anthropic"):
                                assert all(m["content"][0]["signature"] == "opaque-signature" for m in enriched)
                            reference = next((event["replay_id"] for event in events if "replay_id" in event), None)
                            assert bool(reference) == bool(limit), (identifier, response)
                            reply = {"role": "assistant", "content": "Displayed answer with sources and model label"}
                            if reference:
                                reply["replay_id"] = reference
                            history.append(reply)
                        history[0]["content"] = "Edited original question"
                        history.append({"role": "user", "content": "After edit"})
                        body.update(messages=history, response_id=identifier + "-edited")
                        request_json(base + "/api/chat", body)
                        outgoing = captured.get(timeout=3)
                        assert all("Private" not in json.dumps(m) for m in outgoing["messages"] if m["role"] == "assistant")
                        print("PASS", identifier, "replay window, reasoning events, opaque fields, edit invalidation")
                    print("90 local API replay requests passed.")
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
            for invalid in [-2, 256, 1.5, True, "3", 18446744073709551615]:
                providers[0]["reasoning"]["history_replay"] = invalid
                (work / "providers.json").write_text(json.dumps(catalog))
                rejected = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True, timeout=15,
                                          creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                assert rejected.returncode != 0, ("Accepted invalid history_replay", invalid)
                assert "history_replay" in rejected.stdout + rejected.stderr, (invalid, rejected.stdout, rejected.stderr)
            print("6 invalid replay configuration checks passed.")
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
