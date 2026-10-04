<script lang="ts">
  import { onMount } from "svelte";
  import { Consts, getEnv, getPersistentKey, helper_checkPathExists, setPersistentKey } from "../utils";

  let coreExecutablePathInput = $state("");
  let projectsFolderPathInput = $state("");
  let invalidExecPathMessage = $state("");
  let invalidPrjPathMessage = $state("");

  let projectsFolderEnv: string | null = "";

  onMount(async () => {
    projectsFolderEnv = await getEnv(Consts.Env.PHENIXCODE_PROJECTS_FOLDER);
    const path = await getPersistentKey(Consts.CoreExecutablePath);
    coreExecutablePathInput = path || "./phenixcode-core";
    if (!path) {
      setPersistentKey(Consts.CoreExecutablePath, coreExecutablePathInput);
    }
    console.log("coreExecutablePathInput", $state.snapshot(coreExecutablePathInput));

    initProjectPath();
  });

  async function initProjectPath() {
    const defProjPath = "./phenixcode-projects";
    const path = await getPersistentKey(Consts.ProjectsFolderPath);
    projectsFolderPathInput = path || projectsFolderEnv || defProjPath;
    console.log("projectsFolderPathInput", $state.snapshot(projectsFolderPathInput));
  }

  function onCorePathChange(e: Event) {
    const ev = e as InputEvent;
    if (ev && ev.target) {
      setPersistentKey(Consts.CoreExecutablePath, (ev.target as HTMLInputElement).value);
      coreExecutablePathInput = (ev.target as HTMLInputElement).value;
      isValidPath(coreExecutablePathInput).then((res: any) => {
        if (res.status !== "success") {
          invalidExecPathMessage = res.message || "Invalid executable path";
        } else {
          invalidExecPathMessage = "";
        }
      });
    }
  }

  function onProjectsPathChange(e: Event) {
    const ev = e as InputEvent;
    if (ev && ev.target) {
      const val = (ev.target as HTMLInputElement).value;
      setPersistentKey(Consts.ProjectsFolderPath, val);
      projectsFolderPathInput = val;
      isValidPath(projectsFolderPathInput).then((res: any) => {
        if (res.status !== "success") {
          invalidPrjPathMessage = res.message || "Invalid projects folder path";
        } else {
          invalidPrjPathMessage = "";
        }
      });
    }
  }

  async function onResetProjPath() {
    await setPersistentKey(Consts.ProjectsFolderPath, "");
    initProjectPath();
    invalidPrjPathMessage = "";
  }

  async function isValidPath(path: string | undefined | null) {
    if (!path) return false;
    return await helper_checkPathExists(path);
  }
</script>

<div class="flex flex-col gap-4">
  <div class="text-right text-xs">Build date: {__BUILD_DATE__}</div>
  <label class="label">
    <span class="label-text">PhenixCode Executable Path</span>
    <div class="flex items-center space-x-1">
      <input
        type="text"
        id="input-core-executable-path"
        class="input max-w-xl {!!invalidExecPathMessage ? 'outline-2 outline-red-500' : ''}"
        oninput={onCorePathChange}
        value={coreExecutablePathInput}
      />
    </div>
    {#if !!invalidExecPathMessage}
      <div class="text-xs italic text-error-700-300">{invalidExecPathMessage}</div>
    {/if}
  </label>
  <label class="label">
    <span class="label-text">PhenixCode Projects Path</span>
    <div class="flex items-center space-x-1">
      <input
        type="text"
        id="input-projects-path"
        class="input max-w-xl {!!invalidPrjPathMessage ? 'outline-2 outline-red-500' : ''}"
        oninput={onProjectsPathChange}
        value={projectsFolderPathInput}
      />
      <button type="button" class="btn preset-tonal" onclick={onResetProjPath}>Reset to default</button>
    </div>
    {#if !!invalidPrjPathMessage}
      <div class="text-xs italic text-error-700-300">{invalidPrjPathMessage}</div>
    {/if}
  </label>
</div>

<style>
  .label {
    text-align: left;
  }
</style>
