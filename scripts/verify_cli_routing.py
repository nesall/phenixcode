"""Verify real CLI chat routing with local mock APIs and disposable databases.

  python scripts/verify_cli_routing.py --binary build_rel/Release/phenixcode-core.exe

Requires the normal embedded classifier model beside the executable. Production
settings, credentials, databases, and model files are never modified. All API
providers in these fixtures point to the mock server on this computer.
"""

import argparse
import copy
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]


def verify(binary, tokenizer):
    calls = []

    class MockProvider(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            calls.append((self.path, body))
            status = 200
            content_type = "application/json"
            if self.path == "/embeddings":
                inputs = body.get("input", [])
                if isinstance(inputs, str):
                    inputs = [inputs]
                result = {"data": [{"index": i, "embedding": [1.0, 0.0, 0.0]} for i, _ in enumerate(inputs)]}
            elif self.path == "/classifier":
                question = body["messages"][-1]["content"]
                if question == "classifier failure":
                    status, result = 503, {"error": "Deliberate local failure"}
                else:
                    if question == "classifier timeout":
                        time.sleep(0.3)
                    tag = {"simple": "[TIER_1_SIMPLE]", "medium": "[TIER_2_MEDIUM]", "complex": "[TIER_3_COMPLEX]"}.get(question, "unknown-tier")
                    result = {"choices": [{"message": {"content": tag}}]}
            elif self.path == "/completion":
                reply = "Reply from " + body["model"]
                if body.get("stream"):
                    result = "data: " + json.dumps({"choices": [{"delta": {"content": reply}, "finish_reason": "stop"}]}) + "\n\ndata: [DONE]\n\n"
                    content_type = "text/event-stream"
                else:
                    result = {"choices": [{"message": {"content": reply}, "finish_reason": "stop"}]}
            else:
                self.send_error(404)
                return
            payload = (result if isinstance(result, str) else json.dumps(result)).encode()
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            try:
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass  # Expected for the deliberate classifier timeout.

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockProvider)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix="phenixcode-cli-routing-") as directory:
            work = Path(directory)
            mock_url = f"http://127.0.0.1:{server.server_port}"
            providers = [{"id": name, "name": name, "model": name, "api_key": "local-test-key",
                "api_style": "openai", "api_url": mock_url + "/completion", "context_length": 32000, "stream": False}
                for name in ["tier1", "tier2", "tier3", "fallback"]]
            providers.append({"id": "classifier", "model": "classifier", "api_url": mock_url + "/classifier", "api_key": "local-test-key"})
            catalog = {"generation_providers": providers, "embedding_providers": [{"id": "embedding", "model": "embedding",
                "api_url": mock_url + "/embeddings", "api_key": "local-test-key"}]}
            router = {"enabled": True, "classifier": {"type": "api", "api_id": "classifier", "timeout_ms": 100,
                "max_tokens": 20, "temperature": 0, "prompt": "Classify into one tier."},
                "fallback": {"default_model_id": "fallback"}, "routing_rules": {
                    "tier_1_simple": {"strategy": "direct", "direct_model_id": "tier1"},
                    "tier_2_medium": {"strategy": "direct", "direct_model_id": "tier2"},
                    "tier_3_complex": {"strategy": "direct", "direct_model_id": "tier3"},
                }}
            base_settings = {"tokenizer": {"config_path": str(tokenizer)}, "source": {
                "project_id": "cli-routing-test", "project_title": "Disposable CLI test", "paths": []},
                "embedding": {"current_api": "embedding"}, "chunking": {},
                "generation": {"current_api": "tier1", "enabled_providers": [p["id"] for p in providers],
                    "timeout_ms": 3000, "default_max_tokens": 128, "auto_router": router},
                "database": {"sqlite_path": str(work / "metadata.db"), "index_path": str(work / "vectors.index"), "vector_dim": 3, "max_elements": 100},
                "logging": {"log_to_console": False, "log_to_file": False,
                    "logging_file": str(work / "app.log"), "diagnostics_file": str(work / "diagnostics.log")}}
            (work / "providers.json").write_text(json.dumps(catalog), encoding="utf-8")
            env = {**os.environ, "EMBEDDER_REGISTRY": str(work / "registry.sqlite"), "EMBEDDER_METRICS": str(work / "metrics.sqlite")}

            def check_case(name, questions, *, automatic=True, internal=False, streaming=False, executable=binary, expected=None):
                settings = copy.deepcopy(base_settings)
                settings["generation"]["auto_router"]["enabled"] = automatic
                settings["generation"]["auto_router"]["classifier"]["type"] = "internal" if internal else "api"
                settings["generation"]["current_api"] = "tier2"
                (work / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
                provider_config = copy.deepcopy(catalog)
                for provider in provider_config["generation_providers"]:
                    if provider["id"] != "classifier":
                        provider["stream"] = streaming
                (work / "providers.json").write_text(json.dumps(provider_config), encoding="utf-8")
                calls.clear()
                command = [str(executable), "--config", str(work / "settings.json"), "--providers-config", str(work / "providers.json"),
                    "--no-startup-tests", "chat"]
                process = subprocess.Popen(command, cwd=work, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                try:
                    output, _ = process.communicate("\n".join(questions + ["exit", ""]), timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    output, _ = process.communicate(timeout=5)
                    raise AssertionError(name + " timed out:\n" + output)
                assert process.returncode == 0, name + " exited unsuccessfully:\n" + output
                generated = [body for path, body in calls if path == "/completion"]
                classified = [body for path, body in calls if path == "/classifier"]
                models = [body["model"] for body in generated]
                assert len(generated) == len(questions), name + " lost a turn:\n" + output
                assert len(classified) == (len(questions) if automatic and not internal else 0), name + " used the wrong classifier:\n" + output
                if expected is not None:
                    assert models == expected, f"{name}: expected {expected}, received {models}\n{output}"
                for index, body in enumerate(generated):
                    history = body["messages"]
                    assert history[0] == {"role": "system", "content": "You are a helpful assistant."}
                    assert history[-1]["role"] == "user" and questions[index] in history[-1]["content"]
                    assert len([m for m in history if m["role"] == "user"]) == index + 1
                    assert len([m for m in history if m["role"] == "assistant"]) == index
                    assert body["stream"] == streaming
                    assert body["max_tokens"] == 128
                    assert "reasoning" not in body and "thinking" not in body, "Routing refactor introduced reasoning behavior"
                    assert "Reply from " + models[index] in output
                if automatic:
                    assert output.count("[router] Auto routed to model ") == len(questions), output
                else:
                    assert "Using model: tier2" in output and "[router]" not in output
                assert "Exiting chat mode." in output
                print("PASS", name, ", ".join(models))
                return models, output

            check_case("manual model bypasses classifier", ["simple", "complex"], automatic=False, expected=["tier2", "tier2"])
            check_case("API classifier selects all tiers", ["simple", "medium", "complex"], expected=["tier1", "tier2", "tier3"])
            check_case("API classifier failure uses fallback", ["classifier failure"], expected=["fallback"])
            check_case("API classifier timeout uses fallback", ["classifier timeout"], expected=["fallback"])
            check_case("unknown classifier tier uses fallback", ["unrecognized"], expected=["fallback"])
            check_case("streaming completion preserves conversation", ["simple", "medium"], streaming=True, expected=["tier1", "tier2"])
            models, output = check_case("internal classifier preserves session context", [
                "Design a distributed architecture for a fault-tolerant multi-region platform, including consensus, sharding, replication, failover, and migration across multiple services.",
                "...",
            ], internal=True)
            assert len(re.findall(r"\[router\] Internal router classified task as tier_[123]_", output)) == 2, output
            assert models[0] in ("tier2", "tier3"), "Context test needs a non-simple initial tier:\n" + output
            assert models[1] == models[0], "Follow-up failed to inherit the conversation tier:\n" + output

            # Exercise the missing-model fallback using a disposable executable copy.
            # Never rename, remove, or replace the user's deployed model.
            isolated = work / "without-model"
            isolated.mkdir()
            copied_binary = Path(shutil.copy2(binary, isolated / binary.name))
            for library in binary.parent.glob("*.dll"):
                shutil.copy2(library, isolated / library.name)
            check_case("unavailable internal classifier uses fallback", ["complex"], internal=True, executable=copied_binary, expected=["fallback"])
            print("8 CLI routing scenarios passed.")
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
