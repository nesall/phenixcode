import { helper_fetchLogTail, helper_fetchSnapshot } from "#lib/utils.js";
import type { InstanceItem, MonitorEvent, MonitorSnapshot, ProjectItem, LogTailResult } from "../../app";

const snapshotPath = "/api/monitor/snapshot";
const streamPath = "/api/monitor/stream";
const pollIntervalMs = 2_000;
const maxLogLines = 1_000;
const maxLogLineLength = 2_000;
const maxEvents = 500;
const logBootstrapCount = 500;

export const monitorStore = $state({
  projectId: "",
  snapshot: null as MonitorSnapshot | null,
  events: {} as Record<number, MonitorEvent>,
  lastSeq: -1,
  connected: false,
  loading: false,
  error: "",
  logLines: [] as string[],
  logError: "",
  logOffset: -1,
  nowSeconds: Date.now() / 1000,
});

let activeKey = "";
let generation = 0;
let eventSource: EventSource | null = null;
let snapshotTimer: ReturnType<typeof setInterval> | null = null;
let logsTimer: ReturnType<typeof setInterval> | null = null;
let clockTimer: ReturnType<typeof setInterval> | null = null;

function coreBase(instance: InstanceItem): string {
  const host = instance.host.includes("://") ? instance.host : `http://${instance.host}`;
  const url = new URL(host);
  url.port = String(instance.port);
  return url.origin;
}

function createSnapshot(projectId: string, sessionId: string, seq: number, ts: number, heartbeat: number): MonitorSnapshot {
  return {
    project_id: projectId,
    session_id: sessionId,
    seq,
    ts,
    state: "idle",
    last_heartbeat: heartbeat,
    progress: {
      phase: "idle",
      files_total: 0,
      files_done: 0,
      current_batch: 0,
      current_batch_size: 0,
      chunks_total: 0,
      chunks_done: 0,
      eta_seconds: -1,
      started_at: 0,
    },
    metrics: {
      requests: 0,
      errors: 0,
      tokens_in: 0,
      tokens_out: 0,
      latency_p50_ms: 0,
      latency_p95_ms: 0,
      latency_samples: 0,
    },
    cost: {
      generation_input: 0,
      generation_output: 0,
      generation_cached_input: 0,
      embedding: 0,
      total: 0,
      pricing_version: "",
    },
  };
}

function beginSession(sessionId: string, projectId: string, seq: number, ts: number, heartbeat: number): MonitorSnapshot {
  monitorStore.events = {};
  monitorStore.lastSeq = -1;
  monitorStore.logLines = [];
  monitorStore.logOffset = -1;
  return createSnapshot(projectId, sessionId, seq, ts, heartbeat);
}

function applySnapshot(snapshot: MonitorSnapshot): void {
  if (!snapshot || typeof snapshot.session_id !== "string" || typeof snapshot.seq !== "number") return;
  const current = monitorStore.snapshot;
  if (!current || current.session_id !== snapshot.session_id) {
    monitorStore.snapshot = beginSession(
      snapshot.session_id,
      snapshot.project_id || monitorStore.projectId,
      snapshot.seq,
      snapshot.ts,
      snapshot.last_heartbeat,
    );
  } else if (snapshot.seq < monitorStore.lastSeq) {
    return;
  }

  monitorStore.snapshot = snapshot;
  monitorStore.lastSeq = Math.max(monitorStore.lastSeq, snapshot.seq);
  monitorStore.events[snapshot.seq] = {
    seq: snapshot.seq,
    ts: snapshot.ts,
    session_id: snapshot.session_id,
    kind: "snapshot",
    state: snapshot.state,
    last_heartbeat: snapshot.last_heartbeat,
    progress: snapshot.progress,
    metrics: snapshot.metrics,
    cost: snapshot.cost,
  };
  trimEvents();
}

function trimEvents(): void {
  const sequences = Object.keys(monitorStore.events).map(Number).sort((a, b) => a - b);
  while (sequences.length > maxEvents) {
    delete monitorStore.events[sequences.shift()!];
  }
}

function applyEvent(event: MonitorEvent): void {
  if (!event || typeof event.session_id !== "string" || typeof event.seq !== "number") return;
  const current = monitorStore.snapshot;
  if (!current || current.session_id !== event.session_id) {
    monitorStore.snapshot = beginSession(
      event.session_id,
      monitorStore.projectId,
      -1,
      event.ts,
      event.last_heartbeat ?? event.ts,
    );
  }
  if (event.seq <= monitorStore.lastSeq) return;

  const snapshot = monitorStore.snapshot!;
  monitorStore.snapshot = {
    ...snapshot,
    seq: event.seq,
    ts: event.ts,
    state: event.state ?? snapshot.state,
    last_heartbeat: event.last_heartbeat ?? snapshot.last_heartbeat,
    progress: { ...snapshot.progress, ...event.progress },
    metrics: { ...snapshot.metrics, ...event.metrics },
    cost: { ...snapshot.cost, ...event.cost },
  };
  monitorStore.events[event.seq] = event;
  monitorStore.lastSeq = event.seq;
  trimEvents();
}

function endpoint(base: string, path: string, params: Record<string, string> = {}): string {
  const url = new URL(path, base);
  for (const [key, value] of Object.entries(params)) url.searchParams.set(key, value);
  return url.toString();
}

async function getJson<T>(url: string): Promise<T> {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Request failed (${response.status})`);
  return await response.json() as T;
}

function stopTimers(): void {
  if (snapshotTimer) clearInterval(snapshotTimer);
  if (logsTimer) clearInterval(logsTimer);
  if (clockTimer) clearInterval(clockTimer);
  snapshotTimer = null;
  logsTimer = null;
  clockTimer = null;
}

function stopConnections(): void {
  stopTimers();
  eventSource?.close();
  eventSource = null;
}

async function loadSnapshot(project: ProjectItem, instance: InstanceItem | null, run: number): Promise<void> {
  monitorStore.loading = !monitorStore.snapshot;
  try {
    if (!instance) {
      const res = await helper_fetchSnapshot(project.settingsFilePath);
      if (res.status !== "success") throw new Error(res.message);
      if (run !== generation) return;
      applySnapshot(res.monitor);
      monitorStore.error = "";
      return;
    }
    const base = coreBase(instance);
    const snapshot = await getJson<MonitorSnapshot>(endpoint(base, snapshotPath));
    if (run !== generation) return;
    applySnapshot(snapshot);
    monitorStore.error = "";
  } catch (error) {
    if (run !== generation) return;
    monitorStore.error = error instanceof Error ? error.message : String(error);
    console.error("Unable to load monitor snapshot", error);
  } finally {
    if (run === generation) monitorStore.loading = false;
  }
}

async function fetchTail(project: ProjectItem): Promise<LogTailResult> {
  const result = await helper_fetchLogTail(project.settingsFilePath, "main", -logBootstrapCount, logBootstrapCount);
  if (result.status !== "success") throw new Error(result.message || "Log tail request failed");
  return result;
}

async function loadLogTail(project: ProjectItem, run: number): Promise<void> {
  try {
    const bootstrap = monitorStore.logOffset < 0; // sentinel: no cursor yet
    let replace = bootstrap;
    let result: LogTailResult;

    if (bootstrap) {
      result = await fetchTail(project);
    } else {
      result = await helper_fetchLogTail(project.settingsFilePath, "main", monitorStore.logOffset, logBootstrapCount);
      if (run !== generation) return;
      if (result.status !== "success") throw new Error(result.message || "Log tail request failed");
      if (result.total_lines < monitorStore.logOffset) {
        // File truncated/rotated — restart from tail
        replace = true;
        result = await fetchTail(project);
      }
    }
    if (run !== generation) return;

    const lines = result.lines.map((line) =>
      line.length > maxLogLineLength ? `${line.slice(0, maxLogLineLength - 3)}...` : line
    );
    monitorStore.logLines = (replace ? lines : [...monitorStore.logLines, ...lines]).slice(-maxLogLines);
    monitorStore.logOffset = replace
      ? result.total_lines
      : monitorStore.logOffset + result.lines.length; // advance by lines received, so a >count backlog isn't skipped
    monitorStore.logError = "";
  } catch (error) {
    if (run !== generation) return;
    monitorStore.logError = error instanceof Error ? error.message : String(error);
    console.error("Unable to load project log tail", error);
  }
}

function start(project: ProjectItem, instance: InstanceItem | null, run: number): void {
  console.log(`Starting monitor for project ${project.jsonData.source.project_id} (run ${run})`);
  void loadSnapshot(project, instance, run);
  void loadLogTail(project, run);
  logsTimer = setInterval(() => void loadLogTail(project, run), pollIntervalMs);
  clockTimer = setInterval(() => {
    if (run === generation) monitorStore.nowSeconds = Date.now() / 1000;
  }, pollIntervalMs);

  if (!instance) return;

  const base = coreBase(instance);
  eventSource = new EventSource(endpoint(base, streamPath));
  eventSource.onopen = () => {
    if (run !== generation) return;
    monitorStore.connected = true;
    if (snapshotTimer) clearInterval(snapshotTimer);
    snapshotTimer = null;
  };
  eventSource.onmessage = (message) => {
    if (run !== generation) return;
    try {
      const payload = JSON.parse(message.data) as MonitorEvent | MonitorSnapshot;
      if ("project_id" in payload && "progress" in payload && "metrics" in payload && "cost" in payload) {
        applySnapshot(payload as MonitorSnapshot);
      } else {
        applyEvent(payload as MonitorEvent);
      }
      monitorStore.error = "";
    } catch (error) {
      monitorStore.error = error instanceof Error ? error.message : String(error);
      console.error("Unable to parse monitor stream event", error);
    }
  };
  eventSource.onerror = () => {
    if (run !== generation) return;
    monitorStore.connected = false;
    if (!snapshotTimer) {
      snapshotTimer = setInterval(() => void loadSnapshot(project, instance, run), pollIntervalMs);
    }
  };
}

export function selectMonitorProject(project: ProjectItem | null, instance: InstanceItem | null): void {
  const key = project ? `${project.settingsFilePath}|${instance?.id ?? "offline"}` : "";
  if (key === activeKey) return;
  activeKey = key;
  generation += 1;
  const run = generation;
  stopConnections();
  eventSource?.close();
  eventSource = null;
  monitorStore.projectId = project?.jsonData.source.project_id ?? "";
  monitorStore.snapshot = null;
  monitorStore.events = {};
  monitorStore.lastSeq = -1;
  monitorStore.connected = false;
  monitorStore.loading = !!project;
  monitorStore.error = "";
  monitorStore.logLines = [];
  monitorStore.logError = "";
  monitorStore.logOffset = -1;
  if (project) start(project, instance, run);
}
