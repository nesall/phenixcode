import type { InstanceItem, ProjectItem, SettingsJsonType } from "../app";
import { createToaster } from '@skeletonlabs/skeleton-svelte';

export const toaster = createToaster();

export function jsonDeepCopy<T>(obj: T): T {
  return JSON.parse(JSON.stringify(obj));
}

const defaultJsonSettings: SettingsJsonType =
{
  "tokenizer": {
    "config_path": "./bge_tokenizer.json"
  },
  "embedding": {
    "apis": [
      {
        "api_key": "",
        "api_url": "http://127.0.0.1:8583/embedding",
        "id": "local",
        "model": "bge-base-v1.5",
        "name": "llamacpp-server",
        "document_format": "{}",
        "query_format": "Represent this sentence for searching relevant passages: {}"
      }
    ],
    "current_api": "local",
    "batch_size": 4,
    "timeout_ms": 30000,
    "retry_attempts": 3,
    "top_k": 5,
    "prepend_label_format": "[Source: {}]\n"
  },
  "generation": {
    "apis": [
      {
        "api_key": "${MISTRAL_API_KEY}",
        "api_url": "https://api.mistral.ai/v1/chat/completions",
        "id": "mistral-devstral",
        "model": "devstral-small-latest",
        "name": "Mistral",
        "pricing_tpm": {
          "input": 0.1,
          "output": 0.3
        },
        "context_length": 128000
      },
      {
        "api_key": "${GEMINI_API_KEY}",
        "api_url": "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions",
        "id": "gemini-2.0-flash",
        "model": "gemini-2.0-flash",
        "name": "Gemini",
        "pricing_tpm": {
          "input": 0.1,
          "output": 0.4
        },
        "context_length": 1000000
      },
      {
        "api_key": "${DEEPSEEK_API_KEY}",
        "api_url": "https://api.deepseek.com/chat/completions",
        "id": "deepseek",
        "model": "deepseek-chat",
        "name": "DeepSeek",
        "pricing_tpm": {
          "cached_input": 0.028,
          "input": 0.28,
          "output": 0.42
        },
        "context_length": 128000
      },
      {
        "api_key": "${XAI_API_KEY}",
        "api_url": "https://api.x.ai/v1/chat/completions",
        "id": "xai",
        "model": "grok-4-fast-non-reasoning",
        "name": "X AI",
        "pricing_tpm": {
          "cached_input": 0.05,
          "input": 0.2,
          "output": 0.5
        },
        "context_length": 2000000
      },
      {
        "api_key": "${OPENAI_API_KEY}",
        "api_url": "https://api.openai.com/v1/chat/completions",
        "id": "openai-4o-mini",
        "model": "gpt-4o-mini",
        "name": "OpenAI",
        "max_tokens_name": "max_completion_tokens",
        "pricing_tpm": {
          "cached_input": 0.075,
          "input": 0.15,
          "output": 0.6
        },
        "context_length": 128000
      }
    ],
    "current_api": "mistral-devstral",
    "timeout_ms": 120000,
    "max_chunks": 7,
    "max_full_sources": 2,
    "max_related_per_source": 3,
    "max_context_tokens": 64000,
    "default_temperature": 0.1,
    "default_max_tokens": 2048,
    "default_max_tokens_name": "max_tokens",
    "prepend_label_format": "[Source: {}]\n",
    "excerpt": {
      "enabled": true,
      "min_chunks": 3,
      "max_chunks": 15,
      "threshold_ratio": 0.75
    }
  },
  "database": {
    "sqlite_path": "./db_metadata.db",
    "index_path": "./db_embeddings.index",
    "vector_dim": 768,
    "max_elements": 100000,
    "distance_metric": "cosine",
    "_comment": "For distance_metric use either cosine (default) or l2"
  },
  "chunking": {
    "semantic": true,
    "nof_min_tokens": 50,
    "nof_max_tokens": 450,
    "overlap_percentage": 0.2
  },
  "source": {
    "project_id": "",
    "project_title": "Default Project Title",
    "project_description": "",
    "paths": [
      {
        "exclude": [
          "*/dist/*",
          "*/test/*",
          "*/3rdparty/*"
        ],
        "extensions": [],
        "path": "./",
        "recursive": true,
        "type": "directory"
      }
    ],
    "default_extensions": [".c", ".cpp", ".h", ".hpp", ".py", ".js", ".ts", ".java", ".rs", ".cs", ".xaml", ".php", ".jsp", ".html", ".css", ".md"],
    "global_exclude": [
      "*/node_modules/*", "*.min.js", "*.log", "*/build/*",
      "*/.git/*", ".gitignore", "*/CVS/*",
      "*/debug/*", "*/release/*", "*/lib/*", "*/docker/*",
      "*/fonts/*", "*/images/*", "*/test/*", "*/tests/*",
      "*/example/*", "*/examples/*", "*/obj/*", "*/build_dbg/*", "*/build_rel/*"
    ],
    "encoding": "utf-8",
    "max_file_size_mb": 10
  },
  "logging": {
    "log_to_console": true,
    "log_to_file": true,
    "logging_file": "embedder.log",
    "diagnostics_file": "embedder_d.log"
  }
};



export const Consts = {
  DarkOrLightKey: "darkOrLight",
  EmbedderExecutablePath: "EmbedderExecutablePath",
  WatchForChanges: "WatchForChanges",
  WatchInterval: "WatchInterval",
};

export async function setPersistentKey(key: string, value: string, sendToCpp = true) {
  try {
    console.log(`setPersistentKey ${key}`, value, sendToCpp, window.cppApi);
    localStorage.setItem(key, value);
    if (sendToCpp && window.cppApi) {
      await window.cppApi.setPersistentKey(key, value);
      console.log(`Saved persistent key ${key} to C++`, value);
    }
  } catch (error) {
    console.log(`Unable to set persistent key ${key}`, error)
  }
}

export async function getPersistentKey(key: string, readFromCpp = true): Promise<string | null> {
  try {
    console.log(`getPersistentKey ${key}`, readFromCpp, window.cppApi);
    let val = localStorage.getItem(key);
    if (readFromCpp && window.cppApi) {
      val = await window.cppApi.getPersistentKey(key);
      console.log(`Loaded persistent key ${key} from C++`, val);
      if (val != null) {
        localStorage.setItem(key, val);
      }
    }
    return val;
  } catch (error) {
    console.log(`Unable to get persistent key ${key}`, error)
  }
  return null;
}
// Mock data for testing without C++ backend
let mockProjects: ProjectItem[] = [
  {
    settingsFilePath: "C:/Users/Arman/workspace/projects/alpha/settings-embcpp.json",
    jsonData: {
      ...defaultJsonSettings,
      source: {
        ...defaultJsonSettings.source,
        project_id: "alpha-316e366b",
        project_title: "phenixcode",
      }
    },
    status: 'success'
  },
  {
    settingsFilePath: "C:/Users/Arman/workspace/projects/alpha/settings-vsix.json",
    jsonData: {
      ...defaultJsonSettings,
      source: {
        ...defaultJsonSettings.source,
        project_id: "project2",
        project_title: "ChatAssistantVSIX",
      }
    },
    status: 'success'
  }
];

let mockInstances: InstanceItem[] = [
  {
    "config": "/workspace/projects/alpha/settings-embcpp.json",
    "cwd": "\\workspace\\projects\\alpha\\phenixcode-v1.0.2-win64",
    "host": "localhost",
    "id": "MERTUN-18860-1765014564",
    "last_heartbeat": 1765021210,
    "last_heartbeat_str": "2025-12-06 15:40:10",
    "name": "phenixcode",
    "pid": 18860,
    "port": 8590,
    "project_id": "alpha-316e366b",
    "started_at": 1765014564,
    "started_at_str": "2025-12-06 13:49:24",
    "status": "healthy",
    params: { "watch_interval": 0 }
  },
  { // instance launched outside of admin dashboard (possibility to import into dashboard).
    "config": "/workspace/projects/alpha/settings-embcpp_2.json",
    "cwd": "\\workspace\\projects\\alpha\\phenixcode-v1.0.2-win64",
    "host": "localhost",
    "id": "MERTUN-18860-1765014564_2",
    "last_heartbeat": 1765021210,
    "last_heartbeat_str": "2025-12-06 15:40:10",
    "name": "phenixcode_2",
    "pid": 18860,
    "port": 8590,
    "project_id": "alpha-316e366b_2",
    "started_at": 1765014564,
    "started_at_str": "2025-12-06 13:49:24",
    "status": "healthy",
    params: { "watch_interval": 60 }
  }
];

async function test_getInstances() {
  console.log("[mock] fetching instances", mockInstances);
  return { status: "success", instances: mockInstances };
}

async function test_getProjectList() {
  console.log("[mock] fetching projects", mockProjects);
  return { status: "success", projects: mockProjects };
}

async function test_saveProjectSettings(project: ProjectItem): Promise<{ status: string; message: string }> {
  console.log("[mock] saving project settings", project);
  return { status: "success", message: "Project 2 settings updated." };
  // return { status: "error", message: "Project not found." };
}

async function test_createProject(): Promise<ProjectItem> {
  const newProject = jsonDeepCopy({ settingsFilePath: "", jsonData: defaultJsonSettings }) as ProjectItem;
  newProject.settingsFilePath = `C:/Users/Arman/workspace/projects/alpha/settings-new-${Date.now()}.json`;
  newProject.jsonData.source.project_id = `project${mockProjects.length + 1}`;
  newProject.jsonData.source.project_title = `New Project ${mockProjects.length + 1}`;
  mockProjects = [newProject, ...mockProjects];
  console.log("[mock] creating new project", newProject);
  return newProject;
}

async function test_deleteProject(project: ProjectItem): Promise<{ status: string; message: string }> {
  const index = mockProjects.findIndex(p => p.settingsFilePath === project.settingsFilePath);
  if (index !== -1) {
    mockProjects.splice(index, 1);
    console.log("[mock] deleted project", project);
    return { status: "success", message: "Project deleted successfully." };
  } else {
    console.log("[mock] failed to delete project - not found", project);
    return { status: "error", message: "Project not found." };
  }
}

async function test_importProject(projectId: string, configPath: string): Promise<{ status: string; message: string }> {
  const importedProject: ProjectItem = {
    settingsFilePath: configPath,
    jsonData: {
      ...defaultJsonSettings,
      source: {
        ...defaultJsonSettings.source,
        project_id: projectId,
        project_title: `Imported Project ${projectId}`,
      }
    }, 
    status: 'success'
  };
  mockProjects = [importedProject, ...mockProjects];
  console.log("[mock] imported project", importedProject);
  return { status: "success", message: "Project imported successfully." };
}

async function test_startServe(project: ProjectItem, coreExecutablePath: string, watch: boolean, interval: number) {
  let inst: InstanceItem = {
    config: project.settingsFilePath,
    cwd: "C:\\Users\\Arman\\workspace\\projects\\alpha\\phenixcode-v1.0.2-win64",
    host: "localhost",
    id: "instance-for-" + project.jsonData.source.project_id,
    last_heartbeat: Date.now(),
    last_heartbeat_str: new Date().toISOString(),
    name: project.jsonData.source.project_title,
    pid: 20000 + Math.floor(Math.random() * 10000),
    port: 8600 + Math.floor(Math.random() * 1000),
    project_id: project.jsonData.source.project_id,
    started_at: Date.now(),
    started_at_str: new Date().toISOString(),
    status: "healthy",
    params: { watch_interval: watch && 0 < interval ? interval : 0 }
  }
  mockInstances.push(inst);
  return { status: "success", message: "Project started successfully." };
}

async function test_stopServe(instanceId: string) {
  const insts = mockInstances;
  const inst = insts.find((instance) => instance.id === instanceId);
  if (inst) {
    mockInstances = mockInstances.filter(i => i.project_id != inst.project_id);
    console.log("test_stopServe", mockInstances);
    return { status: "success", message: "Project started successfully." };
  }
  return { status: "error", message: "Instance not found." };
}

async function test_checkPathExists(path: string) {
  let ok = true;
  if (path.includes("test2")) ok = false;
  else if (path === "./phenixcode-core2") ok = false;
  console.log("test_checkPathExists", path, ok);
  if (ok) {
    return { status: "success", message: "Path exists." };
  } else {
    return { status: "error", message: "Path does not exist." };
  }
}

export async function helper_getInstances(): Promise<{ status: string, instances: InstanceItem[], message?: string }> {
  if (window.cppApi) {
    return await window.cppApi.getInstances();
  } else {
    return await test_getInstances();
  }
}

export async function helper_getProjectList(): Promise<{ status: string, projects: ProjectItem[], message?: string }> {
  if (window.cppApi) {
    return await window.cppApi.getProjectList();
  } else {
    return await test_getProjectList();
  }
}

export async function helper_saveProjectSettings(project: ProjectItem): Promise<{ status: string; message: string }> {
  if (window.cppApi) {
    return await window.cppApi.saveProject(project);
  } else {
    return await test_saveProjectSettings(project);
  }
}

export async function helper_createProject(): Promise<ProjectItem> {
  if (window.cppApi) {
    return await window.cppApi.createProject();
  } else {
    return await test_createProject();
  }
}

export async function helper_deleteProject(project: ProjectItem): Promise<{ status: string; message: string }> {
  if (window.cppApi) {
    return await window.cppApi.deleteProject(project);
  } else {
    return await test_deleteProject(project);
  }
}

export async function helper_importProject(projectId: string, configPath: string): Promise<{ status: string; message: string }> {
  if (window.cppApi) {
    return await window.cppApi.importProject(projectId, configPath);
  } else {
    return await test_importProject(projectId, configPath);
  }
}

export async function helper_startServe(project: ProjectItem, coreExecutablePath: string, watch: boolean, interval: number) {
  if (window.cppApi) {
    return await window.cppApi.startServe(project, coreExecutablePath, watch, interval);
  } else {
    return await test_startServe(project, coreExecutablePath, watch, interval);
  }
}

export async function helper_stopServe(instanceId: string) {
  if (window.cppApi) {
    return await window.cppApi.stopServe(instanceId);
  } else {
    return await test_stopServe(instanceId);
  }
}

export async function helper_checkPathExists(path: string) {
  if (window.cppApi) {
    return await window.cppApi.checkPathExists(path);
  } else {
    return await test_checkPathExists(path);
  }
}

export function standardizePath(path: string): string {
  return path.replace(/\\/g, '/').toLowerCase();
}

export function mapProjectToInstance(project: ProjectItem, insts: InstanceItem[]): InstanceItem | null {
  const v = insts.find(i => standardizePath(i.config) === standardizePath(project.settingsFilePath));
  return v ? v : null;
}

export async function isParentDirValid(path: string) {
  path = standardizePath(path);
  const parentDir = path.substring(0, path.lastIndexOf('/'));
  return await helper_checkPathExists(parentDir);
}

export async function hardValidateProjectItem(item: ProjectItem): Promise<{ status: string; message: string }[]> {

  const vec: { status: string; message: string }[] = [];

  // Helper to resolve relative paths to absolute based on the settings file's directory
  // function resolvePath(base: string, p: string): string {
  //   if (p.startsWith('/') || p.includes(':')) return p; // Assume absolute if starts with / or has drive letter
  //   return base ? base + '/' + p : p;
  // }

  // Get the base directory of the settings file
  // const baseDir = item.settingsFilePath.substring(0, item.settingsFilePath.lastIndexOf('/'));
  // if (!baseDir) {
  //   return { status: "error", message: "Invalid settings file path: no parent directory." };
  // }

  // Check if the settings file's parent directory exists
  const settingsParentRes = await helper_checkPathExists(item.settingsFilePath);
  if (settingsParentRes.status !== "success") {
    vec.push({ status: "error", message: `Settings file does not exist` });
  }

  // Validate source paths
  // for (const pathEntry of item.jsonData.source.paths) {
  //   const fullPath = resolvePath(baseDir, pathEntry.path);
  //   if (pathEntry.type === 'directory') {
  //     const res = await helper_checkPathExists(fullPath);
  //     if (res.status !== "success") {
  //       return { status: "error", message: `Source directory does not exist: ${fullPath}` };
  //     }
  //   } else { // 'file'
  //     const parent = fullPath.substring(0, fullPath.lastIndexOf('/'));
  //     if (parent) {
  //       const res = await helper_checkPathExists(parent);
  //       if (res.status !== "success") {
  //         return { status: "error", message: `Source file parent directory does not exist: ${parent}` };
  //       }
  //     }
  //   }
  // }

  if ((await isParentDirValid(item.jsonData.database.sqlite_path)).status !== "success") {
    vec.push({ status: "error", message: `Database SQLite directory does not exist for path: ${item.jsonData.database.sqlite_path}` });
  }

  if ((await isParentDirValid(item.jsonData.database.index_path)).status !== "success") {
    vec.push({ status: "error", message: `Database index directory does not exist for path: ${item.jsonData.database.index_path}` });
  }

  if ((await isParentDirValid(item.jsonData.tokenizer.config_path)).status !== "success") {
    vec.push({ status: "error", message: `Tokenizer config directory does not exist for path: ${item.jsonData.tokenizer.config_path}` });
  }

  if ((await isParentDirValid(item.jsonData.logging.logging_file)).status !== "success") {
    vec.push({ status: "error", message: `Logging file directory does not exist for path: ${item.jsonData.logging.logging_file}` });
  }

  if ((await isParentDirValid(item.jsonData.logging.diagnostics_file)).status !== "success") {
    vec.push({ status: "error", message: `Diagnostics file directory does not exist for path: ${item.jsonData.logging.diagnostics_file}` });
  }

  return vec;
}