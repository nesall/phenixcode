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
    jsonData.embedding.apis.push({
      api_key: "",
      api_url: "",
      id: newId,
      model: "",
      name: "New API",
      document_format: "{}",
      query_format: "Represent this sentence for searching relevant passages: {}",
    });
    jsonData.embedding.current_api = newId;
    // selectedJsonSettings.set(jsonData);
    onChange();
  }

  function removeApi(index: number) {
    if (!jsonData) {
      return;
    }

    if (1 < jsonData.embedding.apis.length) {
      jsonData.embedding.apis.splice(index, 1);
      // If we removed the current API, switch to the first one
      if (jsonData.embedding.current_api === jsonData.embedding.apis[index]?.id) {
        jsonData.embedding.current_api = jsonData.embedding.apis[0]?.id || "";
      }
      // selectedJsonSettings.set(jsonData);
      onChange();
    }
  }

  function moveApiUp(index: number) {
    if (!jsonData) {
      return;
    }

    if (0 < index) {
      const temp = jsonData.embedding.apis[index];
      jsonData.embedding.apis[index] = jsonData.embedding.apis[index - 1];
      jsonData.embedding.apis[index - 1] = temp;
      // selectedJsonSettings.set(jsonData);
      onChange();
    }
  }

  function moveApiDown(index: number) {
    if (!jsonData) {
      return;
    }

    if (index < jsonData.embedding.apis.length - 1) {
      const temp = jsonData.embedding.apis[index];
      jsonData.embedding.apis[index] = jsonData.embedding.apis[index + 1];
      jsonData.embedding.apis[index + 1] = temp;
      // selectedJsonSettings.set(jsonData);
      onChange();
    }
  }

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
    if ($selectedProject) {
      $selectedProject = $selectedProject;
      helper_saveProjectSettings($selectedProject);
    }
  }

  function onExpandAll() {
    if (!$selectedProject) {
      return;
    }
    for (const api of $selectedProject?.jsonData.embedding.apis) {
      api._hidden = false;
    }
    $selectedProject = $selectedProject;
  }

  function onCollapseAll() {
    if (!$selectedProject) {
      return;
    }
    for (const api of $selectedProject?.jsonData.embedding.apis) {
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
              <icons.MoveUpLeft size={24} />
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
                bind:value={$selectedProject.jsonData.embedding.batch_size}
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
                bind:value={$selectedProject.jsonData.embedding.timeout_ms}
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
                bind:value={$selectedProject.jsonData.embedding.retry_attempts}
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
                bind:value={$selectedProject.jsonData.embedding.top_k}
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
                bind:value={$selectedProject.jsonData.embedding.prepend_label_format}
                placeholder="[Source: &#123;&#125;]\n"
                onchange={onChange}
              />
              <p class="text-sm text-surface-500 mt-1">Use &#123;&#125; as placeholder for the source name</p>
            </label>
          </div>
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <label class="label">
              <span class="label-text">Current API</span>
              <select
                id="current-api-emb"
                class="select"
                value={$selectedProject.jsonData.embedding.current_api}
                onchange={onCurApiChange}
              >
                {#each $selectedProject.jsonData.embedding.apis as api}
                  <option value={api.id}>{api.name} ({api.id})</option>
                {/each}
              </select>
            </label>
          </div>
        </div>

        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="flex justify-between items-center mb-4">
            <h2 class="text-xl font-bold">Embedding APIs ({$selectedProject.jsonData.embedding.apis.length})</h2>
            <button type="button" class="btn px-3 py-1 preset-filled-primary-500 rounded-md" onclick={addApi}>
              Add API
            </button>
          </div>
          <div>
            <button type="button" class="btn btn-sm" onclick={onCollapseAll}>collapse all</button> |
            <button type="button" class="btn btn-sm" onclick={onExpandAll}>expand all</button>
          </div>

          {#each $selectedProject.jsonData.embedding.apis as api, i}
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
                      <input type="text" id="api-name-{i}" class="input" bind:value={api.name} onchange={onChange} />
                    </label>

                    <label class="label">
                      <span class="label-text">API ID</span>
                      <input type="text" id="api-id-{i}" class="input" bind:value={api.id} onchange={onChange} />
                    </label>
                  </div>

                  <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
                    <label class="label">
                      <span class="label-text">API URL</span>
                      <input
                        type="text"
                        id="api-url-{i}"
                        class="input"
                        bind:value={api.api_url}
                        placeholder="http://127.0.0.1:8583/embedding"
                        onchange={onChange}
                      />
                    </label>

                    <label class="label">
                      <span class="label-text">API Key</span>
                      <input
                        type="text"
                        id="api-key-{i}"
                        class="input"
                        bind:value={api.api_key}
                        placeholder="API key or {'${ENV_VAR_NAME}'}"
                        onchange={onChange}
                      />
                    </label>
                  </div>

                  <label class="label">
                    <span class="label-text">Model</span>
                    <input
                      type="text"
                      id="model-{i}"
                      class="input"
                      bind:value={api.model}
                      placeholder="bge-base-v1.5"
                      onchange={onChange}
                    />
                  </label>

                  <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
                    <label class="label">
                      <span class="label-text">Document Format</span>
                      <input
                        type="text"
                        id="document-format-{i}"
                        class="input"
                        bind:value={api.document_format}
                        placeholder="&#123;&#125;"
                        onchange={onChange}
                      />
                      <p class="text-sm text-surface-500 mt-1">Use &#123;&#125; as placeholder for the document text</p>
                    </label>

                    <label class="label">
                      <span class="label-text">Query Format</span>
                      <input
                        type="text"
                        id="query-format-{i}"
                        class="input"
                        bind:value={api.query_format}
                        placeholder="Represent this sentence for searching relevant passages: &#123;&#125;"
                        onchange={onChange}
                      />
                      <p class="text-sm text-surface-500 mt-1">Use &#123;&#125; as placeholder for the query text</p>
                    </label>
                  </div>

                  <div class="flex justify-between">
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
                        disabled={i === $selectedProject.jsonData.embedding.apis.length - 1}
                      >
                        ↓ Down
                      </button>
                    </div>
                    <button
                      type="button"
                      class="btn btn-sm preset-filled-error-500"
                      onclick={() => removeApi(i)}
                      disabled={$selectedProject.jsonData.embedding.apis.length === 1}
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
