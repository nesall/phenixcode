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
}

interface ChatMessage {
  role: "user" | "assistant" | "system";
  content: string;
  _html: string;
  _metaInfoArray?: string[];
  _metaVisible?: boolean;
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
}

interface SavedChatMessage {
  role: "user" | "assistant" | "system";
  content: string;
  _metaInfoArray?: string[];
}

interface PersistedChat {
  id?: string;              // absent/empty => new chat
  project_id: string;
  title: string;
  messages: SavedChatMessage[];
  created_at?: number;
  updated_at?: number;
}
