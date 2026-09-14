<script lang="ts">
  import { onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import { selectedProject } from "../../store";
  import { helper_saveProjectSettings } from "../../utils";
  import { generationProviders } from "../../store.svelte";

  interface Props {
    onChanged: any;
  }
  let { onChanged }: Props = $props();

  const jsonData = $derived($selectedProject?.jsonData);
  const projectTitle = $derived($selectedProject?.jsonData.source.project_title);

  let checkedProviders: string[] = $state([...($selectedProject?.jsonData.generation.enabled_providers || [])]);

  onMount(() => {});

  // function addApi() {
  //   if (!jsonData) {
  //     return;
  //   }
  //   const newId = `api_${Date.now()}`;
  //   jsonData.generation.apis.push({
  //     api_key: "",
  //     api_url: "",
  //     id: newId,
  //     model: "",
  //     name: "New API",
  //     max_tokens_name: "max_tokens",
  //     context_length: 4096,
  //     pricing_tpm: { cached_input: 0, input: 0, output: 0 },
  //   });
  //   jsonData.generation.current_api = newId;
  //   // selectedJsonSettings.set(jsonData);
  //   onChange();
  // }

  // function removeApi(index: number) {
  //   if (!jsonData) {
  //     return;
  //   }
  //   if (1 < jsonData.generation.apis.length) {
  //     jsonData.generation.apis.splice(index, 1);
  //     // If we removed the current API, switch to the first one
  //     if (
  //       jsonData.generation.current_api === jsonData.generation.apis[index]?.id
  //     ) {
  //       jsonData.generation.current_api = jsonData.generation.apis[0]?.id || "";
  //     }
  //     onChange();
  //   }
  // }

  // function moveApiUp(index: number) {
  //   if (!jsonData) {
  //     return;
  //   }
  //   if (0 < index) {
  //     const temp = jsonData.generation.apis[index];
  //     jsonData.generation.apis[index] = jsonData.generation.apis[index - 1];
  //     jsonData.generation.apis[index - 1] = temp;
  //     onChange();
  //   }
  // }

  // function moveApiDown(index: number) {
  //   if (!jsonData) {
  //     return;
  //   }
  //   if (index < jsonData.generation.apis.length - 1) {
  //     const temp = jsonData.generation.apis[index];
  //     jsonData.generation.apis[index] = jsonData.generation.apis[index + 1];
  //     jsonData.generation.apis[index + 1] = temp;
  //     onChange();
  //   }
  // }

  function onCurApiChange(event: Event) {
    if (!jsonData) {
      return;
    }
    const selectElem = event.target as HTMLSelectElement;
    jsonData.generation.current_api = selectElem.value;
    // selectedJsonSettings.set(jsonData);
    onChange();
  }

  function onChange() {
    if ($selectedProject) {
      $selectedProject = $selectedProject;
      helper_saveProjectSettings($selectedProject);
      onChanged($selectedProject);
    }
  }

  function onToggle(api: string) {
    if (!jsonData) {
      return;
    }
    const index = checkedProviders.indexOf(api);
    if (index === -1) {
      checkedProviders.push(api);
    } else {
      checkedProviders.splice(index, 1);
    }
    jsonData.generation.enabled_providers = [...checkedProviders];
    onChange();
  }

  // function onExpandAll() {
  //   if (!$selectedProject) {
  //     return;
  //   }
  //   for (const api of $selectedProject?.jsonData.generation.apis) {
  //     api._hidden = false;
  //   }
  //   $selectedProject = $selectedProject;
  // }

  // function onCollapseAll() {
  //   if (!$selectedProject) {
  //     return;
  //   }
  //   for (const api of $selectedProject?.jsonData.generation.apis) {
  //     api._hidden = true;
  //   }
  //   $selectedProject = $selectedProject;
  // }
</script>

{#if $selectedProject}
  <div class="h-full p-4 overflow-auto">
    <form class="w-full">
      <fieldset class="space-y-4">
        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="mb-4 flex items-center justify-between">
            <h2 class="text-xl font-bold flex items-center gap-2">
              <icons.Merge size={24} />
              Generation API Settings
            </h2>
            <code class="px-2 rounded text-lg">{projectTitle}</code>
          </div>

          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2">General</h3>
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <label class="label">
              <span class="label-text">Timeout (ms)</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.generation.timeout_ms}
                min="1000"
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Max Context Tokens</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.generation.max_context_tokens}
                min="1"
                onchange={onChange}
              />
            </label>
          </div>

          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2 pt-2">Retrieval limits</h3>
          <div class="grid grid-cols-1 md:grid-cols-3 gap-4">
            <label class="label">
              <span class="label-text">Max Chunks</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.generation.max_chunks}
                min="1"
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Max Full Sources</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.generation.max_full_sources}
                min="0"
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Max Related / Source</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.generation.max_related_per_source}
                min="0"
                onchange={onChange}
              />
            </label>
          </div>

          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2 pt-2">Defaults</h3>
          <div class="grid grid-cols-1 md:grid-cols-3 gap-4">
            <label class="label">
              <span class="label-text">Default Temp</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.generation.default_temperature}
                step="0.1"
                min="0"
                max="2"
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Default Max Tokens</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.generation.default_max_tokens}
                min="1"
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Max Tokens Param Name</span>
              <input
                type="text"
                class="input"
                bind:value={$selectedProject.jsonData.generation.default_max_tokens_name}
                onchange={onChange}
              />
            </label>
          </div>

          <div class="grid grid-cols-1 md:grid-cols-1 gap-4">
            <label class="label">
              <span class="label-text">Prepend Label Format</span>
              <input
                type="text"
                class="input"
                bind:value={$selectedProject.jsonData.generation.prepend_label_format}
                placeholder="[Source: &#123;&#125;]\n"
                onchange={onChange}
              />
              <p class="text-sm text-surface-500 mt-1">Use &#123;&#125; as placeholder for source name</p>
            </label>
          </div>

          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2 pt-2 flex items-center gap-2">
            Excerpt Logic
            <input
              type="checkbox"
              class="checkbox"
              bind:checked={$selectedProject.jsonData.generation.excerpt.enabled}
              onchange={onChange}
            />
          </h3>
          <div class="grid grid-cols-1 md:grid-cols-3 gap-4">
            <label class="label">
              <span class="label-text">Min Chunks</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.generation.excerpt.min_chunks}
                disabled={!$selectedProject.jsonData.generation.excerpt.enabled}
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Max Chunks</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.generation.excerpt.max_chunks}
                disabled={!$selectedProject.jsonData.generation.excerpt.enabled}
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Threshold Ratio</span>
              <input
                type="number"
                class="input"
                step="0.05"
                bind:value={$selectedProject.jsonData.generation.excerpt.threshold_ratio}
                disabled={!$selectedProject.jsonData.generation.excerpt.enabled}
                onchange={onChange}
              />
            </label>
          </div>

          <div class="grid grid-cols-1 md:grid-cols-2 gap-4 pt-4 border-t border-surface-500">
            <label class="label">
              <span class="label-text">Current API</span>
              <select
                id="current-api-gen"
                class="select"
                value={$selectedProject.jsonData.generation.current_api}
                onchange={onCurApiChange}
              >
                {#each $selectedProject.jsonData.generation.enabled_providers as api}
                  <option value={api}>{api}</option>
                {/each}
              </select>
            </label>
          </div>
        </div>

        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="flex justify-between items-center">
            <h2 class="text-xl font-bold">
              Generation Providers ({generationProviders.length})
            </h2>
          </div>
          <div class="text-left">Check providers to make them enabled for this project</div>
          {#each generationProviders as api, i}
            <div class="flex flex-col">
              <label class="label flex items-center gap-2">
                <input
                  type="checkbox"
                  class="checkbox"
                  checked={checkedProviders.includes(api.id)}
                  onchange={() => onToggle(api.id)}
                />
                <span class="font">{api.name} ({api.id})</span>
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
