[![License](https://img.shields.io/badge/License-MIT-blue.svg)](./LICENSE)

# PhenixCode Assistant

A self-hosted RAG system for querying codebases with AI. Index your code locally, search with semantic meaning, and get context-aware answers using local or cloud LLMs.

**Built for developers who want:**
- **Privacy** – Your code never leaves your machine
- **Zero subscriptions** – Run entirely on local models (no monthly fees)
- **Flexibility** – Switch between local and cloud LLMs as needed

Works offline with local models or integrates with OpenAI-compatible APIs. Your data, your infrastructure, your choice.

![PhenixCode UI](media/demo.gif)

*Complete control and full customization*

![PhenixCode Admin Dashboard](media/dashboard0.png)

**Key features:** Local embeddings • Fast vector search (HNSWLib) • SQLite metadata • JWT auth • HTTP API • Single JSON config

**[Download release](https://github.com/nesall/phenixcode/releases/latest)** | **[Quick start](#quick-start)**  

### Why Self-Host?

**GitHub Copilot:** $10–20/month, cloud-only, limited customization  
**PhenixCode:** Free with local models or your own APIs, fully customizable  
*Note: PhenixCode focuses on chat-based code assistance, not inline code auto-complete.*  

### Features overview

Core Functionality:  
* Lightweight tokenization  
* Smart chunking with overlap  
* Local embeddings (llama-server + any choice of embedding model)  
* Both local and remote completion models of your choice
* Fast vector search (Hnswlib with cosine similarity)  
* Metadata storage (SQLite)  
* Incremental updates with file tracking  
* CLI + HTTP API  

API & Server:  
* HTTP API server (httplib)  
* REST endpoints (/api/search, /api/chat, /api/embed)  
* Metrics endpoint (JSON + Prometheus format)  
* Health checks  
* Graceful shutdown  

Security:  
* JWT token authentication  
* Password management (CLI-based)  
* Protected admin endpoints  
* Hashed password storage  

Deployment & Operations:  
* Console setup wizard (interactive)  
* Web setup wizard (password protected)
* Service installation scripts (Windows/Linux/macOS)  
* Structured logging (output.log + diagnostics.log)  
* Auto-start on boot (systemd/NSSM/LaunchAgent)  
* Release packaging (build_rel scripts)  

Configuration:  
* Template-based settings.json  
* Environment variable overrides  
* CLI parameter support  
* Multiple source types (directories, files, URLs)  

<a name="quick-start"></a>
### Quick Start (Prebuilt Binaries)

  1. **Download** the latest release for your platform  
  2. **Extract the package**. The included `settings.json` is pre-configured for a local, private setup.  
  3. **Start Local LLMs**: Since the default configuration uses local models, you must start the necessary servers first.  
  The simplest way is using llama-server (or a similar tool):  
```
# 1. Start Embedding Server (e.g., CodeRankEmbed)
# The current settings.json is configured for this default:
./llama-server -m ./coderankembed-q4_k_m.gguf --embedding --pooling cls -ub 8192 --port 8584 &

# 2. Start Generation Server (e.g., Qwen)
# The current settings.json is configured for this default:
./llama-server -m ./qwen2.5-coder-1.5b-instruct-q4_k_m.gguf -c 32768 --port 8585 -np 1 &
```  
  *Note: To use a cloud API (like Mistral or OpenAI), simply ensure the corresponding environment variable (e.g., `${MISTRAL_API_KEY}`) is set.*  

  3. **Start embedding** (this uses the configured local directory ./):  
```
./phenixcode-core embed
```  
  4. **Start** the main HTTP server and the UI: 
```
./phenixcode-core serve --watch --interval 60
./phenixcode-ui
```  

### How to build

C++ 20 or newer is required.  
nodejs v20 or newer is required.  

```bash
# clone the repository and cd into it.

# Build both core and ui with a single command:

# Build in Linux
./package-lin.sh

# Build in MacOS
./package-mac.sh

# Build in Windows
package.bat


# To build either core or ui separately, use build_rel scripts

# e.g. building core only
./build_rel.sh

# or building ui only
cd ui/clients/webview
./build_rel.sh
```

### CLI commands

Initial full embed of all sources from settings.json  
```./phenixcode-core --config settings.json embed```

Check for changes and update only what changed  
```./phenixcode-core update```

Continuous monitoring (checks every 60 seconds)  
```./phenixcode-core watch --interval 60```

Reclaim space used by deleted index items  
```./phenixcode-core compact```

Search nearest neighbours  
```./phenixcode-core search "how to optimize C++" --top 10```

Chat with LLM  
```./phenixcode-core chat```

Serve on a custom port with auto-update every N seconds  
```./phenixcode-core serve --port 9000 --watch --interval 60```

Serve on the default port (8590) without auto-update (manual trigger via /update endpoint)  
```./phenixcode-core serve```

Default admin password: `admin` — change it immediately using one of the methods below.  
Change Password - Method 1: Direct  
```./phenixcode-core reset-password --pass NewPassword456```

Change Password - Method 2: Interactive (hides input)  
```./phenixcode-core reset-password-interactive```

Check password status  
```./phenixcode-core password-status```


### Editing settings.json

Method 1:  
Edit file `settings.json` to configure settings manually

Method 2:  
Use dashboard GUI `phenixcode_admin` to start/stop add/remove projects for various codebases (recommended).

![PhenixCode Admin Dashboard](media/dashboard1.png)


### REST API endpoints

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
    "attachedonly": false
  }'
  
# Fill-in-the-middle endpoint. Suffix is optional.
curl -X POST "http://localhost:8590/api/fim" \
  -H "Content-Type: application/json" \
  -d '{
    "prefix": "std::string greet(const std::string &name) { return \"Hello, \"+",
    "suffix": "+\"!\"; }",
    "temperature": 0.0,
    "max_tokens": 64,
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
