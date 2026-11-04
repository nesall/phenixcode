"""Capture real /api/chat provider requests using only disposable local fixtures.

Run from the repository root:
  python scripts/verify_reasoning_serialization.py --binary build_rel/Release/phenixcode-core.exe

No production configuration or provider credentials are read. All writable data,
including the instance registry, lives in a temporary directory. No external API
is called. Production request construction and endpoint code remain unchanged.
"""

import argparse
import copy
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
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def request_json(url, body=None, headers=None):
    request = urllib.request.Request(
        url, data=None if body is None else json.dumps(body).encode(),
        headers={"Content-Type": "application/json", **(headers or {})},
    )
    with HTTP.open(request, timeout=20) as response:
        return response.read().decode()


def verify(binary, tokenizer):
    captured = queue.Queue()

    class MockProvider(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/embeddings":
                inputs = body.get("input", [])
                if isinstance(inputs, str):
                    inputs = [inputs]
                result = {"data": [{"index": i, "embedding": [1.0, 0.0, 0.0]} for i, _ in enumerate(inputs)]}
                content, content_type = json.dumps(result), "application/json"
            elif self.path == "/chat/completions":
                captured.put(body)
                if body.get("stream"):
                    chunk = {"choices": [{"delta": {"content": "Mock response"}, "finish_reason": "stop"}]}
                    content, content_type = "data: " + json.dumps(chunk) + "\n\ndata: [DONE]\n\n", "text/event-stream"
                else:
                    result = {"choices": [{"message": {"role": "assistant", "content": "Mock response"}, "finish_reason": "stop"}]}
                    content, content_type = json.dumps(result), "application/json"
            else:
                self.send_error(404)
                return
            payload = content.encode()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockProvider)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix="phenixcode-reasoning-") as directory:
            work = Path(directory)
            mock_url = f"http://127.0.0.1:{server.server_port}"
            parameters = [
                {"name": "reasoning_effort", "value_type": "scalar", "values": ["low", "high", "max"], "default": "low"},
                {"name": "thinking", "value_type": "object", "values": ["enabled", "disabled"], "default": "enabled", "value_field": "type"},
            ]
            providers = []
            for streaming in [False, True]:
                providers.append({"id": "streaming" if streaming else "non-streaming", "name": "Local verification",
                    "api_url": mock_url + "/chat/completions", "api_key": "local-test-key", "api_style": "openai",
                    "model": "deepseek-flash", "context_length": 32000, "stream": streaming,
                    "reasoning": {"parameters": copy.deepcopy(parameters)}})
            extended = copy.deepcopy(providers[0])
            extended["id"] = "extended"
            extended["reasoning"]["parameters"][1]["constant_fields"] = {"fixture": "preserved", "type": "must-be-overridden"}
            extended["reasoning"]["parameters"].append({"name": "extra_parameter", "value_type": "scalar",
                "values": ["first", "second"], "default": "first"})
            providers.append(extended)
            unsupported = copy.deepcopy(providers[0])
            unsupported["id"] = "unsupported"
            unsupported.pop("reasoning")
            providers.append(unsupported)
            catalog = {"generation_providers": providers, "embedding_providers": [{
                "id": "local-embedding", "api_url": mock_url + "/embeddings", "api_key": "local-test-key", "model": "local-embedding",
            }]}
            settings = {
                "tokenizer": {"config_path": str(tokenizer)},
                "embedding": {"current_api": "local-embedding"},
                "generation": {"current_api": "non-streaming", "enabled_providers": [p["id"] for p in providers],
                    "auto_router": {"enabled": False}, "timeout_ms": 5000, "default_max_tokens": 128, "excerpt": {"enabled": False}},
                "source": {"project_id": "reasoning-verification", "project_title": "Disposable reasoning test", "paths": []},
                "chunking": {},
                "database": {"sqlite_path": str(work / "metadata.db"), "index_path": str(work / "vectors.index"),
                    "vector_dim": 3, "max_elements": 100},
                "logging": {"log_to_console": False, "log_to_file": False,
                    "logging_file": str(work / "app.log"), "diagnostics_file": str(work / "diagnostics.log")},
            }
            for name, data in [("providers.json", catalog), ("settings.json", settings)]:
                (work / name).write_text(json.dumps(data), encoding="utf-8")
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            info = work / "instance.json"
            command = [str(binary), "--config", str(work / "settings.json"), "--providers-config", str(work / "providers.json"),
                "--no-startup-tests", "serve", "--yes", "--port", str(port), "--info-file", str(info), "--appkey", "local-verification-key"]
            env = {**os.environ, "EMBEDDER_REGISTRY": str(work / "registry.sqlite"),
                "EMBEDDER_METRICS": str(work / "metrics.sqlite")}
            base = None
            with (work / "process.log").open("w", encoding="utf-8") as log:
                process = subprocess.Popen(command, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                try:
                    deadline = time.monotonic() + 25
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            raise RuntimeError("Core exited during startup:\n" + (work / "process.log").read_text(encoding="utf-8", errors="replace"))
                        try:
                            actual_port = json.loads(info.read_text())["port"]
                            candidate = f"http://127.0.0.1:{actual_port}"
                            request_json(candidate + "/api/health")
                            base = candidate
                            break
                        except (OSError, ValueError, KeyError, urllib.error.URLError):
                            time.sleep(0.05)
                    if base is None:
                        raise RuntimeError("Core did not start in time:\n" + (work / "process.log").read_text(encoding="utf-8", errors="replace"))

                    def check_case(provider, label, selected, expected):
                        payload = {"messages": [{"role": "user", "content": "Verify serialization"}], "targetapi": provider,
                            "attachments": [{"filename": "fixture.txt", "content": "Local test context."}], "sourceids": [],
                            "attachedonly": True, "temperature": 0.2, "session_id": label}
                        if selected is not None:
                            payload["reasoning"] = selected
                        response = request_json(base + "/api/chat", payload)
                        if "Mock response" not in response:
                            raise AssertionError("No mock response for " + label + ": " + response)
                        outgoing = captured.get(timeout=3)
                        actual = {key: outgoing[key] for key in ["thinking", "reasoning_effort", "extra_parameter"] if key in outgoing}
                        if actual != expected:
                            raise AssertionError(f"{label}: expected {expected}, received {actual}")
                        assert "reasoning" not in outgoing, "Internal reasoning envelope leaked to provider"
                        assert "unknown_parameter" not in outgoing, "Unconfigured parameter leaked to provider"
                        assert outgoing["model"] == "deepseek-flash"
                        assert outgoing["stream"] == (provider == "streaming")
                        print("PASS", label, json.dumps(actual, sort_keys=True))

                    for provider in ["non-streaming", "streaming"]:
                        check_case(provider, provider + "/enabled-high", {"thinking": "enabled", "reasoning_effort": "high"},
                            {"thinking": {"type": "enabled"}, "reasoning_effort": "high"})
                        check_case(provider, provider + "/disabled-max", {"thinking": "disabled", "reasoning_effort": "max"},
                            {"thinking": {"type": "disabled"}, "reasoning_effort": "max"})
                        check_case(provider, provider + "/defaults", None,
                            {"thinking": {"type": "enabled"}, "reasoning_effort": "low"})
                        check_case(provider, provider + "/invalid-and-unknown", {"thinking": "invalid", "reasoning_effort": "invalid", "unknown_parameter": "ignored"},
                            {"thinking": {"type": "enabled"}, "reasoning_effort": "low"})
                    check_case("extended", "constant-fields-and-third-parameter",
                        {"thinking": "disabled", "reasoning_effort": "high", "extra_parameter": "second"},
                        {"thinking": {"type": "disabled", "fixture": "preserved"}, "reasoning_effort": "high", "extra_parameter": "second"})
                    check_case("unsupported", "provider-without-reasoning", {"thinking": "enabled", "reasoning_effort": "high"}, {})
                    print("10 outgoing serialization checks passed.")
                finally:
                    if process.poll() is None and base:
                        try:
                            request_json(base + "/api/shutdown", {}, {"X-App-Key": "local-verification-key"})
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
        worker.join(timeout=3)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--tokenizer", type=Path, default=ROOT / "assets" / "bge_tokenizer.json")
    arguments = parser.parse_args()
    binary, tokenizer = arguments.binary.resolve(), arguments.tokenizer.resolve()
    if not binary.is_file() or not tokenizer.is_file():
        parser.error("The core executable and tokenizer must exist.")
    verify(binary, tokenizer)
