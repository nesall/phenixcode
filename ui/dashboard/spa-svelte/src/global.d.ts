
declare interface Window {
  cppApi: {
    setPersistentKey: (key: string, value: string) => Promise<void>;
    getPersistentKey: (key: string) => Promise<string | null>;

    createProject: () => Promise<ProjectItem>;
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
  };
}