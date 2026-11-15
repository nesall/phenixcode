[![License](https://img.shields.io/badge/License-MIT-blue.svg)](./LICENSE)

# PhenixCode Assistant

A self-hosted, privacy-first RAG and AI assistance system tailored for codebases. Index your code locally, perform hybrid vector + lexical searches, and get context-aware answers using local models or remote LLM providers.

**Built for developers who want:**
- **Privacy** – Keep code within your infrastructure using local models or private endpoints. Remote embedding and generation providers receive the source content and context sent to them.
- **Zero Subscriptions** – Run completely offline using local models.
- **Hybrid Retrieval** – Combine dense semantic embeddings with exact keyword BM25 retrieval via SQLite FTS5.
- **Zero-Latency Auto-Routing** – Automatically classify queries into model tiers using an embedded sub-millisecond C++ classifier without LLM classification overhead.
- **Multi-Provider Flexibility** – Seamlessly switch between local `llama-server` instances, OpenAI, Anthropic, Mistral, xAI, and other OpenAI/Anthropic-compatible APIs.

![PhenixCode UI](media/demo.gif)

*Complete control and full customization*

![PhenixCode Admin Dashboard](media/dashboard0.png)

**Key Features:** Hybrid Search (HNSW + SQLite FTS5 BM25) • Read-Only Agent Tools • Local Sub-Millisecond Task Auto-Router • Live Reasoning • Searchable Chat History & Trash • Database Browser • Anthropic & OpenAI API Support • Retrieval Evaluation Suite (Recall@k, MRR, NDCG) • JWT Auth • HTTP API • Decoupled Provider & Project Settings

**[Download release](https://github.com/nesall/phenixcode/releases/latest)** | **[Quick start](#quick-start)**

---

### Why Self-Host?

- **GitHub Copilot:** $10–20/month, cloud-only, limited codebase context control.
- **PhenixCode:** Free, operates offline or with private endpoints, fully customizable indexing, hybrid code retrieval, and zero telemetry.

*Note: PhenixCode is designed for codebase question-answering, architecture exploration, and conversational assistance, rather than inline ghost-text auto-complete.*

---

### Features Overview

#### Core & Retrieval Engine
* **Hybrid Search (Vector + BM25):** Combines HNSW dense vector similarity with SQLite FTS5 full-text indexing using Reciprocal Rank Fusion (RRF).
* **Embedded Auto-Router:** Zero-latency query classifier (`assets/baseline_v2.bin`) that automatically routes queries to simple, medium, or complex model tiers without API roundtrips.
* **Code-Aware Chunking & Tokenization:** Token-budgeted, line-aware code chunking and text splitting with configurable overlap. Token counts depend on the configured tokenizer and can differ from the embedding model's counts.
* **Incremental Synchronization:** File hash and modification tracking detects new, modified, or deleted files to update vector and FTS indexes incrementally.
* **Retrieval Evaluation Suite (`eval`):** Built-in CLI commands to measure Recall@k, MRR, and NDCG against ground-truth code reference datasets.

#### Inference & Model Providers
* **Decoupled Provider Architecture:** Global API providers (`providers.json`) are maintained independently from project-level configurations (`settings.json`).
* **Broad Protocol Support:** Native support for both OpenAI-compatible and Anthropic-style chat completions.
* **Local & Cloud Models:** First-class support for `llama-server`, Ollama, vLLM, OpenAI, Anthropic, Mistral, Grok, and custom endpoints.
* **Fill-In-The-Middle (FIM):** Dedicated code completion API endpoint.
* **Read-Only Agent Tools:** Supported models can request additional source searches and file excerpts after receiving the initial retrieved context. See [Agent Tools](#agent-tools) for setup and compatibility.
* **Reasoning Controls & Replay:** Provider-defined reasoning parameters are selectable per request. Optional history replay retains assistant reasoning in backend memory for subsequent turns during the current backend session.

#### User Interfaces & Services
* **Chat SPA & Native WebView:** Desktop GUI with Markdown tables, syntax highlighting, code-block copying, and expandable live reasoning when supplied by the model. Progress, token counts, and approximate costs remain available in the response metadata.
* **Persistent Chat Sessions:** Local SQLite-backed history with live list filtering, inline previews, and recoverable deletion through Undo and Trash/Restore.
* **Free Chat:** Converse without retrieved codebase context while still including user-attached files.
* **Admin Dashboard:** Manage projects and providers, monitor indexing and usage, and browse indexed files under **Monitor → Database**. Inspect stored chunks, preview current file contents, or open a file's parent directory.
* **Background Monitoring & Daemonizing:** Built-in `--watch` filesystem monitoring, systemd / LaunchAgent / NSSM service scripts.

---

<a name="quick-start"></a>
### Quick Start (Prebuilt Binaries)

#### 1. Download and Extract
Download the latest release package for your operating system and extract the contents.

#### 2. Configure Providers and Settings
PhenixCode separates global LLM connections from project indexing:
- `providers.json`: Configures available embedding and generation models (local endpoints or cloud API keys).
- `settings.json`: Configures project-specific source directories, chunking limits, and vector storage paths.

Open the admin dashboard and choose **Add new project**. Enter a project filename (or accept the generated name); the dashboard creates a blank configuration from built-in defaults and reconciles its model selections with your provider catalog. Set the source directories and database location before indexing.

For manual setup, use the [configuration examples](#configuration-files) below. Select provider IDs that exist in your catalog and point their URLs at your running model servers. Project files can have any filename; pass `--config /path/to/project.json` to core commands when using a name other than `settings.json`.

#### 3. Start Local LLMs (Optional)
If running fully offline, start your embedding and generation models (e.g., using `llama-server`):

```bash
# 1. Start Embedding Server (e.g., CodeRankEmbed / BGE)
./llama-server -m ./coderankembed-q4_k_m.gguf --embedding --pooling cls -ub 8192 --port 8584 &

# 2. Start Generation Server (e.g., Qwen 2.5 Coder)
./llama-server -m ./qwen2.5-coder-1.5b-instruct-q4_k_m.gguf -c 32768 --port 8585 -np 1 &

```

> **Using Cloud Providers:** To use cloud APIs (Anthropic, OpenAI, Mistral, xAI), ensure their keys are exported in your environment (e.g., `export ANTHROPIC_API_KEY="..."`, `export OPENAI_API_KEY="..."`) and enabled in `providers.json`.

#### 4. Build the Codebase Index

Run the embedding process against your configured project sources:

```bash
./phenixcode-core embed

```

#### 5. Launch the Server and UI

Start the core HTTP backend with live file-watching enabled:

```bash
./phenixcode-core serve --watch --interval 60

```

In a separate terminal or desktop shortcut, open the UI:

```bash
./phenixcode-ui

```

---

### Configuration Files

#### `providers.json` (Global API Providers)

Stores API keys, base URLs, protocol styles, and model definitions:

```json
{
  "agent_enabled": false,
  "embedding_providers": [
    {
      "id": "local_embed",
      "model": "coderankembed",
      "api_url": "http://127.0.0.1:8584/v1/embeddings",
      "api_key": "",
      "enabled": true
    }
  ],
  "generation_providers": [
    {
      "id": "local_qwen",
      "model": "qwen2.5-coder-1.5b-instruct",
      "api_url": "http://127.0.0.1:8585/v1/chat/completions",
      "api_style": "openai",
      "supports_tools": "off",
      "api_key": "",
      "enabled": true
    },
    {
      "id": "claude_sonnet",
      "model": "claude-3-7-sonnet-20250219",
      "api_url": "https://api.anthropic.com/v1/messages",
      "api_style": "anthropic",
      "api_key": "${ANTHROPIC_API_KEY}",
      "enabled": true
    }
  ]
}

```

#### `settings.json` (Project Configuration)

Specifies indexing paths, chunking parameters, and runtime settings:

```json
{
  "tokenizer": {
    "config_path": "./bge_tokenizer.json"
  },
  "source": {
    "project_title": "My Application",
    "paths": [
      {
        "type": "directory",
        "path": "./src",
        "recursive": true,
        "extensions": [".cpp", ".h", ".hpp", ".c", ".py", ".ts", ".js"]
      }
    ]
  },
  "embedding": {
    "current_api": "local_embed",
    "batch_size": 4
  },
  "generation": {
    "current_api": "local_qwen",
    "enabled_providers": ["local_qwen"],
    "auto_router": { "enabled": false }
  },
  "database": {
    "sqlite_path": "./db_metadata.db",
    "index_path": "./db_embeddings.index",
    "vector_dim": 768,
    "max_elements": 100000,
    "distance_metric": "cosine"
  },
  "chunking": {
    "semantic": true,
    "nof_min_tokens": 100,
    "nof_max_tokens": 450,
    "overlap_percentage": 0.15
  }
}

```

---

The example selects one generation provider directly. Configure auto-routing in the dashboard when using multiple model tiers. Set `database.vector_dim` to your embedding model's output dimension (768 in this example), and choose chunk limits appropriate for that model and tokenizer.

### Agent Tools

Agent tools let the model investigate beyond the initial retrieved context. They are read-only and scoped to the project's configured local source files, with bounded result sizes and tool rounds.

| Tool | Purpose |
| --- | --- |
| `grep` | Search source text for a literal pattern and return matching snippets. |
| `read_file` | Read selected line ranges from an allowed source file. |
| `list_files` | Discover source paths, optionally filtered by path or extension. |
| `search_context` | Retrieve relevant indexed excerpts using natural-language hybrid search, with text fallback when query embeddings are unavailable. |
| `project_info` | Inspect project identity, configured sources, current local source count, indexed chunk count, and available tools. Index freshness is reported as unknown. |
| `project_settings` | Inspect loaded project, model, retrieval, or reasoning settings, including the generation provider selected for the request. Credentials, URLs, and prompts are omitted; database locations are filenames only. Reasoning choices describe configured defaults. |

Project summaries flag shortened arrays or strings with `truncated: true` and list affected fields in `truncated_fields` (`*` indicates additional fields).

To enable tools, set the top-level `agent_enabled` to `true` in `providers.json`, and set `supports_tools` to `"on"` on a generation provider whose model and endpoint support OpenAI-style function calling. Restart the running backend after changing provider settings. Tool progress appears in the Chat UI's response metadata.

Tool execution currently applies to `/api/chat` with OpenAI-compatible providers. `supports_tools: "auto"` does not perform detection yet; Anthropic tool execution is not supported. Tools are disabled for free-chat and attached-files-only requests. The interactive CLI `chat` command does not execute these tools.

### Reasoning Settings

Configure a provider's `reasoning.parameters` in the dashboard to expose its supported choices in the Chat UI's **Reasoning** picker. Parameters remain independent; PhenixCode does not infer that one option enables or disables another. Object-valued parameters use `value_field` to serialize the selected value into the provider's expected object.

The dashboard's **History replay** selector controls `reasoning.history_replay`: `0` disables replay, `3` or `5` retains that many recent assistant turns for replay, and `-1` selects All (capped at 255 turns). Replay can be enabled without configurable reasoning parameters. Retained responses live in backend memory only; they do not survive backend restarts. This setting controls what is sent back to the model, independently of displaying reasoning in the UI.

### CLI Reference

```
Usage: phenixcode-core <command> [options]

Commands:
  embed              Process and embed all configured sources
  update             Incrementally update changed files only
  watch              Continuously monitor and update
  search <query>     Search for similar chunks using hybrid retrieval
  stats              Show database statistics
  clear              Clear all indexed data
  compact            Reclaim deleted vector and SQLite storage
  chat               Interactive terminal chat mode
  serve              Start HTTP API server
  providers          List or test configured LLM providers
  eval               Evaluate retrieval quality against a dataset

Password Management:
  reset-password             Reset admin password directly (--pass <new_password>)
  reset-password-interactive Reset admin password interactively
  password-status            Check admin password state

```

#### Common Command Examples

```bash
# Force re-embed of all files
./phenixcode-core embed --force

# Run incremental check and update
./phenixcode-core update

# Search top 10 relevant code snippets
./phenixcode-core search "how is connection retry implemented" --top 10

# Start server on a specific port with file-watching and custom configs
./phenixcode-core serve --port 8590 --watch --interval 30 \
  --config ./settings.json \
  --providers-config ./providers.json

# Test a specific provider connection
./phenixcode-core providers --test claude_sonnet

```

#### Retrieval Evaluation (`eval`)

Run benchmark evaluations to test recall and ranking quality against test datasets:

```bash
# Generate a template evaluation file from indexed sources
./phenixcode-core eval --init --output eval.json --samples 25

# List available document paths in the index
./phenixcode-core eval --list

# Run evaluation on a dataset using dense and hybrid search
./phenixcode-core eval --dataset eval.json --top 5

```

---

### How to Build

#### Requirements

* **C++ Compiler:** C++20 compliant (`gcc >= 11`, `clang >= 13`, or `MSVC 2019+`)
* **CMake:** Version 3.20 or newer
* **Node.js:** v20 or newer (for building the Webview/Svelte UIs)

#### All-in-One Build Scripts

```bash
# Linux
./package-lin.sh

# macOS
./package-mac.sh

# Windows
package.bat

```

#### Individual Component Builds

```bash
# Build core daemon only
./build_rel.sh

# Build desktop client only
cd ui/clients/webview
./build_rel.sh

# Build admin dashboard only
cd ui/dashboard/webview
./build_rel.sh

```

---

### REST API Reference

All requests accept and return `application/json` unless otherwise specified.

| Method | Endpoint | Description |
| --- | --- | --- |
| `GET` | `/api/health` | Healthcheck and server status |
| `GET` | `/api/stats` | Indexed documents, lines, languages, and chunk counts |
| `GET` | `/api/metrics` | Internal engine telemetry and latency stats |
| `GET` | `/api/settings` | Current active model selections and settings |
| `POST` | `/api/search` | Execute hybrid search (`{"query": "...", "top_k": 5}`) |
| `POST` | `/api/chat` | Streaming SSE endpoint for code completions and chat |
| `POST` | `/api/fim` | Fill-in-the-middle code generation endpoint |
| `POST` | `/api/update` | Trigger manual incremental directory re-scan |
| `POST` | `/api/shutdown` | Graceful server shutdown (Requires `X-App-Key` header) |

#### Example: Chat Completion Request

Set `free_chat: true` to skip codebase retrieval while still processing `attachments`. Optional `reasoning` selections are an object mapping configured parameter names to selected strings, for example `"reasoning": {"reasoning_effort": "low"}` when that parameter exists on the selected provider.

```bash
curl -X POST http://localhost:8590/api/chat \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [
      {"role": "system", "content": "You are a concise code assistant."},
      {"role": "user", "content": "How does the cache eviction policy work?"}
    ],
    "temperature": 0.1,
    "max_tokens": 1024,
    "ctxratio": 0.5
  }'

```

#### Other Examples

```bash
# Get list of API endpoints
curl -X GET http://localhost:8590/api

# Health check
curl -X GET http://localhost:8590/api/health

# Get document list
curl -X GET http://localhost:8590/api/documents

# Get configuration parameters (full config)
curl -X GET http://localhost:8590/api/setup

# Get available APIs (completion endpoints)
curl -X GET http://localhost:8590/api/settings

# Get running instances (usually one instance per project codebase)
curl -X GET http://localhost:8590/api/instances

# Get database statistics
curl -X GET http://localhost:8590/api/stats

# Get metrics (JSON)
curl -X GET http://localhost:8590/api/metrics

# Get Prometheus metrics
curl -X GET http://localhost:8590/metrics

# Setup configuration (POST)
curl -X POST http://localhost:8590/api/setup \
  -H "Authorization: Basic $(echo -n "username:password" | base64)" \
  -H "Content-Type: application/json" \
  -d '{
    "embedding": {...},
    "generation": {...},
    "database": {...},
    "chunking": {...}
  }'

# Search
curl -X POST http://localhost:8590/api/search \
  -H "Content-Type: application/json" \
  -d '{"query": "optimize performance", "top_k": 5}'

# Generate embeddings (without storing)
curl -X POST http://localhost:8590/api/embed \
  -H "Content-Type: application/json" \
  -d '{"text": "your text here"}'

# Add document
curl -X POST http://localhost:8590/api/documents \
  -H "Content-Type: application/json" \
  -d '{
    "content": "your document content",
    "source_id": "document_source_id"
  }'

# Trigger a manual update
curl -X POST http://localhost:8590/api/update

# Chat with optional context (streaming)
curl -X POST http://localhost:8590/api/chat \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [
      {"role": "system", "content": "Keep it short."},
      {"role": "user", "content": "What is the capital of France?"}
    ],
    "sourceids": [
      "../phenixcode-core/src/main.cpp",
      "../phenixcode-core/include/settings.h"
    ],
    "attachments": [
      { "filename": "filename1.cpp", "content": "..text file content 1.."},
      { "filename": "filename2.cpp", "content": "..text file content 2.."}
    ],
    "temperature": 0.2,
    "max_tokens": 800,
    "targetapi": "xai",
    "ctxratio": 0.5,
    "attachedonly": false,
    "free_chat": false
  }'
  
# Fill-in-the-middle endpoint. Suffix is optional.
curl -X POST "http://localhost:8590/api/fim" \
  -H "Content-Type: application/json" \
  -d '{
    "prefix": "std::string greet(const std::string &name) { return \"Hello, \"+",
    "suffix": "+\"!\"; }",
    "temperature": 0.0,
    "max_tokens": 64,
    "filename": "inference.cpp",
    "targetapi": "xai"
  }'  

# Initiate server shutdown that was started with an app key e.g. ./phenixcode-core serve --appkey abc123
curl -X POST http://localhost:8590/api/shutdown \
  -H "X-App-Key: abc123" \
  -d '{}'  

# Authenticate  
curl -X POST http://localhost:8590/api/authenticate \
  -H "Authorization: Basic $(echo -n "username:password" | base64)" 
```
