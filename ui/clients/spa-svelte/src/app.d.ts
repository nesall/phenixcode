interface SettingsType {
  completionApis: ModelItem[];
  currentApi: string;
}

interface ChatParametersType {
  temperature: number;
  settings: SettingsType;
}

interface ModelItem {
  id: string;
  name: string;
  url: string;
  model: string;
  current: boolean;
  combinedPrice: number;
  reasoning?: {
    parameters: ReasoningParameter[];
  };
}

interface ReasoningParameter {
  name: string;
  value_type: "scalar" | "object";
  value_field?: string;
  values: string[];
  default: string;
  constant_fields?: Record<string, string>;
}

interface ChatMetaInfo {
  content: string;
  summary?: string;
  status: "info" | "running" | "success" | "warning" | "error";
  event_id?: string;
  round?: number;
  call_id?: string;
  tool?: string;
  call_index?: number;
  call_count?: number;
  action?: string;
  outcome_summary?: string;
  args?: Record<string, unknown>;
  args_status?: "missing" | "invalid" | "oversized";
  duration_ms?: number;
  result_preview?: string;
  result_preview_truncated?: boolean;
  interrupted?: boolean;
}

interface ChatMessage {
  replay?: { id: string; session: string; content: string };
  role: "user" | "assistant" | "system";
  content: string;
  _html: string;
  _metaInfoArray?: (string | ChatMetaInfo)[];
  _reasoningText?: string;
}

interface AppInstance {
  value: string; // for dropdowns
  label: string; // for dropdowns
  desc: string; // for dropdowns

  id: string;
  name: string;
  project_id: string;
  host: string;
  port: number;
  status: string;
  last_heartbeat: number;
}

interface ChatSummary {
  id: string;
  title: string;
  project_id: string;
  updated_at: number;
  deleted_at?: number | null;
  preview?: string;
}

interface SavedChatMessage {
  replay?: { id: string; session: string };
  role: "user" | "assistant" | "system";
  content: string;
  _metaInfoArray?: (string | ChatMetaInfo)[];
}

interface PersistedChat {
  id?: string;              // absent/empty => new chat
  project_id: string;
  title: string;
  messages: SavedChatMessage[];
  created_at?: number;
  updated_at?: number;
  deleted_at?: number | null;
  free_chat?: number;
}
