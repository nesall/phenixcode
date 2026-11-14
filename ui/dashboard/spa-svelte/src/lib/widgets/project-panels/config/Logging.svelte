<script lang="ts">
  import { onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import { projectStore } from "../../../store.svelte";
  import { helper_saveProjectSettings } from "../../../utils";

  interface Props {
    onChanged: any;
  }
  let { onChanged }: Props = $props();

  const projectTitle = $derived(projectStore.selected?.jsonData.source.project_title);

  onMount(() => {});

  function onChange() {
    if (projectStore.selected) {
      helper_saveProjectSettings(projectStore.selected);
      onChanged(projectStore.selected);
    }
  }
</script>

{#if projectStore.selected}
  <div class="h-full p-4 overflow-auto">
    <form class="w-full">
      <fieldset class="space-y-4">
        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="flex justify-between items-center mb-4">
            <h2 class="text-xl font-bold flex items-center gap-2">
              <icons.Logs size={24} />
              Application Logging Configuration
            </h2>
            <code class="px-2 rounded text-lg">{projectTitle}</code>
          </div>

          <!-- Logging Toggles -->
          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2">Destinations</h3>
          <div class="grid grid-cols-1 gap-4 md:grid-cols-3 md:gap-2">
            <div class="flex-col space-x-3 p-3 bg-surface-50-900 rounded-lg text-left">
              <label for="log-console-toggle" class="flex items-center cursor-pointer gap-2">
                <input
                  type="checkbox"
                  class="checkbox checkbox-lg preset-filled-primary-500"
                  bind:checked={projectStore.selected.jsonData.logging.log_to_console}
                  id="log-console-toggle"
                  onchange={onChange}
                />
                <span class="font-medium text-surface-900-50">Log to Console</span>
              </label>
              <span class="text-xs text-surface-500-400">Display messages in the terminal/browser console.</span>
            </div>

            <div class="flex-col space-x-3 p-3 bg-surface-50-900 rounded-lg text-left">
              <label for="log-file-toggle" class="flex items-center cursor-pointer gap-2">
                <input
                  type="checkbox"
                  class="checkbox checkbox-lg preset-filled-primary-500"
                  bind:checked={projectStore.selected.jsonData.logging.log_to_file}
                  id="log-file-toggle"
                  onchange={onChange}
                />
                <span class="font-medium text-surface-900-50"> Log to File</span>
              </label>
              <span class="text-xs text-surface-500-400"> Persist log messages to a local file. </span>
            </div>

            <div class="flex-col space-x-3 p-3 bg-surface-50-900 rounded-lg text-left">
              <div class="flex items-center gap-2">
                <span class="font-medium text-surface-900-50">Min Log Level</span>
                <select
                  class="select select-lg preset-filled-primary-500 max-w-32"
                  bind:value={projectStore.selected.jsonData.logging.min_log_level}
                  id="min-log-level-select"
                  onchange={onChange}
                >
                  <option value="debug">Debug</option>
                  <option value="info">Info</option>
                  <option value="warn">Warn</option>
                  <option value="error">Error</option>
                </select>
              </div>
              <span class="text-xs text-surface-500-400">Filter log messages by minimum severity.</span>
            </div>
          </div>

          <!-- File Paths -->
          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2 pt-2">File Paths</h3>
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <label class="label">
              <span class="label-text">General Log File Path</span>
              <input
                type="text"
                class="input"
                bind:value={projectStore.selected.jsonData.logging.logging_file}
                placeholder="application.log"
                disabled={!projectStore.selected?.jsonData.logging.log_to_file}
                onchange={onChange}
              />
              <p class="text-sm text-surface-500 mt-1">File for general information and warnings.</p>
            </label>

            <label class="label">
              <span class="label-text">Diagnostics Log File Path</span>
              <input
                type="text"
                class="input"
                bind:value={projectStore.selected.jsonData.logging.diagnostics_file}
                placeholder="diagnostics.log"
                disabled={!projectStore.selected?.jsonData.logging.log_to_file}
                onchange={onChange}
              />
              <p class="text-sm text-surface-500 mt-1">File for detailed debug and error messages.</p>
            </label>
          </div>
        </div>
      </fieldset>
    </form>
  </div>
{/if}

<style>
  .label {
    text-align: left;
  }
</style>
