"""Local-only integration checks. Run with Python and the path to phenixcode-core.

Uses temporary project databases and a mock provider; no model or API key needed.
"""
import json
import os
import shutil
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
        git = shutil.which("git")
        if not git:
            raise RuntimeError("Git is required for the HTTP Git tool fixtures")
        def fixture_git(*args):
            return subprocess.run([git, "-C", str(root), *args], check=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, timeout=20, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        fixture_git("init", "-b", "main")
        fixture_git("config", "user.name", "Fixture")
        fixture_git("config", "user.email", "fixture@example.invalid")
        fixture_git("config", "core.autocrlf", "false")
        fixture_git("config", "commit.gpgsign", "false")
        (root / "excluded.env").write_text("HTTP_GIT_SECRET original\n", encoding="utf-8")
        fixture_git("add", "source.cpp", "excluded.env")
        fixture_git("commit", "-m", "initial HTTP Git fixture")
        source.write_text(source.read_text(encoding="utf-8") + "// GIT_COMMITTED_SENTINEL\n", encoding="utf-8")
        fixture_git("add", "source.cpp")
        fixture_git("commit", "-m", "second HTTP Git fixture")
        source.write_text(source.read_text(encoding="utf-8") + "// GIT_WORKTREE_SENTINEL\n", encoding="utf-8")
        (root / "excluded.env").write_text("HTTP_GIT_SECRET modified\n", encoding="utf-8")
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
                if self.path == "/batch" and step == 0:
                    calls = [{"id": "first", "type": "function", "function": {"name": "grep", "arguments": json.dumps({"pattern": "Needle", "path_filter": "missing/"})}},
                             {"id": "second", "type": "function", "function": {"name": "grep", "arguments": json.dumps({"pattern": "Needle(", "path_filter": str(source)})}}]
                if self.path == "/project" and step < 2:
                    name, args = ("project_info", {}) if step == 0 else ("project_settings", {"section": "all"})
                    calls = [{"id": f"project_{step}", "type": "function", "function": {"name": name, "arguments": json.dumps(args)}}]
                if self.path == "/grep-re2" and step == 0:
                    calls = [{"id": "re2_0", "type": "function", "function": {"name": "grep", "arguments": json.dumps(
                        {"pattern": "void\\s+Needle\\(", "regex": True, "case_sensitive": True, "context_lines": 0, "glob": "*.cpp"})}}]
                if self.path == "/read-files" and step == 0:
                    calls = [{"id": "rf_0", "type": "function", "function": {"name": "read_files", "arguments": json.dumps(
                        {"files": [{"path": str(source), "start_line": 1, "end_line": 1}, {"path": str(source), "start_line": 2, "end_line": 2}]})}}]
                if self.path.startswith("/git-") and step == 0:
                    name = "git_diff" if self.path in ("/git-branch", "/git-context") else "git_log" if self.path == "/git-log-search" else self.path[1:].replace("-", "_")
                    args = {"path": str(source)} if name == "git_diff" else {"limit": 1} if name == "git_log" else {"path": str(source), "start_line": 1, "end_line": 4} if name == "git_blame" else {"revision": "HEAD", "path": str(source)}
                    if self.path == "/git-log-search":
                        args = {"search": "GIT_COMMITTED_SENTINEL", "author": "Fixture"}
                    if self.path == "/git-branch":
                        args = {"base": "HEAD~1", "path": str(source)}
                    if self.path == "/git-context":
                        args = {"context": 0, "path": str(source)}
                    calls = [{"id": "git_call", "type": "function", "function": {"name": name, "arguments": json.dumps(args)}}]
                if self.path == "/listing" and step < 2:
                    name, args = ("list_files", {"path_filter": "source.cpp", "extension": "cpp"}) if step == 0 else ("read_file", {"path": str(source)})
                    calls = [{"id": f"list_{step}", "type": "function", "function": {"name": name, "arguments": json.dumps(args)}}]
                if self.path in ("/ranked-listing", "/ranked-unindexed") and step == 0:
                    calls = [{"id": "ranked", "type": "function", "function": {"name": "list_files", "arguments": json.dumps({"sort_by": "lines", "order": "desc", "limit": 20})}}]
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
                if self.path == "/empty":
                    message["content"] = ""
                if calls:
                    message["tool_calls"] = calls
                reason = "length" if truncated else "tool_calls" if calls else "stop"
                usage = {"prompt_tokens": (step + 1) * 100, "completion_tokens": (step + 1) * 10,
                         "completion_tokens_details": {"reasoning_tokens": 5}}
                if self.path == "/no-reasoning-usage":
                    message.pop("reasoning_content")
                    usage.pop("completion_tokens_details")
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
        modes = ["stream", "plain", "error", "bad-json", "truncated", "no-usage", "budget", "steps", "listing", "ranked-listing", "ranked-unindexed",
                 "context-text", "context-hybrid", "context-fallback", "project", "git-diff", "git-log", "git-show", "git-branch", "git-context",
                 "git-blame", "git-log-search", "grep-re2", "read-files", "batch", "no-reasoning-usage", "empty"]
        catalog = {"agent_enabled": True, "embedding_providers": [{"id": "embed", "api_url": provider_url + "/embedding", "model": "mock",
                    "query_format": "query: {}", "document_format": "document: {}"}],
                   "generation_providers": [{"id": mode, "model": mode, "api_url": provider_url + "/" + mode,
                       "stream": mode not in ("plain", "bad-json"), "supports_tools": "on",
                       "context_length": 4096 if mode == "budget" else 32000} for mode in modes]}
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
                starts = [event for event in events if event.get("status") == "running" and event.get("event_id", "").startswith("round:")]
                check(starts and all(event.get("summary") == "Generating response..." for event in starts), "generation labels are independent of round numbering")
                reads = [event for event in events if event.get("tool") == "read_file" and event.get("status") != "running"]
                check(reads and "Read source.cpp:" in reads[0]["summary"] and str(root) not in reads[0]["summary"] and str(source).replace("\\", "/") not in reads[0]["summary"], "read summaries use configured-source-relative paths")
                first = requests["/stream"][0]
                check(first["max_tokens"] == 257 and first["stream_options"]["include_usage"], "integer max_tokens and streaming usage reach provider")
                check("RETRIEVED_CONTEXT_SENTINEL" in first["messages"][-1]["content"], "retrieval context precedes tool decision")
                check(requests["/stream"][2]["messages"][:2] == first["messages"], "context is retained without repeated wrapping")
                check(any(e.get("content") == "Token totals (provider-reported): input 600, output 60" for e in events), "three-round provider totals are exact")
                answer = "".join(e.get("content", "") for e in events if "type" not in e)
                check(answer == "Final answer", "only final prose is published")
                check(any(e.get("content") == "Reasoning tokens across all rounds: 15" for e in events), "reasoning usage is totaled across model rounds")
                events = chat("batch")
                tool_events = [event for event in events if event.get("tool") == "grep"]
                check(len(tool_events) == 4 and [event["call_index"] for event in tool_events] == [1, 1, 2, 2] and
                      all(event["round"] == 1 for event in tool_events), "batched tools share a round but have separate call numbers")
                check(tool_events[0]["event_id"] == tool_events[1]["event_id"] and tool_events[2]["event_id"] == tool_events[3]["event_id"], "call results update their own start identity")
                check(tool_events[1]["status"] == "info" and tool_events[1]["outcome"] == "no_files_matched_filter" and
                      tool_events[3]["searched_files"] == 1 and tool_events[3]["status"] == "success", "grep distinguishes empty scope from a successful searched scope")
                check('path_filter="missing/"' in tool_events[0]["content"] and 'pattern="Needle("' in tool_events[2]["content"], "literal patterns and path filters are visible in streaming metadata")
                check("path_filter=" not in tool_events[0]["summary"] and "in missing/" in tool_events[0]["summary"], "compact tool summary retains custom scope without parameter names")
                check("1/1 files" not in tool_events[3]["summary"] and "1 files" in tool_events[3]["summary"], "complete searches omit redundant denominator counts")
                check(not any("Model stopped with reason: tool_calls" in event.get("content", "") for event in events), "normal tool handoff is not presented as a stopped model")
                events = chat("no-reasoning-usage", free=True)
                check(any(event.get("content") == "Reasoning usage unavailable for all rounds" for event in events), "missing reasoning breakdown is not presented as zero")
                events = chat("empty", free=True)
                check(any(event.get("status") == "warning" and "no answer text returned" in event.get("content", "") for event in events), "empty answers do not receive a successful response status")
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
                check(len(requests["/steps"]) == 15, "fifteenth round is reserved for final answer after fourteen tool rounds")
                check(all(request.get("tools") for request in requests["/steps"][:-1]), "tools remain available through round fourteen")
                check(1 < len(requests["/budget"]) < 15, "small context fits all tool schemas but finalizes before the step limit")
                events = chat("listing")
                check(len(requests["/listing"]) == 3, "list_files/read_file require three model rounds")
                listed = json.loads(requests["/listing"][1]["messages"][-1]["content"])
                check(listed["count"] == 1 and listed["files"] == [source.as_posix()], "list_files applies filters through the HTTP dispatcher")
                check(any(e.get("tool") == "list_files" and e.get("status") != "running" and "1 files returned" in e["content"] for e in events),
                      "list_files result summary reaches typed meta SSE")
                chat("ranked-unindexed")
                unindexed = json.loads(requests["/ranked-unindexed"][1]["messages"][-1]["content"])
                check(unindexed["count"] == 2 and all(item["line_count"] is None and item["size_bytes"] is None for item in unindexed["file_metadata"]),
                      "unindexed ranking returns allowed files with unknown metrics")
                check(any(e.get("content") == "Final answer" for e in events), "file discovery leads to a final answer")
                events = chat("project")
                check(len(requests["/project"]) == 3, "project inspection completes a full tool workflow")
                offered = {tool["function"]["name"] for tool in requests["/project"][0]["tools"]}
                check({"project_info", "project_settings"} <= offered, "both project tools are offered to the model")
                project_info = json.loads(requests["/project"][1]["messages"][-1]["content"])
                project_settings = json.loads(requests["/project"][2]["messages"][-1]["content"])
                check(set(project_info["capabilities"]["available_tools"]) == offered, "project info tool list matches request definitions")
                check(project_settings["retrieval"]["database"]["sqlite_filename"] == "metadata.db" and
                      str(root) not in json.dumps(project_settings["retrieval"]["database"]), "database directories are omitted")
                check(project_info["project"]["id"] == "tools-http-test" and project_info["local_source_count"] == 2,
                      "project info uses current project and allowed local files")
                check(project_settings["models"]["selected_generation"]["id"] == "project" and
                      project_settings["models"]["configured_generation_id"] == "stream", "settings reflect request provider override")
                check(provider_url not in json.dumps(project_settings), "settings omit provider endpoints")
                check(any(event.get("tool") == "project_info" and "loaded summary" in event.get("content", "") for event in events) and
                      any(event.get("tool") == "project_settings" and "loaded summary" in event.get("content", "") for event in events), "project tool progress reaches SSE")
                check(any(e.get("content") == "Final answer" for e in events), "inspection evidence produces final answer")
                for mode, expected in (("git-diff", "GIT_WORKTREE_SENTINEL"), ("git-log", "second HTTP Git fixture"), ("git-show", "void Needle"), ("git-branch", "GIT_COMMITTED_SENTINEL"), ("git-context", "GIT_WORKTREE_SENTINEL"), ("git-blame", "GIT_WORKTREE_SENTINEL"), ("git-log-search", "second HTTP Git fixture")):
                    events = chat(mode)
                    check(len(requests["/" + mode]) == 2, "Git tool result reaches a second model round")
                    git_result = json.loads(requests["/" + mode][1]["messages"][-1]["content"])
                    check(expected in git_result["content"] and "HTTP_GIT_SECRET" not in git_result["content"], "Git HTTP dispatcher enforces source scope")
                    expected_tool = "git_diff" if mode in ("git-branch", "git-context") else "git_log" if mode == "git-log-search" else mode.replace("-", "_")
                    check(any(event.get("tool") == expected_tool and "inspection returned" in event.get("content", "") for event in events), "Git progress reaches typed SSE metadata")
                    if mode == "git-context":
                        check(git_result["context"] == 0 and "RETRIEVED_CONTEXT_SENTINEL" not in git_result["content"], "zero-context patch reaches the next model round without surrounding lines")
                    if mode == "git-branch":
                        check(git_result["mode"] == "branch" and "GIT_WORKTREE_SENTINEL" not in git_result["content"], "branch review excludes uncommitted worktree changes")
                    if mode == "git-blame":
                        check(git_result["start_line"] == 1 and git_result["end_line"] == 4, "git_blame line range reaches model round")
                    if mode == "git-log-search":
                        check("initial HTTP Git fixture" not in git_result["content"], "git_log pickaxe/author filter excludes non-matching commits")
                    check(any(event.get("content") == "Final answer" for event in events), "model completes answer after Git inspection")
                events = chat("grep-re2")
                re2_result = json.loads(requests["/grep-re2"][1]["messages"][-1]["content"])
                check(len(re2_result["matches"]) == 1 and re2_result["matches"][0]["start_line"] == 2 and
                      re2_result["matches"][0]["end_line"] == 2 and re2_result["glob"] == "*.cpp",
                      "RE2 regex, zero context_lines, and glob filter work through HTTP dispatcher")
                events = chat("read-files")
                rf_result = json.loads(requests["/read-files"][1]["messages"][-1]["content"])
                check(rf_result["count"] == 2 and rf_result["results"][0]["start_line"] == 1 and rf_result["results"][1]["start_line"] == 2,
                      "read_files batch reads disjoint ranges through HTTP dispatcher")
                check(any(event.get("tool") == "read_files" and "2/2 files returned" in event.get("content", "") for event in events),
                      "read_files progress reaches typed SSE metadata")
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
                events = chat("ranked-listing")
                ranked = json.loads(requests["/ranked-listing"][1]["messages"][-1]["content"])
                metadata = {item["source"]: item for item in ranked["file_metadata"]}
                check(metadata[source.as_posix()]["size_bytes"] == source.stat().st_size and metadata[source.as_posix()]["line_count"] == 4,
                      "ranked listing reads persisted indexed size and line metadata through the dispatcher")
                check(removed.as_posix() not in metadata and ranked["metadata_source"] == "index" and ranked["metadata_freshness"] == "unknown",
                      "deleted indexed sources are omitted and snapshot freshness is explicit")
                check(any(event.get("tool") == "list_files" and "by lines desc" in event.get("summary", "") for event in events),
                      "ranked listing exposes compact sorting information in progress")
                for mode, expected_mode in (("context-hybrid", "hybrid"), ("context-fallback", "text")):
                    events = chat(mode)
                    result = json.loads(requests["/" + mode][1]["messages"][-1]["content"])
                    check(result["mode"] == expected_mode and result["matches"], "indexed search selects hybrid retrieval or embedding-error fallback")
                    check(all("REMOVED_CONTEXT_SENTINEL" not in hit["content"] for hit in result["matches"]), "stale sources never enter tool context")
                    check(any(e.get("content") == "Final answer" for e in events), "indexed context search completes the tool workflow")
                    check(any(e.get("tool") == "search_context" for e in events), "context search progress reaches SSE metadata")
                hybrid = json.loads(requests["/context-hybrid"][1]["messages"][-1]["content"])
                check(any("SEMANTIC_ONLY_SENTINEL" in hit["content"] for hit in hybrid["matches"]), "semantic search finds evidence without literal query matches")
                check(any("query: recovery coordination" in req["content"] for req in requests["/embedding"]), "tool queries use the embedding provider query format")
                print("HTTP tool/context/usage/error/budget integration checks passed (23 scenarios).")
            finally:
                process.terminate()
                process.wait(timeout=10)
                provider.shutdown()
                provider.server_close()


if __name__ == "__main__":
    main()
