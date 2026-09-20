<script lang="ts">
  import { onMount } from "svelte";
  import { Consts, getPersistentKey, helper_checkPathExists, setPersistentKey } from "../utils";

  let embedderExecutablePath = $state("");
  let valid = $state(false);
  let invalidMessage = $state("");

  onMount(() => {
    getPersistentKey(Consts.EmbedderExecutablePath).then((path) => {
      embedderExecutablePath = path || "./phenixcode-core";
      if (!path) {
        setPersistentKey(Consts.EmbedderExecutablePath, embedderExecutablePath);
      }
      console.log("embedderExecutablePath", $state.snapshot(embedderExecutablePath));
    });
  });

  $effect(() => {
    isValidExecutablePath(embedderExecutablePath).then((res: any) => {
      if (res.status !== "success") {
        valid = false;
        invalidMessage = res.message || "Invalid executable path.";
      } else {
        valid = true;
        invalidMessage = "";
      }
      console.log("valid", $state.snapshot(valid));
    });
  });

  function onCorePathChange(e: Event) {
    const ev = e as InputEvent;
    if (ev && ev.target) {
      setPersistentKey(Consts.EmbedderExecutablePath, (ev.target as HTMLInputElement).value);
      embedderExecutablePath = (ev.target as HTMLInputElement).value;
    }
  }

  async function isValidExecutablePath(path: string | undefined | null) {
    if (!path) return false;
    return await helper_checkPathExists(path);
  }
</script>

<div>
  <div class="text-right text-xs">Build date: {__BUILD_DATE__}</div>
  <label class="label">
    <span class="label-text">PhenixCode Executable path</span>
    <div class="flex items-center space-x-1">
      <input
        type="text"
        id="core-executable-path"
        class="input max-w-xl {!valid ? 'outline-2 outline-red-500' : ''}"
        oninput={onCorePathChange}
        value={embedderExecutablePath}
      />
    </div>
    {#if !valid}
      <div class="text-xs italic">{invalidMessage}</div>
    {/if}
  </label>
</div>

<style>
  .label {
    text-align: left;
  }
</style>
