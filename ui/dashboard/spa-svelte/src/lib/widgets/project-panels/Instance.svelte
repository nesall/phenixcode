<script lang="ts">
  import { getContext, onMount, tick } from "svelte";
  import * as icons from "@lucide/svelte";
  import {
    Consts,
    getPersistentKey,
    hardValidateProjectItem,
    helper_startServe,
    helper_stopServe,
    mapProjectToInstance,
    setPersistentKey,
    toaster,
  } from "../../utils";
  import InstanceInfo from "../misc/InstanceInfo.svelte";
  import { projectStore, instances } from "../../store.svelte";

  const jsonData = $derived(projectStore.selected?.jsonData);
  const projectTitle = $derived(projectStore.selected?.jsonData.source.project_title);

  const running = $derived(projectStore.selected && mapProjectToInstance(projectStore.selected, instances));
  const instance = $derived(jsonData && instances.find((a) => a.project_id == jsonData.source.project_id));

  let beingStarted = $state(false);
  let beingStopped = $state(false);
  let beingValidated = $state(false);
  let bWatch = $state(true);
  let watchInterval = $state(60);
  let fileValidationResults = $state<{ status: string; message: string }[]>([]);

  const fetchInstances: () => void = getContext("FetchInstances");
  const fetchProjects: () => void = getContext("FetchProjects");

  onMount(async () => {
    const w = (await getPersistentKey(Consts.WatchForChanges)) || "";
    bWatch = !!w && w !== "0" && w !== "false";
    const i = (await getPersistentKey(Consts.WatchInterval)) || "60";
    watchInterval = parseInt(i, 10) || 60;
  });

  async function onStop() {
    beingStopped = true;
    if (projectStore.selected) {
      const inst = mapProjectToInstance(projectStore.selected, instances);
      if (!inst) {
        toaster.error({ title: "No running instance for this project." });
        return;
      }
      const res = await helper_stopServe(inst.id);
      if (res.status === "success") {
        toaster.success({ title: "Server stopped successfully" });
        projectStore.selected = projectStore.selected;
        fetchInstances();
        fetchProjects();
      } else {
        toaster.error({ title: "Failed to stop", description: res.message });
      }
    } else {
      toaster.error({ title: "No active project" });
    }
    beingStopped = false;
  }

  async function onServe() {
    await validate(true);
    await tick();
    if (0 < fileValidationResults.length) return;
    beingStarted = true;
    const execPath = await getPersistentKey(Consts.EmbedderExecutablePath);
    if (!execPath) {
      toaster.error({ title: "Empty executable path is set in the Settings tab" });
      return;
    }
    if (!projectStore.selected) {
      toaster.error({ title: "No active project" });
      return;
    }
    console.log("onServe", projectStore.selected);
    const res = await helper_startServe(projectStore.selected, execPath, bWatch, watchInterval);
    if (res.status === "success") {
      toaster.success({ title: "Server started successfully" });
        fetchInstances();
      fetchProjects();
    } else {
      toaster.error({ title: "Failed to start", description: res.message });
    }
    beingStarted = false;
  }

  function onWatchChange(e: Event) {
    const ev = e.target as HTMLInputElement;
    bWatch = ev.checked;
    setPersistentKey(Consts.WatchForChanges, ev.checked ? "1" : "0");
  }

  function onIntervalChange(e: Event) {
    const ev = e.target as HTMLInputElement;
    watchInterval = Number(ev.value);
    setPersistentKey(Consts.WatchInterval, ev.value);
  }

  async function onValidate() {
    validate(false);
  }

  async function validate(doNotToastSuccess?: boolean) {
    fileValidationResults = [];
    if (projectStore.selected) {
      beingValidated = true;
      const res = (await hardValidateProjectItem(projectStore.selected)) as { status: string; message: string }[];
      const errors = res.filter((r) => r.status !== "success");
      if (errors.length === 0) {
        if (!doNotToastSuccess) toaster.success({ title: "Project settings file is valid." });
      } else {
        fileValidationResults = errors;
        toaster.error({ title: "Project settings file has errors." });
      }
      beingValidated = false;
    } else {
      toaster.error({ title: "Project not initialized." });
    }
  }
</script>

{#if jsonData}
  <div class="h-full p-4 overflow-auto">
    <form class="w-full">
      <fieldset class="space-y-4">
        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="text-left text-lg font-bold">
            Project: {projectTitle}
          </div>
          <div class="relative">
            {#if running}
              <div class="p-4 bg-success-300-700 rounded text-lg flex items-center justify-center space-x-4">
                <icons.CircleCheck class="mr-2" />
                <span>An instance of this project is currently <strong>running</strong>.</span>
                <button
                  type="button"
                  class="btn preset-filled font-bold"
                  title="Stop currently running instance."
                  onclick={onStop}
                  disabled={beingStopped}
                >
                  {#if beingStopped}
                    Stopping...
                  {:else}
                    STOP
                  {/if}
                </button>
              </div>
              <InstanceInfo {instance} />
              {#if beingStopped}
                <div class="absolute bg-surface-500/50 left-0 top-0 w-full h-full"></div>
              {/if}
            {:else}
              <div class="flex flex-col space-y-4">
                <div class="p-4 bg-error-300-700 rounded text-lg flex items-center justify-center space-x-4">
                  <icons.CircleX class="mr-2" />
                  <span>No instance of this project is currently <strong>running</strong>.</span>
                </div>
                <div class="flex flex-col space-y-2">
                  <div class="flex items-center space-x-1">
                    <span class="font-semibold">File location:</span>
                    <span class="font-semibold2"> {projectStore.selected?.settingsFilePath} </span>
                  </div>
                  <div class="flex flex-col">
                    <div class="flex">
                      <button type="button" class="btn btn-sm preset-filled-primary-500" onclick={onValidate}>
                        Validate Settings File
                      </button>
                    </div>
                    {#if fileValidationResults.length}
                      <ul class="text-left rounded border border-2 border-error-500 p-2 m-2">
                        {#each fileValidationResults as ferror}
                          <li>&#x274C;{ferror.message}</li>
                        {/each}
                      </ul>
                    {/if}
                  </div>
                </div>
                <div class="flex flex-col justify-start space-y-4 rounded border border-surface-200-800 p-4">
                  <div class="flex items-center space-x-2">
                    <input
                      type="checkbox"
                      class="checkbox"
                      id="serve-watch"
                      checked={bWatch}
                      onchange={onWatchChange}
                      disabled={beingStarted || beingValidated || 0 < fileValidationResults.length}
                    />
                    <label for="serve-watch">Watch for changes</label>
                  </div>
                  <div class="flex items-center space-x-2">
                    <input
                      type="number"
                      class="input max-w-32"
                      step="1"
                      min="5"
                      max="2000"
                      id="serve-watch-interval"
                      value={watchInterval}
                      onchange={onIntervalChange}
                      disabled={!bWatch || beingStarted || beingValidated || 0 < fileValidationResults.length}
                    />
                    <label for="serve-watch-interval">Watch interval (seconds)</label>
                  </div>
                  <div class="flex">
                    <button
                      type="button"
                      class="btn preset-filled font-bold min-w-32"
                      title="Start an instance to serve"
                      onclick={onServe}
                      disabled={beingStarted || 0 < fileValidationResults.length}
                    >
                      {#if beingStarted}
                        Starting...
                      {:else}
                        Start Server Instance
                      {/if}
                    </button>
                  </div>
                </div>
                <!-- <div class="flex items-center space-x-2">
                  <button
                    type="button"
                    class="btn preset-outlined font-bold min-w-32"
                    title="Stop currently running instance."
                    onclick={onVersion}
                  >
                    Version
                  </button>
                  <span>Fetch version info</span>
                </div> -->
              </div>
            {/if}
          </div>
        </div>
      </fieldset>
    </form>
  </div>
{/if}
