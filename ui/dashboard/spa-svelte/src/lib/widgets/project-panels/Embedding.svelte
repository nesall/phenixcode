<script lang="ts">
  import { onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import { slide } from "svelte/transition";
  import UpDownButton from "../misc/UpDownButton.svelte";
  import { helper_saveProjectSettings } from "../../utils";
    import { projectStore } from "../../store.svelte";

  const jsonData = $derived(projectStore.selected?.jsonData);
  const projectTitle = $derived(
    projectStore.selected?.jsonData.source.project_title,
  );

  interface Props {
    onChanged: any;
  }
  let { onChanged }: Props = $props();

  onMount(() => {});

  function onCurApiChange(event: Event) {
    if (!jsonData) {
      return;
    }
    const selectElem = event.target as HTMLSelectElement;
    jsonData.embedding.current_api = selectElem.value;
    // selectedJsonSettings.set(jsonData);
    onChange();
  }

  function onChange() {
    if (projectStore.selected) {
      projectStore.selected = projectStore.selected;
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
          <div class="mb-4 flex items-center justify-between">
            <h2 class="text-xl font-bold flex items-center gap-2">
              <icons.Split size={24} />
              Embedding API Settings
            </h2>
            <code class="px-2 rounded text-lg">{projectTitle}</code>
          </div>

          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <label class="label">
              <span class="label-text">Batch Size</span>
              <input
                type="number"
                id="batch-size"
                class="input"
                bind:value={projectStore.selected.jsonData.embedding.batch_size}
                min="1"
                onchange={onChange}
              />
            </label>

            <label class="label">
              <span class="label-text">Timeout (ms)</span>
              <input
                type="number"
                id="timeout-ms"
                class="input"
                bind:value={projectStore.selected.jsonData.embedding.timeout_ms}
                min="1000"
                onchange={onChange}
              />
            </label>
          </div>

          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <label class="label">
              <span class="label-text">Retry Attempts</span>
              <input
                type="number"
                id="retry-attempts"
                class="input"
                bind:value={projectStore.selected.jsonData.embedding.retry_attempts}
                min="0"
                onchange={onChange}
              />
            </label>

            <label class="label">
              <span class="label-text">Top K</span>
              <input
                type="number"
                id="top-k"
                class="input"
                bind:value={projectStore.selected.jsonData.embedding.top_k}
                min="1"
                onchange={onChange}
              />
            </label>
          </div>

          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <label class="label">
              <span class="label-text">Prepend Label Format</span>
              <input
                type="text"
                id="prepend-label-format"
                class="input"
                bind:value={
                  projectStore.selected.jsonData.embedding.prepend_label_format
                }
                placeholder="[Source: &#123;&#125;]\n"
                onchange={onChange}
              />
              <p class="text-sm text-surface-500 mt-1">
                Use &#123;&#125; as placeholder for the source name
              </p>
            </label>
          </div>
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <label class="label">
              <span class="label-text">Current API</span>
              <select
                id="current-api-emb"
                class="select"
                value={projectStore.selected.jsonData.embedding.current_api}
                onchange={onCurApiChange}
              >
                {#each projectStore.selected.jsonData.embedding.enabled_providers as api}
                  <option value={api}>{api}</option>
                {/each}
              </select>
            </label>
          </div>
        </div>

        <div class="rounded-md shadow p-4 flex flex-col gap-4">

          {#each projectStore.selected.jsonData.embedding.enabled_providers as api, i}
            <div class="flex flex-col">
              <!-- checkbox for each API -->
              <label class="label flex items-center gap-2">
                <input
                  type="checkbox"
                  class="checkbox"
                  onchange={onChange}
                />
                <span class="font-semibold">{api}</span>
              </label>
            </div>
          {/each}
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
