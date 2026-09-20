<script lang="ts">
  import * as icons from "@lucide/svelte";
  import { Dialog, Portal } from "@skeletonlabs/skeleton-svelte";
  import { onMount } from "svelte";
  import {
    apiOptionsGroupedSorted,
    apiUrl,
    clog,
    Consts,
    fnv1a64,
    getLastLogs,
    getPersistentKey,
    setPersistentKey,
    stripCommonPrefix,
    toaster,
  } from "../utils";
  import Dropdown from "./Dropdown.svelte";
  import {
    messages,
    settings,
    temperature,
    instances,
    curInstance,
    bApisGroupedByLabel,
    bApisSortedByPrice,
  } from "../store";
  import { slide } from "svelte/transition";

  interface Props {
    fetchInstances: () => Promise<void>;
    onClear: () => void;
  }
  let { fetchInstances, onClear = () => {} }: Props = $props();

  let openState = $state(false);
  let curTheme = $state("cerberus");
  let serverUrl = $state("127.0.0.1:8590");

  let openLogsState = $state(false);
  let openStatsState = $state(false);
  let showProjectsAndSources = $state(false);

  let embedderExecutablePath = $state<string>("");
  let embedderSettingsFilePaths = $state<string[]>([]);

  let mapIdToRunningEmbedder: Record<string, boolean> = $state({});
  let mapIdToStartInitiated: Record<string, boolean> = $state({});
  let mapPathToId: Record<string, string> = $state({});
  let mapPathToAppKey: Record<string, string> = $state({});

  interface StatsType {
    sources: {
      by_directory: Record<string, number>;
      by_language: Record<string, number>;
      top_files: {
        chunks: number;
        language: string;
        last_modified: number;
        lines: number;
        path: string;
        size_bytes: number;
      }[];
      total_files: number;
      total_lines: number;
      total_size_bytes: number;
    };
    total_chunks: number;
    vector_count: number;
  }

  let statsData: StatsType | undefined = $state();

  const themeOptions = [
    { label: "Cerberus", value: "cerberus" },
    { label: "Concord", value: "concord" },
    { label: "Hamlindigo", value: "hamlindigo" },
    { label: "Terminus", value: "terminus" },
    { label: "Mona", value: "mona" },
    { label: "Wintry", value: "wintry" },
    { label: "Nosh", value: "nosh" },
    { label: "Vox", value: "vox" },
    { label: "Rocket", value: "rocket" },
  ];

  const apiOptions = $derived(
    apiOptionsGroupedSorted(
      $settings.completionApis.map((a) => ({
        value: a.id,
        label: a.name,
        group: a.name,
        desc: `${a.model} (cost: ${Number(a.combinedPrice).toFixed(2)})`,
        _price: a.combinedPrice,
      })),
      $bApisSortedByPrice,
      $bApisGroupedByLabel,
    ),
  );

  const curApi = $derived(
    -1 != $settings.completionApis.findIndex((a) => a.current)
      ? $settings.completionApis[$settings.completionApis.findIndex((a) => a.current)].id
      : "",
  );

  let hasCppApi = $state(!!window.cppApi);

  onMount(async () => {
    console.log("Toolbar onMount");
    try {
      const savedTheme = await getPersistentKey(Consts.ThemeKey);
      if (savedTheme && -1 != themeOptions.findIndex((a) => a.value == savedTheme)) {
        document.documentElement.setAttribute("data-theme", savedTheme);
        curTheme = savedTheme;
      }
      const savedDarkLight = await getPersistentKey(Consts.DarkOrLightKey);
      setDarkOrLight(savedDarkLight);

      // serverUrl = await getPersistentKey(Consts.ServerUrlKey) || serverUrl;
      if (window.cppApi) {
        window.cppApi.getServerUrl().then((url) => {
          if (url) serverUrl = url.replace("/api/health", "");
          console.log("Fetched serverUrl from cppApi:", url, serverUrl);
        });
      }

      const sortedStr = await getPersistentKey(Consts.ApiOptionsSortedKey);
      $bApisSortedByPrice = sortedStr === "1";
      const groupedStr = await getPersistentKey(Consts.ApiOptionsGroupedKey);
      $bApisGroupedByLabel = groupedStr === "1";

      const embExecPath = await getPersistentKey(Consts.EmbedderExecutablePath);
      embedderExecutablePath = embExecPath || "";
      const embSettingsFiles = (await getPersistentKey(Consts.EmbedderSettingsFilePaths)) || "[]";
      embedderSettingsFilePaths = JSON.parse(embSettingsFiles);
    } catch (e) {
      clog("Unable to access localStorage", e);
    }
  });

  $effect(() => {
    if ($settings) clog("Toolbar $settings changed:", $state.snapshot($settings));
    if (curApi) clog("Toolbar curApi changed:", $state.snapshot(curApi));
  });

  function setDarkOrLight(dl: string | null) {
    clog("setDarkOrLight", dl);
    const themeLink = document.getElementById("hljs-theme") as HTMLLinkElement;
    const htmlEl = document.documentElement;
    if (dl === "dark") {
      htmlEl.setAttribute("data-mode", "dark");
      themeLink.href = "hljs/vs2015.css";
    } else {
      htmlEl.setAttribute("data-mode", "light");
      themeLink.href = "hljs/vs.css";
    }
  }

  function onToggleDarkMode() {
    const htmlEl = document.documentElement;
    const newDl = htmlEl.getAttribute("data-mode") === "dark" ? "light" : "dark";
    setDarkOrLight(newDl);
    try {
      setPersistentKey(Consts.DarkOrLightKey, newDl);
    } catch (e) {
      clog("Unable to access localStorage", e);
    }
  }

  async function modalClose() {
    openState = false;
  }

  function onThemeChange(i: number, theme: string) {
    document.documentElement.setAttribute("data-theme", theme);
    curTheme = theme;
    setPersistentKey(Consts.ThemeKey, theme);
  }

  function onModelChange(i: number, modelId: string) {
    try {
      setPersistentKey(Consts.CurrentApiKey, modelId);
      $settings.completionApis = $settings.completionApis.map((api) => ({
        ...api,
        current: api.id === modelId,
      }));
      clog("Toolbar.onModelChange", modelId);
      $settings.currentApi = modelId;
    } catch (e) {
      clog("Unable to access localStorage", e);
    }
  }

  function onTempChange(e: Event) {
    const t = (e.target as HTMLInputElement).value;
    setPersistentKey(Consts.TemperatureKey, t);
    $temperature = Number(t);
  }

  function describeTemperature(temp: number): string {
    clog("describeTemperature", temp);
    if (temp <= 0.2) return "Precise";
    if (temp <= 0.5) return "Balanced";
    if (temp <= 0.8) return "Creative";
    return "Unpredictable";
  }

  function onDownloadChat() {
    clog("onDownloadChat");
    const chat = document.getElementById("chat-messages");
    if (!chat) return;
    let text = "";
    chat.querySelectorAll(".message").forEach((msg) => {
      const role = msg.getAttribute("data-role") || "user";
      const contentEl = msg.querySelector(".message-content");
      const content = contentEl ? contentEl.textContent || "" : "";
      text += role.toUpperCase() + ":\n" + content + "\n\n";
    });
    const blob = new Blob([text], { type: "text/plain" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `chat_${new Date().toISOString()}.txt`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  }

  function onServerUrlChange(e: Event) {
    // try {
    //   serverUrl = (e.target as HTMLInputElement).value;
    //   setPersistentKey(Consts.ServerUrlKey, serverUrl);
    // } catch (e) {
    //   clog("Unable to access localStorage", e);
    // }
  }

  async function onSaveConnection() {
    try {
      if (!window.cppApi) {
        alert("Changing server host is not supported in web mode.");
        return;
      }
      let url = `${serverUrl}/api/health`;
      if (!url.startsWith("http")) {
        url = "http://" + url;
      }
      const res = await window.cppApi.setServerUrl(url);
      clog("onSaveConnection", res);
    } catch (error) {
      clog("Saving connection failed:", error);
      alert(`Save failed: ${error instanceof Error ? error.message : "Unknown error"}`);
    }
  }

  function onClearInternal() {
    messages.set([]);
    onClear();
  }

  function onViewStats() {
    fetch(apiUrl("/api/stats"))
      .then((res) => res.json())
      .then((data) => {
        console.log("STATS", data);
        if (data.error) {
          throw new Error(data.error);
        }
        statsData = data;
        if (statsData) {
          if (statsData.sources) {
            let dirs = Object.entries((statsData.sources.by_directory = statsData.sources.by_directory || {}));
            stripCommonPrefix(dirs.map((d) => d[0])).forEach((stripped: string, i: number) => {
              dirs[i][0] = stripped;
            });
            statsData.sources.by_directory = Object.fromEntries(dirs);

            let files = statsData.sources.top_files || [];
            let paths = files.map((f) => f.path);
            let strippedPaths = stripCommonPrefix(paths);
            files.forEach((f, i) => {
              f.path = strippedPaths[i];
            });
            statsData.sources.top_files = files;
            console.log("Top Files:", statsData.sources.top_files);
          }
        }
        openStatsState = true;
      })
      .catch((er) => {
        console.log(er);
        toaster.error({ title: `Failed to fetch stats: ${er instanceof Error ? er.message : "Unknown error"}` });
      });
  }

  async function onProjectChange(i: number, modelId: string) {
    if (window.cppApi) {
      const res = await window.cppApi.setServerUrl($instances[i].desc);
      console.log("onProjectChange", res);
    } else {
      alert("Switching backends is not supported in web mode.");
    }
  }

  async function onToggleSorting() {
    clog("onToggleSorting", $bApisSortedByPrice);
    setPersistentKey(Consts.ApiOptionsSortedKey, $bApisSortedByPrice ? "1" : "0");
  }

  async function onToggleGrouping() {
    clog("onToggleGrouping", $bApisGroupedByLabel);
    setPersistentKey(Consts.ApiOptionsGroupedKey, $bApisGroupedByLabel ? "1" : "0");
  }

  function onAddNewSettingsPath() {
    if (embedderSettingsFilePaths.length < 5) {
      const input = document.querySelector<HTMLInputElement>("#new-settings-input");
      const path = input?.value.trim();
      if (!path) return;
      if (embedderSettingsFilePaths.findIndex((p) => p === path) !== -1) {
        toaster.error({ title: "This settings file path is already added." });
        return;
      }
      embedderSettingsFilePaths = [...embedderSettingsFilePaths, path];
      setPersistentKey(Consts.EmbedderSettingsFilePaths, JSON.stringify(embedderSettingsFilePaths));
      input!.value = "";
    } else {
      toaster.error({ title: "Maximum of 5 embedder settings file paths allowed." });
    }
  }

  async function fetchProjectIdForSettingsPath(path: string): Promise<string | null> {
    if (!window.cppApi) return null;
    try {
      const projectId = await window.cppApi.getSettingsFileProjectId(path);
      return projectId;
    } catch (error) {
      console.log("fetchProjectIdForSettingsPath failed:", error);
      return null;
    }
  }

  async function updateRunningEmbedderStatuses() {
    console.log("updateRunningEmbedderStatuses");
    mapIdToRunningEmbedder = {};
    for (const path of embedderSettingsFilePaths) {
      const configId = await fetchProjectIdForSettingsPath(path);
      console.log("embedderSettingsFilePaths", path, configId);
      if (configId) {
        mapPathToId[path] = configId;
        mapIdToRunningEmbedder[configId] = false;
        for (const inst of $instances) {
          if (inst.project_id === configId) {
            mapIdToRunningEmbedder[configId] = true;
            mapIdToStartInitiated[configId] = false;
            console.log("Running embedder found for configId:", configId);
            break;
          }
        }
      }
    }
  }

  async function onRunStopEmbedder(index: number) {
    if (!window.cppApi) {
      alert("Embedder management is not supported in web mode.");
      return;
    }
    const path = embedderSettingsFilePaths[index];
    if (!path) {
      toaster.error({ title: "Please provide a valid settings.json file path." });
      return;
    }
    const configId = await fetchProjectIdForSettingsPath(path);
    if (!configId) {
      toaster.error({ title: "Unable to fetch project ID for the provided settings.json file." });
      return;
    }
    mapPathToId[path] = configId;
    if (mapIdToRunningEmbedder[configId]) {
      await onStopEmbedder(configId, path);
    } else {
      await onStartEmbedder(configId, path);
    }
  }

  async function onStartEmbedder(configId: string, path: string) {
    mapIdToRunningEmbedder[configId] = false;
    for (const inst of $instances) {
      if (inst.project_id === configId) {
        mapIdToRunningEmbedder[configId] = true;
        mapIdToStartInitiated[configId] = false;
        return;
      }
    }
    try {
      const res = await window.cppApi.startEmbedder(embedderExecutablePath, path);
      console.log("onStartEmbedder", res);
      if (res.status != "success") {
        throw new Error(res.message || "Unknown error");
      }
      mapPathToAppKey[path] = res.appKey;
      mapIdToStartInitiated[configId] = true;
      toaster.success({ title: `Embedder process started` });
      toaster.success({ title: `Please wait a few moments then refresh the connection` });
    } catch (error) {
      console.log("Starting embedder failed:", error);
      toaster.error({ title: `Failed to start: ${error instanceof Error ? error.message : "Unknown error"}` });
    }
  }

  async function onStopEmbedder(configId: string, path: string) {
    const appKey = mapPathToAppKey[path];
    if (!appKey) {
      toaster.error({ title: "Not authorized to stop an externally started server." });
      return;
    }
    try {
      let host = "";
      let port = 0;
      let found = false;
      for (const inst of $instances) {
        if (inst.project_id === configId) {
          host = inst.host;
          port = Number(inst.port);
          found = true;
        }
      }
      if (!found) {
        toaster.error({ title: "Unable to find running embedder" });
        return;
      }
      if (!host || !port) {
        toaster.error({ title: "Invalid embedder host or port fetched." });
        return;
      }

      const res = await window.cppApi.stopEmbedder(appKey, host, port);
      console.log("onStopEmbedder", res);
      if (res.status != "success") {
        throw new Error(res.message || "Unknown error");
      }
      mapIdToStartInitiated[configId] = false;
      toaster.success({ title: `Embedder process stopped` });
      await fetchInstances();
      updateRunningEmbedderStatuses();
    } catch (error) {
      console.log("Stopping embedder failed:", error);
      toaster.error({ title: `Failed to stop: ${error instanceof Error ? error.message : "Unknown error"}` });
    }
  }
</script>

<div class="toolbar flex space-x-2 items-center w-full bg-surface-100-900 px-2 py-1 rounded">
  <!-- <img src="/logo.png" alt="Logo" class="h-6 w-6" />
  <span class="text-sm">Project</span> -->
  <div class="flex items-center space-x-0">
    <span class="text-xs text-surface-700-900" title="Select Project/Instance">
      <Dropdown
        values={$instances}
        value={$curInstance}
        onChange={onProjectChange}
        classNames="py-[2px] min-w-[10rem] preset-tonal"
        onAboutToShow={fetchInstances}
        dropdownWidth="max-content"
      />
    </span>
  </div>
  <span class="w-4">&nbsp;</span>
  <div class="flex space-x-1 items-center ml-auto">
    <button
      type="button"
      class="btn btn-sm btn-icon hover:preset-tonal"
      aria-label="Download"
      onclick={onDownloadChat}
      title="Download chat as text file"
    >
      <icons.Download />
    </button>

    <span class="vr h-6"></span>

    <button
      type="button"
      class="btn btn-sm btn-icon hover:preset-tonal"
      aria-label="Statistics"
      onclick={onViewStats}
      title="View statistics"
    >
      <icons.Info />
    </button>
    <button
      type="button"
      class="btn btn-sm btn-icon hover:preset-tonal"
      aria-label="Settings"
      onclick={async () => {
        curTheme = document.documentElement.getAttribute("data-theme") || "cerberus";
        await fetchInstances();
        updateRunningEmbedderStatuses();
        openState = true;
      }}
      title="Edit settings"
    >
      <icons.Settings />
    </button>
    <button
      type="button"
      class="btn btn-sm btn-icon hover:preset-tonal"
      aria-label="Dark/Light Mode"
      onclick={onToggleDarkMode}
    >
      <icons.SunMoon />
    </button>
    <span class="vr h-6"></span>
    <button
      type="button"
      class="btn btn-sm btn-icon flex hover:preset-tonal"
      aria-label="New Chat"
      onclick={onClearInternal}
      title="Clear chat"
    >
      <icons.Trash2 />
    </button>
  </div>
</div>

<Dialog open={openState} onOpenChange={(e) => (openState = e.open)}>
  <Portal>
    <Dialog.Positioner class="fixed inset-0 z-50 flex justify-center items-center">
      <Dialog.Content class="card bg-surface-100-900 w-xl p-4 space-y-2 shadow-xl">
        <Dialog.Title>
          <div class="flex flex-row items-center justify-between">
            <span class="text-lg font-bold">Settings</span>
            <span class="text-xs">Build date: {__BUILD_DATE__}</span>
          </div>
        </Dialog.Title>
        <hr class="hr" />
        <Dialog.Description>
          <div class="flex flex-col space-y-4">
            <div class="flex flex-col space-x-2 items-left w-full">
              <span class="whitespace-nowrap">Theme:</span>
              <Dropdown values={themeOptions} value={curTheme} onChange={onThemeChange} />
            </div>
            <div class="flex flex-col space-x-2 items-left w-full max-h-[10rem]">
              {#if $settings.completionApis.length === 0}
                <span class="italic text-surface-500">No models available</span>
              {:else}
                <span class="whitespace-nowrap">Default model:</span>
                <Dropdown values={apiOptions} value={curApi} onChange={onModelChange} dropdownClassNames="text-sm" />
                <div class="flex items-center space-x-4">
                  <label class="flex items-center space-x-1">
                    <input
                      class="input checkbox w-4 h-4"
                      type="checkbox"
                      bind:checked={$bApisGroupedByLabel}
                      onchange={onToggleGrouping}
                    />
                    <span>Group by provider</span>
                  </label>
                  <label class="flex items-center space-x-1">
                    <input
                      class="input checkbox w-4 h-4"
                      type="checkbox"
                      bind:checked={$bApisSortedByPrice}
                      onchange={onToggleSorting}
                    />
                    <span>Sort by cost</span>
                  </label>
                </div>
              {/if}
            </div>
            <div class="flex flex-col items-left w-full">
              <div class="flex justify-between">
                <span class="whitespace-nowrap">Temperature:</span>
                <span class="whitespace-nowrap">({describeTemperature($temperature)})</span>
              </div>
              <input
                type="number"
                min="0"
                max="1.0"
                step="0.1"
                class="input w-full px-2"
                value={$temperature}
                onchange={onTempChange}
              />
            </div>
            {#if hasCppApi}
              <div class="flex flex-col space-x-2 items-left w-full">
                <span class="whitespace-nowrap">Server:</span>
                <div class="flex items-center space-x-2">
                  <input type="url" class="input" bind:value={serverUrl} onchange={onServerUrlChange} />
                  <button type="button" class="btn preset-tonal flex items-center" onclick={onSaveConnection}>
                    <icons.CircleCheckBig size={16} />
                    Save
                  </button>
                </div>
              </div>
            {/if}
            <div class="flex space-x-2 items-center">
              <button
                type="button"
                class="btn hover:preset-tonal pl-0 mb-[-10px]"
                aria-label="Manage"
                title="Manage Projects and Sources"
                onclick={() => {
                  showProjectsAndSources = !showProjectsAndSources;
                }}
              >
                <span>Edit Projects and Sources</span>
                {#if showProjectsAndSources}
                  <icons.ChevronUp size={16} />
                {:else}
                  <icons.ChevronDown size={16} />
                {/if}
              </button>
            </div>
            {#if showProjectsAndSources}
              <div class="flex flex-col space-y-2 items-left w-full text-sm" transition:slide>
                <div class="flex flex-col space-y-1">
                  <span class="whitespace-nowrap label">Embedder executable location:</span>
                  <input
                    type="text"
                    class="input w-full px-2 text-xs"
                    value={embedderExecutablePath}
                    placeholder="e.g. ./embeddings_cpp.exe or ./embeddings_cpp"
                    onchange={(e) => {
                      embedderExecutablePath = (e.target as HTMLInputElement).value;
                      setPersistentKey(Consts.EmbedderExecutablePath, embedderExecutablePath);
                    }}
                  />
                </div>
                <div class="flex flex-col space-y-1">
                  <span class="whitespace-nowrap label">Embedder settings.json locations:</span>
                  {#each embedderSettingsFilePaths as path, index}
                    <div class="flex items-center space-x-0">
                      <button
                        type="button"
                        class="btn btn-sm btn-icon preset-tonal mr-1 hover:text-error-500"
                        aria-label="Remove"
                        title="Remove this embedder settings file path"
                        onclick={() => {
                          embedderSettingsFilePaths = embedderSettingsFilePaths.filter((_, i) => i !== index);
                          delete mapPathToId[path];
                          delete mapIdToRunningEmbedder[mapPathToId[path]];
                          setPersistentKey(Consts.EmbedderSettingsFilePaths, JSON.stringify(embedderSettingsFilePaths));
                        }}
                        disabled={mapIdToRunningEmbedder[mapPathToId[path]] || mapIdToStartInitiated[mapPathToId[path]]}
                      >
                        <icons.Trash2 size={16} />
                      </button>
                      <span class=" w-full px-2 text-xs">{path}</span>
                      <button
                        type="button"
                        class="btn btn-sm ml-1 hover:font-bold2 min-w-20
                          {mapIdToRunningEmbedder[mapPathToId[path]]
                          ? 'preset-filled-error-500'
                          : 'preset-tonal-primary'}"
                        aria-label="Start/Stop"
                        title="Start/stop embedder with this settings file path"
                        onclick={() => onRunStopEmbedder(index)}
                        disabled={mapIdToStartInitiated[mapPathToId[path]]}
                      >
                        {#if mapIdToRunningEmbedder[mapPathToId[path]]}
                          <span class="">Stop</span>
                          <icons.OctagonX size={16} />
                        {:else}
                          <span class="">Run</span>
                          <icons.Play size={16} />
                        {/if}
                      </button>
                    </div>
                  {/each}
                  <div class="flex items-center space-x-2 mt-1">
                    <input
                      type="text"
                      id="new-settings-input"
                      class="input w-full px-2 text-xs"
                      placeholder="e.g. ./path/to/settings.json"
                    />
                    <button
                      type="button"
                      class="btn btn-sm preset-filled-primary-500 ml-auto"
                      aria-label="Add"
                      title="Add another embedder settings file path"
                      onclick={onAddNewSettingsPath}
                    >
                      Add new settings path
                    </button>
                  </div>
                </div>
              </div>
            {/if}
          </div>
        </Dialog.Description>
        <footer class="flex justify-end gap-4 pt-4">
          <button
            type="button"
            class="mr-auto btn btn-sm hover:text-primary-500 flex-1 justify-start"
            onclick={() => (openLogsState = true)}
          >
            Show logs...
          </button>
          <button type="button" class="btn preset-filled flex-1" onclick={modalClose}> Finish </button>
        </footer>
      </Dialog.Content>
    </Dialog.Positioner>
  </Portal>
</Dialog>

<Dialog open={openLogsState} onOpenChange={(e) => (openLogsState = e.open)}>
  <Portal>
    <Dialog.Backdrop class="" />
    <Dialog.Positioner class="fixed inset-0 z-50 flex justify-center items-center">
      <Dialog.Content class="card bg-surface-100-900 w-xl p-4 space-y-2 shadow-xl">
        <Dialog.Title class="text-lg font-bold">Logs</Dialog.Title>
        <hr class="hr" />
        <Dialog.Description>
          <div class="whitespace-pre-wrap font-mono text-xs max-h-[60vh] overflow-y-auto">
            <pre id="log-output">
          {#each getLastLogs() as log}<div class="flex items-center space-x-1"><span>{log.date}</span><span>&nbsp;</span
                  ><span>{log.data}</span></div>{/each}
        </pre>
          </div>
        </Dialog.Description>
        <Dialog.CloseTrigger class="btn preset-filled w-full">Close</Dialog.CloseTrigger>
      </Dialog.Content>
    </Dialog.Positioner>
  </Portal>
</Dialog>

<Dialog open={openStatsState} onOpenChange={(e) => (openStatsState = e.open)}>
  <Portal>
    <Dialog.Backdrop class="" />
    <Dialog.Positioner class="fixed inset-0 z-50 flex justify-center items-center">
      <Dialog.Content class="card bg-surface-100-900 w-xl p-4 space-y-2 shadow-xl">
        <Dialog.Title class="text-lg font-bold">Stats</Dialog.Title>
        <hr class="hr" />
        <Dialog.Description>
          <div class="whitespace-pre-wrap font-mono0 text-xs max-h-[60vh] overflow-y-auto">
            <div class="flex flex-col">
              <!-- <span class="font-semibold uppercase">Sources:</span> -->

              <div class="ml-4 flex flex-col space-y-1 mt-2">
                <div class="flex items-center gap-4">
                  <span class="flex-1 text-right">Total chunks</span>
                  <span class="flex-1">{statsData?.total_chunks}</span>
                </div>

                <div class="flex items-center gap-4">
                  <span class="flex-1 text-right">Vector count</span>
                  <span class="flex-1">{statsData?.vector_count}</span>
                </div>

                <div class="flex items-center gap-4">
                  <span class="flex-1 text-right">Total files</span>
                  <span class="flex-1">{statsData?.sources.total_files}</span>
                </div>

                <div class="flex items-center gap-4">
                  <span class="flex-1 text-right">Total lines</span>
                  <span class="flex-1">{statsData?.sources.total_lines}</span>
                </div>

                <div class="flex items-center gap-4">
                  <span class="flex-1 text-right">Total size (bytes)</span>
                  <span class="flex-1">{statsData?.sources.total_size_bytes}</span>
                </div>

                <div class="flex flex-col whitespace-normal">
                  <span class="font-semibold mt-2">By Directory:</span>
                  <div class="ml-4">
                    {#each Object.entries(statsData?.sources.by_directory || {}) as [dir, count]}
                      <div class="flex items-center gap-4">
                        <span class="flex-1 text-right">{dir}</span>
                        <span class="flex-1">{count}</span>
                      </div>
                    {/each}
                  </div>
                </div>

                <div class="flex flex-col mt-2">
                  <span class="font-semibold">By Language:</span>
                  <div class="ml-4">
                    {#each Object.entries(statsData?.sources.by_language || {}) as [lang, count]}
                      <div class="flex items-center gap-4">
                        <span class="flex-1 text-right">{lang}</span>
                        <span class="flex-1">{count}</span>
                      </div>
                    {/each}
                  </div>
                </div>

                <div class="flex flex-col mt-2 whitespace-normal">
                  <span class="font-semibold">Top Files:</span>
                  <div class="ml-4">
                    {#each statsData?.sources.top_files || [] as file}
                      <div class="flex flex-col border-b border-surface-200-800 py-1">
                        <div class="flex items-center gap-4">
                          <span class="flex-1 text-right">Path</span>
                          <span class="flex-1 wrap-anywhere">{file.path}</span>
                        </div>
                        <div class="flex items-center gap-4">
                          <span class="flex-1 text-right">Language</span>
                          <span class="flex-1">{file.language}</span>
                        </div>
                        <div class="flex items-center gap-4">
                          <span class="flex-1 text-right">Chunks</span>
                          <span class="flex-1">{file.chunks}</span>
                        </div>
                        <div class="flex items-center gap-4">
                          <span class="flex-1 text-right">Lines</span>
                          <span class="flex-1">{file.lines}</span>
                        </div>
                        <div class="flex items-center gap-4">
                          <span class="flex-1 text-right">Size (bytes)</span>
                          <span class="flex-1">{file.size_bytes}</span>
                        </div>
                        <div class="flex items-center gap-4">
                          <span class="flex-1 text-right">Last Modified</span>
                          <span class="flex-1">{new Date(file.last_modified * 1000).toLocaleString()}</span>
                        </div>
                      </div>
                    {/each}
                  </div>
                </div>
              </div>
            </div>
          </div>
        </Dialog.Description>
        <Dialog.CloseTrigger class="btn preset-filled w-full">Close</Dialog.CloseTrigger>
      </Dialog.Content>
    </Dialog.Positioner>
  </Portal>
</Dialog>

<style>
  .text-right {
    font-weight: 300;
  }
</style>
