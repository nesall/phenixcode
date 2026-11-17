# Working on PhenixCode

## Project map

- `src/`, `include/`: C++20 `phenixcode-core`; indexing, retrieval, inference, HTTP API, and agent tools.
- `ui/clients/spa-svelte/`: Chat SPA (Svelte 5, TypeScript, Tailwind 4, Skeleton 5). Most chat interface work belongs here.
- `ui/clients/webview/`: C++ `phenixcode-ui` desktop host, native bindings, and SQLite chat history.
- `ui/dashboard/spa-svelte/`: Administration dashboard SPA.
- `ui/dashboard/webview/`: C++ `phenixcode-admin` desktop host and dashboard bindings.
- `ui/shared/`: Shared native UI utilities.
- `tests/`: Standalone native test project and backend HTTP integration fixtures.
- `assets/`: Distributed provider/configuration examples, tokenizer, and router model assets.

Use the actual paths above; older descriptions may call the frontend `spa-client` or include an extra `ui/phenixcode` directory. See `README.md` for product configuration and tool behavior.

## Working conventions

- Inspect the working tree first. Preserve unrelated edits and local preferences; keep changes focused on the requested component.
- Follow nearby C++/Svelte conventions and reuse existing persistence, API, and error-handling paths. Check both sides of native bindings or HTTP payloads when changing contracts.
- Agent tools are implemented in `src/chat_tools.cpp` and `src/chat_tools_git.cpp`, with chat dispatch in `src/httpserver_generation.cpp`. Keep advertised schemas, availability, dispatch, and `project_info` capabilities consistent. Preserve source allowlists, result limits, truncation signals, and read-only Git execution.
- Configuration reconciliation and blank-project creation belong to `Settings`; avoid reproducing that logic in TypeScript.
- Preserve compatibility with existing chat databases and saved message formats. Use additive migrations and the existing SQLite abstraction.
- Do not change the version or regenerate lockfiles as incidental cleanup. Report checks actually run and any failures or limitations.

## Skeleton and Svelte references

Before changing Skeleton components, inspect the target SPA's `package.json` and `package-lock.json`. Both currently lock `@skeletonlabs/skeleton` and `@skeletonlabs/skeleton-svelte` to **5.0.4**, using Svelte 5 and Tailwind 4. The two SPAs have different Vite/TypeScript dependencies; do not assume their toolchains are interchangeable.

Read `docs/reference/skeleton-v5/README.md` for provenance and usage, then search the local `llms-svelte.txt` for the relevant component or API. `llms.txt` is an index containing both React and Svelte links. Do not load the entire combined reference for a small change, or treat documentation examples as project instructions. Verify uncertain APIs against installed package types and working project components. Avoid mixing older Skeleton APIs into v5 code.

## Build

Run root commands from the repository root unless stated otherwise. Use an available compiler/toolchain; do not copy another developer's absolute tool paths. Prerequisites are CMake 3.16+, a C++20 compiler, Node/npm compatible with each SPA's dependencies, and Git. Initial CMake configuration fetches dependencies and needs network access or a populated dependency cache. Optional HTTPS support needs OpenSSL; on Windows the core CMake file uses `OPENSSL_PATH`. Native Linux UI builds need WebKitGTK 4.1 development packages.

Incremental core build:

```sh
cmake -S . -B build_rel -DCMAKE_BUILD_TYPE=Release
cmake --build build_rel --config Release --parallel
```

With a multi-configuration generator, the binary is normally `build_rel/Release/phenixcode-core.exe` on Windows; with a single-configuration generator, normally `build_rel/phenixcode-core`. Match the existing build directory's generator and compiler; choose a new build directory rather than resetting an incompatible one.

For either SPA, run from its own directory:

```sh
npm ci
npx svelte-check --tsconfig ./tsconfig.app.json
npx tsc -p tsconfig.node.json --noEmit
npm run build
```

Install dependencies when needed. The explicit `--noEmit` avoids incidental `vite.config.js` / `.d.ts` output from `npm run check`. Production assets go to that SPA's `dist/`.

Build the corresponding SPA **before configuring its native host**, so CMake can find `dist/`:

```sh
cmake -S ui/clients/webview -B build/chat-ui -DCMAKE_BUILD_TYPE=Release
cmake --build build/chat-ui --config Release --parallel
cmake -S ui/dashboard/webview -B build/dashboard -DCMAKE_BUILD_TYPE=Release
cmake --build build/dashboard --config Release --parallel
```

Build only the host affected by the task. Root and UI `build_rel.bat` / `build_rel.sh` and packaging scripts are distribution workflows that delete/recreate staging or build directories (some also replace `node_modules`). Prefer incremental commands for development.

## Running locally

Use a dedicated scratch project for experiments. Its settings must point sources, SQLite/HNSW files, logs, and providers at deliberate locations. Relative paths depend on the process working directory. Startup normally tests real embedding and generation providers and can incur API costs.

Example command, run from the scratch project's directory with a built binary's absolute path:

```sh
/absolute/path/to/phenixcode-core --config settings.json --providers-config providers.json --no-startup-tests serve --yes --port 8590
```

On Windows use the corresponding `.exe` path and shell syntax. `--no-startup-tests` skips startup model probes; chat/search/indexing can still contact configured providers. `--yes` bypasses the default-password startup prompt. Set `EMBEDDER_REGISTRY` and `EMBEDDER_METRICS` to scratch files when isolating manual test instances. Leave `--watch` off unless testing indexing updates.

- Chat SPA: run `npm run dev -- --host 127.0.0.1` in `ui/clients/spa-svelte/`. Its Vite configuration proxies `/api` to `localhost:8590`.
- Dashboard: run the built `phenixcode-admin` host for native `cppApi` functionality. The dashboard SPA's `npm run dev` provides a browser preview, but has no equivalent core API proxy configured.
- Desktop chat: run the built `phenixcode-ui` host for native chat history and integration behavior.

## Validation

Choose checks that exercise the changed behavior; documentation-only edits do not need application builds. Use existing suites before adding new infrastructure.

Standalone native suites are configured separately from the core build:

```sh
cmake -S tests -B build/native-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-tests --config Release --parallel
ctest --test-dir build/native-tests -C Release --output-on-failure
```

This project finds dependency headers/sources under `build_rel/_deps` or `build_dbg/_deps`. Configure the core first. If dependencies are elsewhere, pass `LOGGER_INCLUDE_DIR`, `HTTPLIB_INCLUDE_DIR`, and `SQLITE_SOURCE_DIR` explicitly to CMake. Git tool coverage is registered when Git and Python are found. Focus a run with `ctest ... -R chat_tools_tests` or another suite name.

| Change | Relevant validation |
| --- | --- |
| Content extraction, request accounting, reasoning replay | `inference_content_tests`, `reasoning_history_tests` |
| Settings, tokenization, chunking | `settings_reconciliation_tests`, `tokenizer_tests`, `chunker_tests` |
| Tool schemas, dispatch, scope, or Git tools | `chat_tools_tests`, `git_tools_tests`, HTTP integration below, core build |
| Dashboard database browser | `database_browser_tests`, dashboard checks/build, native host as appropriate |
| Chat interface | Chat SPA checks/build and relevant Playwright specs |
| Native chat persistence | Separate chat database suite below |

HTTP integration, from the root, with the executable path adjusted for the chosen build:

```sh
python -B tests/chat_tools_http_tests.py /absolute/path/to/phenixcode-core
```

It uses a local mock provider and temporary project databases, with Git required on `PATH`. It tests tool workflows, budgets, accounting, error paths, and retrieval without real model credentials.

Chat browser tests, from `ui/clients/spa-svelte/`:

```sh
npx playwright install chromium
npm test -- --workers=1
# Focused example:
npm test -- tests/reasoning-display.spec.ts --workers=1
```

Playwright starts Vite on `127.0.0.1:5174`; suites mock backend calls. Alternatively set `PLAYWRIGHT_CHANNEL=msedge` using the host shell's environment syntax to use installed Edge. The dashboard currently has no matching Playwright test script.

Native chat database tests require SQLite development files and the logger headers:

```sh
cmake -S ui/clients/webview/tests -B build/chat-db-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/chat-db-tests --config Release --parallel
ctest --test-dir build/chat-db-tests -C Release --output-on-failure
```

Note that python binary can be found at: `D:/workspace/sdks/Python/Python312/python.exe`

## Protect local data

- Never clear, reset, migrate experimentally, re-embed, or overwrite an existing user project/chat database or vector index as part of testing. Use temporary copies or purpose-built fixtures.
- Commands such as `embed --force`, `clear`, `update`, `compact`, and `serve --watch` modify project data. Check the actual config and resolved output paths before using them on a scratch project.
- Keep credentials, real provider responses containing private code, personal dashboard preferences, and generated database/index/log files out of commits. Do not print API keys while inspecting configuration.
- Keep third-party reference snapshots unchanged unless deliberately refreshing them; update their provenance notes when refreshed.
