<script lang="ts">
  import { onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import { slide } from "svelte/transition";
  import { selectedProject } from "../../store";
  import UpDownButton from "../misc/UpDownButton.svelte";
  import { helper_saveProjectSettings } from "../../utils";

  const jsonData = $derived($selectedProject?.jsonData);
  const projectTitle = $derived($selectedProject?.jsonData.source.project_title);

  onMount(() => {});

  function addApi() {
    if (!jsonData) {
      return;
    }
    const newId = `api_${Date.now()}`;
    jsonData.generation.apis.push({
      api_key: "",
      api_url: "",
      id: newId,
      model: "",
      name: "New API",
      max_tokens_name: "max_tokens",
      context_length: 4096,
      pricing_tpm: { cached_input: 0, input: 0, output: 0 },
    });
    jsonData.generation.current_api = newId;
    // selectedJsonSettings.set(jsonData);
    onChange();
  }

  function removeApi(index: number) {
    if (!jsonData) {
      return;
    }
    if (1 < jsonData.generation.apis.length) {
      jsonData.generation.apis.splice(index, 1);
      // If we removed the current API, switch to the first one
      if (jsonData.generation.current_api === jsonData.generation.apis[index]?.id) {
        jsonData.generation.current_api = jsonData.generation.apis[0]?.id || "";
      }
      onChange();
    }
  }

  function moveApiUp(index: number) {
    if (!jsonData) {
      return;
    }
    if (0 < index) {
      const temp = jsonData.generation.apis[index];
      jsonData.generation.apis[index] = jsonData.generation.apis[index - 1];
      jsonData.generation.apis[index - 1] = temp;
      onChange();
    }
  }

  function moveApiDown(index: number) {
    if (!jsonData) {
      return;
    }
    if (index < jsonData.generation.apis.length - 1) {
      const temp = jsonData.generation.apis[index];
      jsonData.generation.apis[index] = jsonData.generation.apis[index + 1];
      jsonData.generation.apis[index + 1] = temp;
      onChange();
    }
  }

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
    }
  }

  function onExpandAll() {
    if (!$selectedProject) {
      return;
    }
    for (const api of $selectedProject?.jsonData.generation.apis) {
      api._hidden = false;
    }
    $selectedProject = $selectedProject;
  }

  function onCollapseAll() {
    if (!$selectedProject) {
      return;
    }
    for (const api of $selectedProject?.jsonData.generation.apis) {
      api._hidden = true;
    }
    $selectedProject = $selectedProject;
  }
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
                {#each $selectedProject.jsonData.generation.apis as api}
                  <option value={api.id}>{api.name} ({api.id})</option>
                {/each}
              </select>
            </label>
          </div>
        </div>

        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="flex justify-between items-center mb-4">
            <h2 class="text-xl font-bold">Generation APIs ({$selectedProject.jsonData.generation.apis.length})</h2>
            <button type="button" class="btn px-3 py-1 preset-filled-primary-500 rounded-md" onclick={addApi}>
              Add API
            </button>
          </div>
          <div>
            <button type="button" class="btn btn-sm" onclick={onCollapseAll}>collapse all</button> |
            <button type="button" class="btn btn-sm" onclick={onExpandAll}>expand all</button>
          </div>
          {#each $selectedProject.jsonData.generation.apis as api, i}
            <div class="flex flex-col">
              <UpDownButton
                hidden={api._hidden}
                text={`${api.name} - ${api.model}`}
                onChange={() => (api._hidden = !api._hidden)}
              />
              {#if !api._hidden}
                <div
                  class="border border-surface-200-800 rounded-md rounded-t-none p-4 mb-4 flex flex-col gap-4"
                  transition:slide
                >
                  <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
                    <label class="label">
                      <span class="label-text">API Name</span>
                      <input type="text" class="input" bind:value={api.name} onchange={onChange} />
                    </label>
                    <label class="label">
                      <span class="label-text">API ID</span>
                      <input type="text" class="input" bind:value={api.id} onchange={onChange} />
                    </label>
                  </div>

                  <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
                    <label class="label">
                      <span class="label-text">API URL</span>
                      <input
                        type="text"
                        class="input"
                        bind:value={api.api_url}
                        placeholder="https://api.openai.com/v1/chat/completions"
                        onchange={onChange}
                      />
                    </label>
                    <label class="label">
                      <span class="label-text">API Key</span>
                      <input
                        type="text"
                        class="input"
                        bind:value={api.api_key}
                        placeholder="API key or {'${ENV_VAR_NAME}'}"
                        onchange={onChange}
                      />
                    </label>
                  </div>

                  <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
                    <label class="label">
                      <span class="label-text">Model</span>
                      <input
                        type="text"
                        class="input"
                        bind:value={api.model}
                        placeholder="gpt-4o"
                        onchange={onChange}
                      />
                    </label>
                    <label class="label">
                      <span class="label-text">Context Length</span>
                      <input type="number" class="input" bind:value={api.context_length} onchange={onChange} />
                    </label>
                  </div>

                  <label class="label">
                    <span class="label-text">Max Tokens Param Name</span>
                    <input
                      type="text"
                      class="input"
                      bind:value={api.max_tokens_name}
                      placeholder="max_tokens"
                      onchange={onChange}
                    />
                  </label>

                  <div class="border border-surface-500 p-3 rounded-md">
                    <span class="label-text font-semibold mb-2 block">Pricing (TPM)</span>
                    <div class="grid grid-cols-1 md:grid-cols-3 gap-4">
                      <label class="label">
                        <span class="label-text text-xs">Input</span>
                        <input
                          type="number"
                          step="0.001"
                          class="input"
                          bind:value={api.pricing_tpm.input}
                          onchange={onChange}
                        />
                      </label>
                      <label class="label">
                        <span class="label-text text-xs">Output</span>
                        <input
                          type="number"
                          step="0.001"
                          class="input"
                          bind:value={api.pricing_tpm.output}
                          onchange={onChange}
                        />
                      </label>
                      <label class="label">
                        <span class="label-text text-xs">Cached Input</span>
                        <input
                          type="number"
                          step="0.001"
                          class="input"
                          bind:value={api.pricing_tpm.cached_input}
                          onchange={onChange}
                        />
                      </label>
                    </div>
                  </div>

                  <div class="flex justify-between mt-2">
                    <div class="space-x-2">
                      <button
                        type="button"
                        class="btn btn-sm preset-tonal-primary"
                        onclick={() => moveApiUp(i)}
                        disabled={i === 0}
                      >
                        ↑ Up
                      </button>
                      <button
                        type="button"
                        class="preset-tonal-primary btn btn-sm"
                        onclick={() => moveApiDown(i)}
                        disabled={i === $selectedProject.jsonData.generation.apis.length - 1}
                      >
                        ↓ Down
                      </button>
                    </div>
                    <button
                      type="button"
                      class="btn btn-sm preset-filled-error-500"
                      onclick={() => removeApi(i)}
                      disabled={$selectedProject.jsonData.generation.apis.length === 1}
                    >
                      Remove API
                    </button>
                  </div>
                </div>
              {/if}
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
