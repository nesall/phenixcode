
/**
 * Configuration for the LLM API pricing (Tokens Per Million).
 */
export interface PricingTpm {
  cached_input?: number;
  input: number;
  output: number;
}

/**
 * Configuration for a single Generative API (e.g., OpenAI, Mistral).
 */
export interface GenerationApi {
  api_key: string;
  api_url: string;
  id: string;
  model: string;
  name: string;
  max_tokens_name?: string; // The query parameter name for max tokens (e.g., 'max_completion_tokens')
  max_tokens?: number; // The maximum number of tokens allowed for generation
  temperature?: number; // The temperature setting for generation
  pricing_tpm: PricingTpm;
  context_length: number;
  _hidden?: boolean;
}

/**
 * Settings for excerpt generation (summarizing retrieved context).
 */
export interface ExcerptSettings {
  enabled: boolean;
  min_chunks: number;
  max_chunks: number;
  threshold_ratio: number;
}

export interface AutoRouterClassifierConfig {
  type: "internal" | "api" | string;
  api_id: string;
  max_tokens: number;
  prompt: string;
  temperature: number;
  timeout_ms: number;
}

export interface AutoRouterFallbackConfig {
  _comment?: string;
  default_model_id: string;
}

export interface AutoRouterTierRule {
  direct_model_id?: string;
  strategy: "direct" | "ensemble" | string;
  draft_model_ids?: string[];
  synthesizer_model_id?: string;
  _strategy?: string;
}

export interface AutoRouterRulesConfig {
  tier_1_simple: AutoRouterTierRule;
  tier_2_medium: AutoRouterTierRule;
  tier_3_complex: AutoRouterTierRule;
  [tier: string]: AutoRouterTierRule;
}

export interface AutoRouterConfig {
  enabled: boolean;
  _validate_ids_exist?: boolean;
  classifier: AutoRouterClassifierConfig;
  fallback: AutoRouterFallbackConfig;
  routing_rules: AutoRouterRulesConfig;
}

export interface GenerationSettings {
  enabled_providers: string[];
  current_api: string;
  timeout_ms: number;
  max_chunks: number;
  max_full_sources: number;
  max_related_per_source: number;
  default_context_length: number;
  default_temperature: number;
  default_max_tokens: number;
  default_max_tokens_name: string;
  prepend_label_format: string;
  excerpt: ExcerptSettings;
  auto_router?: AutoRouterConfig; // Optional sub-block
}

/**
 * Configuration for a single Embedding API (e.g., local server, OpenAI).
 */
export interface EmbeddingApi {
  api_key: string;
  api_url: string;
  id: string;
  model: string;
  name: string;
  document_format: string;
  query_format: string;
  _hidden?: boolean;
}

/**
 * Top-level configuration for the Embedding service.
 */
export interface EmbeddingSettings {
  current_api: string;
  batch_size: number;
  timeout_ms: number;
  retry_attempts: number;
  top_k: number;
  prepend_label_format: string;
}

/**
 * Configuration for the Vector Database.
 */
export interface DatabaseSettings {
  sqlite_path: string;
  index_path: string;
  vector_dim: number;
  max_elements: number;
  distance_metric: 'cosine' | 'l2';
  _comment: string; // Added back the comment field which was present in the JSON definition
}

/**
 * Configuration for document chunking.
 */
export interface ChunkingSettings {
  semantic: boolean;
  nof_min_tokens: number;
  nof_max_tokens: number;
  overlap_percentage: number;
}

/**
 * Configuration for application logging.
 */
export interface LoggingSettings {
  log_to_console: boolean;
  log_to_file: boolean;
  logging_file: string;
  diagnostics_file: string;
}

/**
 * Configuration for a single path entry in the source settings.
 */
export interface PathEntry {
  exclude: string[];
  extensions: string[];
  path: string;
  recursive: boolean;
  type: 'directory' | 'file';
}

/**
 * Top-level configuration for document source and indexing.
 */
export interface SourceSettings {
  project_id: string;
  project_title: string;
  project_description: string;
  paths: PathEntry[];
  default_extensions: string[];
  global_exclude: string[];
  encoding: string;
  max_file_size_mb: number;
}

/**
 * Configuration for the tokenizer model.
 */
export interface TokenizerSettings {
  config_path: string;
}

/**
 * The root interface for the entire application configuration.
 */
export interface SettingsJsonType {
  tokenizer: TokenizerSettings;
  embedding: EmbeddingSettings;
  generation: GenerationSettings;
  database: DatabaseSettings;
  chunking: ChunkingSettings;
  source: SourceSettings;
  logging: LoggingSettings;
}

export interface ProvidersSettings {
  embedding_providers: EmbeddingApi[];
  generation_providers: GenerationApi[];
}

/**
 * Interface representing a project item. ProjectItem should alsways be valid and have jsonData populated.
 */
export interface ProjectItem {
  settingsFilePath: string; // for informational purposes
  jsonData: SettingsJsonType;
  status: string;
  message?: string;
}

export interface InstanceItem {
  config: string;
  cwd: string;
  host: string;
  id: string;
  last_heartbeat: number;
  last_heartbeat_str: string;
  name: string;
  pid: number;
  port: number;
  project_id: string;
  started_at: number;
  started_at_str: string;
  status: string;
  _hidden?: boolean;
  params: { watch_interval?: number };
}


// ---------------------------------------------------------------------------
// Monitor / Live-View
// ---------------------------------------------------------------------------

export type MonitorState = "idle" | "scanning" | "indexing" | "ready" | "error";

/** Snapshot written by the core; also served by GET /api/monitor/snapshot. */
export interface MonitorSnapshot {
  project_id: string;
  session_id: string;        // unique per core run (use instance.id)
  seq: number;               // monotonic; drop out-of-order events
  ts: number;                // epoch seconds, server-side
  state: MonitorState;
  last_heartbeat: number;    // epoch seconds; age => "stale" badge
  progress: MonitorProgress;
  metrics: MonitorMetrics;
  cost: MonitorCost;
}

export interface MonitorProgress {
  phase: "idle" | "walk" | "chunk" | "embed" | "persist";
  files_total: number;
  files_done: number;
  current_batch: number;      // 1-based batch index in this phase
  current_batch_size: number; // files in the current batch
  chunks_total: number;
  chunks_done: number;
  eta_seconds: number;        // -1 = unknown
  started_at: number;         // epoch seconds
}

export interface MonitorMetrics {
  requests: number;
  errors: number;
  tokens_in: number;
  tokens_out: number;
  latency_p50_ms: number;
  latency_p95_ms: number;
  /** count of requests the histogram was computed from (0 => percentiles unknown) */
  latency_samples: number;
  /** optional per-endpoint breakdown; key = route name like "embed" | "chat" */
  by_endpoint?: Record<string, MonitorEndpointMetrics>;
}

export interface MonitorEndpointMetrics {
  requests: number;
  errors: number;
  tokens_in: number;
  tokens_out: number;
  latency_p50_ms: number;
  latency_p95_ms: number;
  latency_samples: number;
}

export interface MonitorCost {
  /** estimated, USD; pricing from providers.generation[].pricing_tpm */
  generation_input: number;
  generation_output: number;
  generation_cached_input: number;
  /** 0 unless the embedding provider exposes pricing_tpm (currently not) */
  embedding: number;
  total: number;
  /** hash/version of providers.json used for this tally */
  pricing_version: string;
  /** per-generation-provider breakdown; key = GenerationApi.id */
  by_provider?: Record<string, MonitorProviderCost>;
  note?: string;              // e.g. "embedding cost not included"
}

export interface MonitorProviderCost {
  input: number;
  output: number;
  cached_input: number;
  total: number;
  tokens_in: number;
  tokens_out: number;
  model: string;
}

/** Incremental event pushed over SSE; only fields that changed are set. */
export interface MonitorEvent {
  seq: number;
  ts: number;
  session_id: string;
  /** full snapshot on first event / after reconnect; deltas afterwards */
  kind: "snapshot" | "delta" | "heartbeat" | "log" | "error";
  state?: MonitorState;
  last_heartbeat?: number;
  progress?: Partial<MonitorProgress>;
  metrics?: Partial<MonitorMetrics>;
  cost?: Partial<MonitorCost>;
  /** log tail event (optional; prefer the file-tail endpoint for offline) */
  message?: string;
  error?: string;
}

/** Result of GET /api/logs/tail */
export interface LogTailResult {
  status: "success" | "error";
  message?: string;
  lines: string[];
  total_lines: number;
}
