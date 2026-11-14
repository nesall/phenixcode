"""Local-only integration checks. Run with Python and the path to phenixcode-core.

Uses temporary project databases and a mock provider; no model or API key needed.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen


def check(value, message):
    if not value:
        raise AssertionError(message)


def main():
    repo = Path(__file__).resolve().parents[1]
    executable = Path(sys.argv[1]).resolve()
    requests = {}
    with tempfile.TemporaryDirectory(prefix="phenixcode-tools-http-") as directory:
        root = Path(directory)
        source = root / "source.cpp"
        source.write_text("// RETRIEVED_CONTEXT_SENTINEL\nvoid Needle() {}\n", encoding="utf-8")
        large = root / "large.cpp"
        large.write_text("evidence words " * 3000, encoding="utf-8")
        semantic = root / "semantic.cpp"
        removed = root / "removed.cpp"

        class Provider(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                rounds = requests.setdefault(self.path, [])
                step = len(rounds)
                rounds.append(body)
                if self.path == "/embedding":
                    texts = body["content"]
                    if any("fallback_error" in text for text in texts):
                        self.send_response(500)
                        self.end_headers()
                        self.wfile.write(b'{"error":"mock embedding failure"}')
                        return
                    def vector(text):
                        if any(marker in text for marker in ("recovery coordination", "SEMANTIC_ONLY_SENTINEL", "REMOVED_CONTEXT_SENTINEL")):
                            return [0.0, 0.0, 1.0]
                        return [1.0, 0.0, 0.0] if "Needle" in text else [0.0, 1.0, 0.0]
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(json.dumps([{"embedding": [vector(text)]} for text in texts]).encode())
                    return
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream" if body["stream"] else "application/json")
                self.end_headers()
                if self.path == "/error":
                    self.wfile.write(b'data: {"error":{"message":"mock provider failure"}}\n\n')
                    return
                if self.path == "/bad-json":
                    self.wfile.write(b"not json")
                    return
                calls = []
                if self.path == "/stream" and step < 2:
                    name, args = ("grep", {"pattern": "Needle"}) if step == 0 else ("read_file", {"path": str(source)})
                    calls = [{"id": f"call_{step}", "type": "function", "function": {"name": name, "arguments": json.dumps(args)}}]
                if self.path == "/listing" and step < 2:
                    name, args = ("list_files", {"path_filter": "source.cpp", "extension": "cpp"}) if step == 0 else ("read_file", {"path": str(source)})
                    calls = [{"id": f"list_{step}", "type": "function", "function": {"name": name, "arguments": json.dumps(args)}}]
                if self.path.startswith("/context") and step < 2:
                    query = "recovery coordination" if self.path == "/context-hybrid" else "Needle fallback_error" if self.path == "/context-fallback" else "Needle"
                    target = semantic if self.path == "/context-hybrid" else source
                    name, args = ("search_context", {"query": query}) if step == 0 else ("read_file", {"path": str(target)})
                    calls = [{"id": f"context_{step}", "type": "function", "function": {"name": name, "arguments": json.dumps(args)}}]
                if self.path in ("/budget", "/steps") and body.get("tools"):
                    calls = [{"id": f"call_{step}", "type": "function", "function": {
                        "name": "read_file", "arguments": json.dumps({"path": str(large if self.path == "/budget" else source)})}}]
                truncated = self.path == "/truncated"
                message = {"role": "assistant", "content": "I will search now" if calls or truncated else "Final answer",
                           "reasoning_content": "thinking through the evidence"}
                if calls:
                    message["tool_calls"] = calls
                reason = "length" if truncated else "tool_calls" if calls else "stop"
                usage = {"prompt_tokens": (step + 1) * 100, "completion_tokens": (step + 1) * 10,
                         "completion_tokens_details": {"reasoning_tokens": 5}}
                if body["stream"]:
                    delta = dict(message)
                    if calls:
                        delta["tool_calls"] = [dict(call, index=i) for i, call in enumerate(calls)]
                    events = [{"choices": [{"delta": delta, "finish_reason": None}]},
                              {"choices": [{"delta": {}, "finish_reason": reason}]}]
                    if self.path != "/no-usage":
                        events.append({"choices": [], "usage": usage})
                    for event in events:
                        self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())
                    self.wfile.write(b"data: [DONE]\n\n")
                else:
                    self.wfile.write(json.dumps({"choices": [{"message": message, "finish_reason": reason}], "usage": usage}).encode())

        provider = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        threading.Thread(target=provider.serve_forever, daemon=True).start()
        provider_url = f"http://127.0.0.1:{provider.server_port}"
        modes = ["stream", "plain", "error", "bad-json", "truncated", "no-usage", "budget", "steps", "listing",
                 "context-text", "context-hybrid", "context-fallback"]
        catalog = {"agent_enabled": True, "embedding_providers": [{"id": "embed", "api_url": provider_url + "/embedding", "model": "mock",
                    "query_format": "query: {}", "document_format": "document: {}"}],
                   "generation_providers": [{"id": mode, "model": mode, "api_url": provider_url + "/" + mode,
                       "stream": mode not in ("plain", "bad-json"), "supports_tools": "on",
                       "context_length": 2400 if mode == "budget" else 32000} for mode in modes]}
        config = json.loads((repo / "settings.json").read_text(encoding="utf-8"))
        config["source"].update(paths=[{"type": "directory", "path": str(root), "recursive": False, "extensions": [".cpp"]}],
                                global_exclude=[], project_id="tools-http-test")
        config["tokenizer"]["config_path"] = str(repo / "assets/bge_tokenizer.json")
        config["embedding"].update(current_api="embed", enabled_providers=["embed"])
        config["generation"].update(current_api="stream", enabled_providers=modes, auto_router={"enabled": False})
        config["database"].update(sqlite_path=str(root / "metadata.db"), index_path=str(root / "vectors.index"), vector_dim=3)
        config["logging"].update(log_to_file=False, log_to_console=True, logging_file=str(root / "core.log"), diagnostics_file=str(root / "diagnostics.log"))
        (root / "settings.json").write_text(json.dumps(config), encoding="utf-8")
        (root / "providers.json").write_text(json.dumps(catalog), encoding="utf-8")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        env = dict(os.environ, EMBEDDER_REGISTRY=str(root / "registry.sqlite"), EMBEDDER_METRICS=str(root / "metrics.sqlite"))
        with (root / "server.log").open("w", encoding="utf-8") as log:
            command = [str(executable), "--config", str(root / "settings.json"), "--providers-config",
                       str(root / "providers.json"), "--no-startup-tests"]
            def start_server():
                return subprocess.Popen(command + ["serve", "--yes", "--port", str(port)],
                cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            base = f"http://127.0.0.1:{port}"
            def wait_for_server():
                for _ in range(300):
                    try:
                        urlopen(base + "/api/health", timeout=.2).close()
                        break
                    except OSError:
                        if process.poll() is not None:
                            raise RuntimeError((root / "server.log").read_text(encoding="utf-8"))
                        time.sleep(.1)
                else:
                    raise RuntimeError("Core did not start: " + (root / "server.log").read_text(encoding="utf-8"))
            process = start_server()
            try:
                wait_for_server()
                def chat(mode, free=False):
                    request = {"targetapi": mode, "messages": [{"role": "system", "content": "You are a useful assistant."}, {"role": "user", "content": "Explain Needle"}],
                               "attachments": [], "sourceids": [str(source)], "max_tokens": 257, "ctxratio": 1.0, "free_chat": free}
                    with urlopen(Request(base + "/api/chat", json.dumps(request).encode(), {"Content-Type": "application/json"}), timeout=30) as reply:
                        text = reply.read().decode()
                    return [json.loads(line[6:]) for line in text.splitlines() if line.startswith("data: ") and line != "data: [DONE]"]

                events = chat("stream")
                check(len(requests["/stream"]) == 3, "grep/read_file require three model rounds")
                first = requests["/stream"][0]
                check(first["max_tokens"] == 257 and first["stream_options"]["include_usage"], "integer max_tokens and streaming usage reach provider")
                check("RETRIEVED_CONTEXT_SENTINEL" in first["messages"][-1]["content"], "retrieval context precedes tool decision")
                check(requests["/stream"][2]["messages"][:2] == first["messages"], "context is retained without repeated wrapping")
                check(any(e.get("content") == "Token totals (provider-reported): input 600, output 60" for e in events), "three-round provider totals are exact")
                answer = "".join(e.get("content", "") for e in events if "type" not in e)
                check(answer == "Final answer", "only final prose is published")
                events = chat("plain", free=True)
                check(any(e.get("content") == "Token totals (provider-reported): input 100, output 10" for e in events), "ordinary nonstreaming output includes reasoning exactly once")
                events = chat("no-usage", free=True)
                check(any("Token totals (estimated in part)" in e.get("content", "") for e in events), "missing usage is explicitly estimated")
                check(any("output 6" in e.get("content", "") for e in events if e.get("type") == "meta"),
                      "fallback output includes the answer and reasoning even without tools")
                for mode in ("error", "bad-json"):
                    events = chat(mode, free=True)
                    check(any("error" in e for e in events), "provider protocol failures become SSE errors")
                events = chat("truncated")
                answer = "".join(e.get("content", "") for e in events if "type" not in e)
                check("incomplete" in answer and "I will search" not in answer and len(requests["/truncated"]) == 1, "truncated tool round cannot become successful prose")
                for mode in ("budget", "steps"):
                    events = chat(mode)
                    check(any(e.get("content") == "Final answer" for e in events), "budget/step exhaustion produces final synthesis")
                    check(not requests["/" + mode][-1].get("tools"), "final synthesis offers no more tools")
                    check(sum(m["role"] == "system" for m in requests["/" + mode][-1]["messages"]) == 1,
                          "finalization extends the existing system message")
                check(len(requests["/steps"]) == 10, "last allowed round is reserved for final answer")
                events = chat("listing")
                check(len(requests["/listing"]) == 3, "list_files/read_file require three model rounds")
                listed = json.loads(requests["/listing"][1]["messages"][-1]["content"])
                check(listed["count"] == 1 and listed["files"] == [source.as_posix()], "list_files applies filters through the HTTP dispatcher")
                check(any("list_files:" in e.get("content", "") and "1 files returned" in e["content"] for e in events),
                      "list_files result summary reaches typed meta SSE")
                check(any(e.get("content") == "Final answer" for e in events), "file discovery leads to a final answer")
                events = chat("context-text")
                text_result = json.loads(requests["/context-text"][1]["messages"][-1]["content"])
                check(text_result["mode"] == "text" and any("Needle" in hit["content"] for hit in text_result["matches"]),
                      "unindexed search_context reuses text retrieval")
                check(any(e.get("content") == "Final answer" for e in events), "context search can lead to read_file and a final answer")
                # Build only this fixture's index using deterministic local embeddings.
                process.terminate()
                process.wait(timeout=10)
                semantic.write_text("// SEMANTIC_ONLY_SENTINEL\nvoid rewindOperation() {}\n", encoding="utf-8")
                removed.write_text("// REMOVED_CONTEXT_SENTINEL\nvoid rewindOther() {}\n", encoding="utf-8")
                config["generation"].update(max_chunks=1, max_full_sources=1)
                (root / "settings.json").write_text(json.dumps(config), encoding="utf-8")
                embedded = subprocess.run(command + ["embed", "--force"], cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                    timeout=60, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                check(embedded.returncode == 0 and (root / "vectors.index").exists(), "temporary fixture is indexed successfully")
                removed.unlink()  # Leave a deliberately stale entry in the fixture index.
                process = start_server()
                wait_for_server()
                for mode, expected_mode in (("context-hybrid", "hybrid"), ("context-fallback", "text")):
                    events = chat(mode)
                    result = json.loads(requests["/" + mode][1]["messages"][-1]["content"])
                    check(result["mode"] == expected_mode and result["matches"], "indexed search selects hybrid retrieval or embedding-error fallback")
                    check(all("REMOVED_CONTEXT_SENTINEL" not in hit["content"] for hit in result["matches"]), "stale sources never enter tool context")
                    check(any(e.get("content") == "Final answer" for e in events), "indexed context search completes the tool workflow")
                    check(any("search_context:" in e.get("content", "") for e in events), "context search progress reaches SSE metadata")
                hybrid = json.loads(requests["/context-hybrid"][1]["messages"][-1]["content"])
                check(any("SEMANTIC_ONLY_SENTINEL" in hit["content"] for hit in hybrid["matches"]), "semantic search finds evidence without literal query matches")
                check(any("query: recovery coordination" in req["content"] for req in requests["/embedding"]), "tool queries use the embedding provider query format")
                print("HTTP tool/context/usage/error/budget integration checks passed (12 scenarios).")
            finally:
                process.terminate()
                process.wait(timeout=10)
                provider.shutdown()
                provider.server_close()


if __name__ == "__main__":
    main()
