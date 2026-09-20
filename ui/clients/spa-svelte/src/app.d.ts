declare global {
  const __BUILD_DATE__: string;
}

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