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
  tier_2_refactor: AutoRouterTierRule;
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
  max_context_tokens: number;
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
  // apis: EmbeddingApi[];
  enabled_providers: string[];
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
