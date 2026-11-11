declare const __BUILD_DATE__: string;

declare interface Window {
  HLJS_CUSTOM?: { hlAuto: (content: string, languages?: string[]) => { value: string } };
  cppApi: {
    setPersistentKey: (key: string, value: string) => Promise<void>;
    getPersistentKey: (key: string) => Promise<string | null>;
    getEnv: (key: string) => Promise<string | null>;

    createProject: (options?: { filename: string }) => Promise<ProjectItem>;
    reconcileProject: (request: {
      project: ProjectItem;
      providers: ProvidersSettings;
      renames: { embedding: Record<string, string>; generation: Record<string, string> };
    }) => Promise<{
      status: string;
      message?: string;
      jsonData?: SettingsJsonType;
      changed: boolean;
      needsReindex: boolean;
      dropped: string[];
      needsSelection: string[];
    }>;
    deleteProject: (project: ProjectItem) => Promise<{ status: string; message: string }>;
    importProject: (projectId: string, configPath: string) => Promise<{ status: string; message: string }>;
    getProjectList: () => Promise<{ status: string, projects: ProjectItem[], message?: string }>;
    saveProject: (project: ProjectItem) => Promise<{ status: string; message: string }>;
    saveProviders: (providers: ProvidersSettings) => Promise<{ status: string; message: string }>;
    startServe: (project: ProjectItem, coreExecutablePath: string, watch: boolean, interval: number) => Promise<{ status: string; message: string }>;
    stopServe: (instanceId: string) => Promise<{ status: string; message: string }>;
    getProviders: () => Promise<{ status: string, providers: ProvidersSettings, message?: string }>;
    getInstances: () => Promise<{ status: string, instances: InstanceItem[], message?: string }>;
    pickSettingsJsonFile: () => Promise<{ project_id: string, path: string } | null>;
    checkPathExists: (path: string) => Promise<{ status: string; message: string }>;
    listDatabaseSources: (params: import("./app").DatabaseRequest) => Promise<import("./app").DatabaseResult<import("./app").DatabasePage>>;
    selectDatabaseWorkingDirectory: (params: import("./app").DatabaseRequest) => Promise<import("./app").DatabaseResult<import("./app").DatabasePage>>;
    getDatabaseSourceChunks: (params: import("./app").DatabaseRequest) => Promise<import("./app").DatabaseResult<import("./app").DatabaseChunks>>;
    readDatabaseSourceFile: (params: import("./app").DatabaseRequest) => Promise<import("./app").DatabaseResult<import("./app").DatabaseFile>>;
    openDatabaseSourceDirectory: (params: import("./app").DatabaseRequest) => Promise<import("./app").DatabaseResult<Record<string, never>>>;
    getMonitorSnapshot(params: { settingsFilePath: string }): Promise<{
      status: "success" | "error";
      message?: string;
      monitor: MonitorSnapshot & { offline?: boolean };
    }>;
    getLogTail(params: {
      settingsFilePath: string;
      file: "main" | "diagnostics";
      offset?: number;
      count?: number;
    }): Promise<LogTailResult>;
    testProvider(params: {
      id: string;
      url: string;
      apiKey: string;
      model: string;
    }): Promise<{ status: "success" | "error"; message?: string }>;
  };
}
