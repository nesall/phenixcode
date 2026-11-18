"""MCP transport, managed credentials, live rotation, and shared context checks using disposable fixtures."""
import concurrent.futures
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen


def check(value, message):
    if not value:
        raise AssertionError(message)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run_cli_credential(binary, env, project_id, action):
    proc = subprocess.run(
        [binary, "mcp-credential", "--project-id", project_id, "--action", action, "--json"],
        env=env, capture_output=True, text=True, check=False,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    return proc.returncode, json.loads(proc.stdout)


def main():
    repo = Path(__file__).resolve().parents[1]
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="phenixcode-mcp-") as directory:
        root = Path(directory)
        creds_dir = root / "mcp-creds"
        source = root / "source.cpp"
        source.write_text("// MCP_NEEDLE\nvoid context() {}\n", encoding="utf-8")
        (root / "excluded.env").write_text("SECRET_SENTINEL", encoding="utf-8")
        large = root / "large.cpp"
        large.write_text("// bounded\n" * 1000, encoding="utf-8")
        git = shutil.which("git")
        git_env = {k: v for k, v in os.environ.items() if not k.upper().startswith("GIT_")}
        git_env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0")
        for args in (("init",), ("config", "user.name", "Fixture"),
                     ("config", "user.email", "fixture@example.invalid"),
                     ("config", "core.autocrlf", "false"),
                     ("config", "commit.gpgsign", "false"), ("add", "."),
                     ("commit", "-m", "fixture")):
            subprocess.run([git, "-C", str(root), *args], env=git_env, check=True, capture_output=True)
        source.write_text(source.read_text(encoding="utf-8") + "// WORKTREE_NEEDLE\n", encoding="utf-8")

        config = json.loads((repo / "settings.json").read_text(encoding="utf-8"))
        config["source"].update(paths=[{"type": "directory", "path": str(root), "recursive": False,
                                       "extensions": [".cpp"]}], global_exclude=[], project_id="mcp-fixture")
        config["tokenizer"]["config_path"] = str(repo / "assets/bge_tokenizer.json")
        config["embedding"].update(current_api="mock", enabled_providers=["mock"])
        config["generation"].update(current_api="mock", enabled_providers=["mock"], auto_router={"enabled": False})
        config["database"].update(sqlite_path=str(root / "data.db"), index_path=str(root / "vectors.index"), vector_dim=3)
        config["logging"].update(log_to_file=True, logging_file=str(root / "core.log"), diagnostics_file=str(root / "diagnostics.log"))
        port, api_port = free_port(), free_port()
        config["mcp"] = {"enabled": True, "port": port}
        settings = root / "settings.json"
        settings.write_text(json.dumps(config), encoding="utf-8")

        # Second settings file for the same project_id on distinct ports to verify multi-instance rotation.
        port2, api_port2 = free_port(), free_port()
        config2 = json.loads(json.dumps(config))
        config2["mcp"] = {"enabled": True, "port": port2}
        settings2 = root / "settings2.json"
        settings2.write_text(json.dumps(config2), encoding="utf-8")

        providers = root / "providers.json"
        providers.write_text(json.dumps({"agent_enabled": False,
            "embedding_providers": [{"id": "mock", "model": "mock", "api_url": "http://127.0.0.1:1/embedding"}],
            "generation_providers": [{"id": "mock", "model": "mock", "api_url": "http://127.0.0.1:1/generation",
                                      "supports_tools": "off"}]}), encoding="utf-8")

        env = dict(os.environ,
                   PHENIXCODE_MCP_CREDENTIALS_DIR=str(creds_dir),
                   EMBEDDER_REGISTRY=str(root / "registry.db"),
                   EMBEDDER_METRICS=str(root / "metrics.db"))
        env.pop("PHENIXCODE_MCP_TOKEN", None)

        # CLI status before first use must report missing without creating a credential.
        rc, status_before = run_cli_credential(binary, env, "mcp-fixture", "status")
        check(rc == 0 and status_before["state"] == "missing" and not status_before["has_token"],
              "status reports missing before first use")
        check(not creds_dir.exists(), "status does not create credential directory")

        info_file = root / "info.json"
        command = [binary, "--config", str(settings), "--providers-config", str(providers),
                   "--no-startup-tests", "serve", "--yes", "--port", str(api_port), "--info-file", str(info_file)]
        command2 = [binary, "--config", str(settings2), "--providers-config", str(providers),
                    "--no-startup-tests", "serve", "--yes", "--port", str(api_port2)]

        with (root / "server.log").open("w") as log, (root / "server2.log").open("w") as log2:
            process = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            process2 = None
            try:
                last_err = None
                for _ in range(200):
                    try:
                        urlopen(f"http://127.0.0.1:{api_port}/api/health", timeout=.2).close()
                        break
                    except OSError as e:
                        last_err = repr(e)
                        if process.poll() is not None:
                            raise RuntimeError((root / "server.log").read_text())
                        time.sleep(.1)
                else:
                    core_log = (root / "core.log").read_text(encoding="utf-8", errors="replace") if (root / "core.log").exists() else ""
                    srv_log = (root / "server.log").read_text(encoding="utf-8", errors="replace") if (root / "server.log").exists() else ""
                    raise RuntimeError(f"Server did not start (poll={process.poll()}, last_err={last_err})\nserver.log:\n{srv_log}\ncore.log:\n{core_log}")

                # First startup must auto-generate a persisted token >= 32 chars.
                rc, status_after = run_cli_credential(binary, env, "mcp-fixture", "status")
                check(rc == 0 and status_after["state"] == "configured" and status_after["has_token"] and "token" not in status_after,
                      "serve auto-generates managed credential and status omits secret")
                rc, revealed = run_cli_credential(binary, env, "mcp-fixture", "reveal")
                token = revealed["token"]
                check(rc == 0 and len(token) >= 64, "revealed managed token generated from 32+ random bytes")

                def rpc(method="server/discover", params=None, headers=None, raw=None, verb="POST", target_port=port, auth_token=None, include_meta=True):
                    active_token = token if auth_token is None else auth_token
                    params = dict(params or {})
                    if include_meta:
                        params.setdefault("_meta", {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                                                    "io.modelcontextprotocol/clientCapabilities": {}})
                    body = raw if raw is not None else json.dumps({"jsonrpc": "2.0", "id": "test", "method": method, "params": params}).encode()
                    hdr = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                           "Authorization": "Bearer " + active_token, "MCP-Protocol-Version": "2026-07-28",
                           "Mcp-Method": method}
                    if "name" in params:
                        hdr["Mcp-Name"] = params["name"]
                    hdr.update(headers or {})
                    try:
                        reply = urlopen(Request(f"http://127.0.0.1:{target_port}/mcp", body if verb == "POST" else None,
                                                hdr, method=verb), timeout=20)
                    except HTTPError as e:
                        reply = e
                    with reply:
                        text = reply.read()
                        return reply.code, json.loads(text) if text else None

                check(rpc()[1]["result"]["supportedVersions"] == ["2026-07-28"], "discovery without Origin")
                check(rpc(headers={"Authorization": ""})[0] == 401, "authentication")
                check(rpc(headers={"Origin": "https://evil.example"})[0] == 403, "invalid Origin")
                check(rpc(headers={"Host": "evil.example"})[0] == 403, "invalid Host")
                check(rpc(headers={"Origin": f"http://127.0.0.1:{port}"})[0] == 200, "valid Origin")
                check(rpc(raw=b"{")[1]["error"]["code"] == -32700, "malformed JSON")
                check(rpc(raw=b"[]")[1]["error"]["code"] == -32600, "batch rejected")
                check(rpc(params={"_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}})[1]["error"]["code"] == -32602,
                      "stateless metadata requires clientCapabilities")
                check(rpc(params={"_meta": "bad"})[1]["error"]["code"] == -32602, "non-object _meta rejected")

                # Stateful / legacy client flow (e.g. Claude Desktop): initialize, notifications/initialized, ping, tools/list, tools/call.
                def legacy_rpc(method, params=None, notification=False):
                    payload = {"jsonrpc": "2.0", "method": method, "params": params or {}}
                    if not notification:
                        payload["id"] = "legacy"
                    hdr = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                           "Authorization": "bearer " + token}
                    try:
                        reply = urlopen(Request(f"http://127.0.0.1:{port}/mcp", json.dumps(payload).encode(), hdr, method="POST"), timeout=20)
                    except HTTPError as e:
                        reply = e
                    with reply:
                        text = reply.read()
                        return reply.code, json.loads(text) if text else None

                init_code, init_body = legacy_rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
                check(init_code == 200 and init_body["result"]["protocolVersion"] == "2025-06-18"
                      and "resultType" not in init_body["result"] and "_meta" not in init_body["result"],
                      "legacy initialize handshake")
                notif_code, notif_body = legacy_rpc("notifications/initialized", notification=True)
                check(notif_code == 202 and notif_body is None, "notification accepted with 202 and empty body")
                ping_code, ping_body = legacy_rpc("ping")
                check(ping_code == 200 and ping_body["result"] == {}, "legacy ping")
                legacy_list_code, legacy_list_body = legacy_rpc("tools/list")
                check(legacy_list_code == 200 and "resultType" not in legacy_list_body["result"],
                      "legacy tools/list without _meta or stateless headers")
                legacy_call_code, legacy_call_body = legacy_rpc("tools/call", {"name": "list_files", "arguments": {}})
                check(legacy_call_code == 200 and legacy_call_body["result"]["structuredContent"]["count"] == 2,
                      "legacy tools/call without Mcp-Name header")
                legacy_missing_code, legacy_missing_body = legacy_rpc("missing")
                check(legacy_missing_code == 200 and legacy_missing_body["error"]["code"] == -32601,
                      "legacy unknown method returns 200 with JSON-RPC -32601")

                check(rpc(headers={"Mcp-Method": "wrong"})[1]["error"]["code"] == -32020, "header mismatch")
                check(rpc(params={"_meta": {"io.modelcontextprotocol/protocolVersion": "1900-01-01",
                                           "io.modelcontextprotocol/clientCapabilities": {}}},
                          headers={"MCP-Protocol-Version": "1900-01-01"})[1]["error"]["code"] == -32022, "unsupported version")
                check(rpc("missing")[1]["error"]["code"] == -32601, "unknown method")
                check(rpc(raw=b"x" * 17000)[0] == 413, "body cap")
                check(rpc(verb="GET")[0] == 405, "GET unsupported")
                tools = rpc("tools/list")[1]["result"]["tools"]
                names = {t["name"] for t in tools}
                check(names == {"grep", "read_file", "read_files", "list_files", "search_context", "project_info",
                                "git_diff", "git_log", "git_show", "git_blame"}, "catalog independent of Chat flags")
                def call(name, arguments=None):
                    return rpc("tools/call", {"name": name, "arguments": arguments or {}})[1]["result"]
                info = call("project_info")["structuredContent"]
                encoded = "=?base64?" + base64.b64encode(b"project_info").decode() + "?="
                check(rpc("tools/call", {"name": "project_info"},
                          headers={"Mcp-Name": encoded})[0] == 200, "encoded name header")
                check(set(info["capabilities"]["available_tools"]) == names, "capabilities consistent")
                check(info["local_source_count"] == 2, "source allowlist")
                check("SECRET_SENTINEL" not in json.dumps(call("read_file", {"path": "excluded.env"})), "excluded file")
                check(call("read_file", {"path": "../excluded.env"})["isError"], "traversal denied")
                read = call("read_file", {"path": "source.cpp"})
                check("MCP_NEEDLE" in read["structuredContent"]["content"] and
                      json.loads(read["content"][0]["text"]) == read["structuredContent"], "text and structured results")
                check(call("read_file", {"path": "large.cpp"})["structuredContent"]["truncated"], "truncation preserved")
                check(call("grep", {"pattern": "MCP_NEEDLE"})["structuredContent"]["matches"], "grep")
                check(call("grep", {"pattern": "void\\s+context\\(", "regex": True, "glob": "*.cpp"})["structuredContent"]["matches"], "RE2 grep with glob")
                rf = call("read_files", {"files": [{"path": "source.cpp", "start_line": 1, "end_line": 1}, {"path": "source.cpp", "start_line": 2, "end_line": 2}]})["structuredContent"]
                check(rf["count"] == 2 and "MCP_NEEDLE" in rf["results"][0]["content"], "read_files")
                check(call("search_context", {"query": "MCP_NEEDLE"})["structuredContent"]["mode"] == "text", "unindexed fallback without provider")
                check("WORKTREE_NEEDLE" in call("git_diff")["structuredContent"]["content"], "read-only Git")
                check("MCP_NEEDLE" in call("git_blame", {"path": "source.cpp", "start_line": 1, "end_line": 2})["structuredContent"]["content"], "git_blame")
                check("fixture" in call("git_log", {"search": "MCP_NEEDLE", "author": "Fixture"})["structuredContent"]["content"], "git_log filters")
                check(rpc("tools/call", {"name": "project_settings"})[1]["error"]["code"] == -32602, "unadvertised tool denied")
                with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                    results = list(pool.map(lambda _: call("list_files"), range(8)))
                check(all(r["structuredContent"]["count"] == 2 for r in results), "concurrent isolated calls")

                # Start a second instance for the same project_id and verify live token rotation across both.
                process2 = subprocess.Popen(command2, cwd=root, env=env, stdout=log2, stderr=subprocess.STDOUT,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                for _ in range(200):
                    try:
                        urlopen(f"http://127.0.0.1:{api_port2}/api/health", timeout=.2).close()
                        break
                    except OSError:
                        if process2.poll() is not None:
                            raise RuntimeError((root / "server2.log").read_text())
                        time.sleep(.1)
                else:
                    raise RuntimeError("Second server did not start:\n" + (root / "server2.log").read_text(encoding="utf-8", errors="replace"))

                check(rpc(target_port=port2, auth_token=token)[0] == 200, "second instance reuses persisted token")

                rc, regen = run_cli_credential(binary, env, "mcp-fixture", "regenerate")
                check(rc == 0 and regen["generation"] == 2 and "token" not in regen, "regenerate advances generation")
                rc, revealed2 = run_cli_credential(binary, env, "mcp-fixture", "reveal")
                new_token = revealed2["token"]
                check(rc == 0 and new_token != token and len(new_token) >= 64, "regenerated token differs")

                check(rpc(target_port=port, auth_token=token)[0] == 401, "old token rejected immediately on instance 1")
                check(rpc(target_port=port2, auth_token=token)[0] == 401, "old token rejected immediately on instance 2")
                check(rpc(target_port=port, auth_token=new_token)[0] == 200, "new token accepted immediately on instance 1")
                check(rpc(target_port=port2, auth_token=new_token)[0] == 200, "new token accepted immediately on instance 2")

                # Verify credential values are absent from logs, settings, info_file, and HTTP metadata endpoints.
                instances_body = urlopen(f"http://127.0.0.1:{api_port}/api/instances", timeout=5).read().decode("utf-8")
                settings_api_body = urlopen(f"http://127.0.0.1:{api_port}/api/settings", timeout=5).read().decode("utf-8")
                stats_api_body = urlopen(f"http://127.0.0.1:{api_port}/api/stats", timeout=5).read().decode("utf-8")
                for secret in (token, new_token):
                    for label, blob in (
                        ("settings.json", settings.read_text(encoding="utf-8")),
                        ("settings2.json", settings2.read_text(encoding="utf-8")),
                        ("info_file", info_file.read_text(encoding="utf-8") if info_file.exists() else ""),
                        ("server.log", (root / "server.log").read_text(encoding="utf-8", errors="replace")),
                        ("server2.log", (root / "server2.log").read_text(encoding="utf-8", errors="replace")),
                        ("core.log", (root / "core.log").read_text(encoding="utf-8", errors="replace") if (root / "core.log").exists() else ""),
                        ("diagnostics.log", (root / "diagnostics.log").read_text(encoding="utf-8", errors="replace") if (root / "diagnostics.log").exists() else ""),
                        ("/api/instances", instances_body),
                        ("/api/settings", settings_api_body),
                        ("/api/stats", stats_api_body),
                    ):
                        check(secret not in blob, f"credential leaked in {label}")

                # Exercise the global request budget after all functional checks.
                check(any(rpc(auth_token=new_token)[0] == 429 for _ in range(305)), "rate limit")
                print("MCP HTTP integration checks PASSED")
            finally:
                for p in (process2, process):
                    if p is not None and p.poll() is None:
                        p.terminate()
                        p.wait(timeout=15)


if __name__ == "__main__":
    main()
