declare const __BUILD_DATE__: string;

type ChatResult =
  | { status: "success"; id?: string; deleted?: number; chats?: ChatSummary[]; chat?: PersistedChat }
  | { status: "error"; message: string };

declare interface Window {
  apiServerUrl: string | undefined;
  cppApi: {
    setServerUrl: (url: string | undefined) => any;
    getServerUrl: () => Promise<string | undefined>;
    setPersistentKey: (key: string, value: string) => Promise<void>;
    getPersistentKey: (key: string) => Promise<string | null>;
    getSettingsFileProjectId: (path: string) => Promise<string | null>;
    startEmbedder: (executablePath: string, settingsFilePath: string) => Promise<{ status: string; message: string, appKey: string, projectId: string }>;
    stopEmbedder: (appKey: string, host: string, port: number) => Promise<{ status: string; message: string }>;
    listChats: (projectId?: string) => Promise<ChatResult>;
    saveChat: (chat: PersistedChat) => Promise<ChatResult>;
    getChat: (id: string) => Promise<ChatResult>;
    deleteChat: (id: string) => Promise<ChatResult>;
  };

  HLJS_CUSTOM: {
    initHljs: () => any;
    hlAuto: (s: string, lang?: string) => string;
  }
}