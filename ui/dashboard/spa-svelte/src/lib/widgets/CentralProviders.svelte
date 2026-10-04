<script lang="ts">
  import { getContext, onMount } from "svelte";
  import UpDownButton from "./misc/UpDownButton.svelte";
  import { slide } from "svelte/transition";
  import { embeddingProviders, generationProviders, projectStore } from "../store.svelte";
  import {
    helper_readjustProject,
    helper_saveProjectSettings,
    helper_saveProvidersSettings,
    type Renames,
  } from "../utils";
  import type { ProjectItem, ProvidersSettings } from "../../app";

  let currentTab = $state(0); // 0=>embedding, 1=>generation

  const fetchProviders: () => void = getContext("FetchProviders");

  onMount(() => {
    fetchProviders();
  });

  const stripUi = <T extends { _hidden?: boolean }>(l: T[]) => l.map(({ _hidden, ...rest }) => rest);

  const jsonStr = $derived(
    JSON.stringify(
      { embedding_providers: stripUi(embeddingProviders), generation_providers: stripUi(generationProviders) },
      null,
      2,
    ),
  );

  function moveApiUp(index: number) {
    let activeProviders = currentTab == 0 ? embeddingProviders : generationProviders;
    if (!activeProviders) return;
    if (index > 0) {
      const temp = activeProviders[index - 1];
      activeProviders[index - 1] = activeProviders[index];
      activeProviders[index] = temp;
      onChange();
    }
  }
  function moveApiDown(index: number) {
    let activeProviders = currentTab == 0 ? embeddingProviders : generationProviders;
    if (!activeProviders) return;
    if (index < (activeProviders.length || 0) - 1) {
      const temp = activeProviders[index + 1];
      activeProviders[index + 1] = activeProviders[index];
      activeProviders[index] = temp;
      onChange();
    }
  }
  function removeApi(index: number) {
    let activeProviders = currentTab == 0 ? embeddingProviders : generationProviders;
    if (!activeProviders) return;
    if (activeProviders.length > 1) {
      activeProviders.splice(index, 1);
      onChange();
    }
  }
  function addApiGeneration() {
    const newId = `api_${Date.now()}`;
    generationProviders.push({
      api_key: "",
      api_url: "",
      id: newId,
      model: "",
      name: "New API",
      context_length: 0,
      max_tokens_name: "max_tokens",
      pricing_tpm: {
        input: 0,
        output: 0,
        cached_input: 0,
      },
    });
    onChange();
  }
  function addApiEmbedding() {
    if (!embeddingProviders) {
      return;
    }
    const newId = `api_${Date.now()}`;
    embeddingProviders.push({
      api_key: "",
      api_url: "",
      id: newId,
      model: "",
      name: "New API",
      document_format: "{}",
      query_format: "Represent this sentence for searching relevant passages: {}",
    });
    onChange();
  }

  // serialize: async onChange runs must not interleave
  let queue: Promise<void> = Promise.resolve();
  function commit(renames?: Renames) {
    queue = queue.then(() => persist(renames)).catch((e) => console.error("CentralProviders.commit", e));
  }

  // DOM handler for all plain fields: ignores the Event argument
  function onChange() {
    commit();
  }
  async function persist(renames?: Renames) {
    const providers = $state.snapshot({
      embedding_providers: stripUi(embeddingProviders),
      generation_providers: stripUi(generationProviders),
    }) as ProvidersSettings;

    await helper_saveProvidersSettings(providers); // throws => projects untouched

    const reindex: string[] = [];
    const droppedByProject: Record<string, string[]> = {};
    const saves: Promise<unknown>[] = [];

    for (const p of projectStore.list) {
      const r = helper_readjustProject(providers, p, renames); // in-place via $state proxy
      if (!r.changed) continue;
      const title = p.jsonData.source.project_title;
      if (r.needsReindex) reindex.push(title);
      if (r.dropped.length) droppedByProject[title] = r.dropped;
      saves.push(
        helper_saveProjectSettings($state.snapshot(p) as ProjectItem).catch((er) =>
          console.error("save failed:", title, er),
        ),
      );
    }
    await Promise.all(saves);

    if (reindex.length || Object.keys(droppedByProject).length) {
      notifyProjectsAdjusted({ reindex, dropped: droppedByProject });
    }
  }

  // --- id editing: treat as rename, reject empty/duplicate ---
  let idBefore = "";
  function onIdChange(kind: "embedding" | "generation", api: { id: string }) {
    const list: { id: string }[] = kind === "embedding" ? embeddingProviders : generationProviders;
    const next = api.id.trim();
    api.id = next;
    if (!next || list.filter((p) => p.id === next).length > 1) {
      api.id = idBefore;
      return;
    }
    if (next === idBefore) return;
    commit({ [kind]: new Map([[idBefore, next]]) }); // was onChange(...)
  }

  function onCollapseAll() {
    (currentTab == 0 ? embeddingProviders : generationProviders).forEach((a) => (a._hidden = true));
  }
  function onExpandAll() {
    (currentTab == 0 ? embeddingProviders : generationProviders).forEach((a) => (a._hidden = false));
  }

  let copied = $state(false);
  async function onCopy() {
    await navigator.clipboard.writeText(jsonStr);
    copied = true;
    setTimeout(() => (copied = false), 1500);
  }

  function notifyProjectsAdjusted(arg: { reindex: string[]; dropped: Record<string, string[]> }) {
    // TODO:
  }
</script>

<div style="height: 100%" class="flex flex-col gap-4 items-center">
  <div>
    <button
      type="button"
      class="btn {currentTab == 0 ? 'preset-filled-tertiary-500' : ''}"
      onclick={() => (currentTab = 0)}
    >
      Embedding
    </button>
    <button
      type="button"
      class="btn {currentTab == 1 ? 'preset-filled-tertiary-500' : ''}"
      onclick={() => (currentTab = 1)}
    >
      Generation
    </button>
    <button
      type="button"
      class="btn {currentTab == 2 ? 'preset-filled-tertiary-500' : ''}"
      onclick={() => (currentTab = 2)}
    >
      JSON
    </button>
  </div>
  <div class="h-full p-4 w-full lg:max-w-6xl overflow-auto">
    {#if currentTab == 1}
      <div class="flex justify-between items-center mb-4">
        <h2 class="text-xl font-bold">
          Generation APIs ({generationProviders.length})
        </h2>
        <button type="button" class="btn px-3 py-1 preset-filled-primary-500 rounded-md" onclick={addApiGeneration}>
          Add API
        </button>
      </div>
      <div>
        <button type="button" class="btn btn-sm" onclick={onCollapseAll}>collapse all</button>
        |
        <button type="button" class="btn btn-sm" onclick={onExpandAll}>expand all</button>
      </div>
      {#each generationProviders as api, i}
        <div class="flex flex-col mb-1">
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
                  <input
                    type="text"
                    class="input"
                    bind:value={api.id}
                    onfocus={() => (idBefore = api.id)}
                    onchange={() => onIdChange("generation", api)}
                  />
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
                  <input type="text" class="input" bind:value={api.model} placeholder="gpt-4o" onchange={onChange} />
                </label>
                <label class="label">
                  <span class="label-text">Context Length</span>
                  <input type="number" class="input" bind:value={api.context_length} onchange={onChange} />
                </label>
              </div>

              <div class="flex flex-col md:flex-row gap-4">
                <label class="label">
                  <span class="label-text">Temperature</span>
                  <input
                    type="number"
                    step="0.01"
                    class="input"
                    bind:value={api.temperature}
                    placeholder="temperature"
                    onchange={onChange}
                  />
                </label>
                <label class="label">
                  <span class="label-text">Max Tokens</span>
                  <input
                    type="number"
                    class="input"
                    bind:value={api.max_tokens}
                    placeholder="max_tokens"
                    onchange={onChange}
                  />
                </label>
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
              </div>

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
                    disabled={i === generationProviders.length - 1}
                  >
                    ↓ Down
                  </button>
                </div>
                <button
                  type="button"
                  class="btn btn-sm preset-filled-error-500"
                  onclick={() => removeApi(i)}
                  disabled={generationProviders.length === 1}
                >
                  Remove API
                </button>
              </div>
            </div>
          {/if}
        </div>
      {/each}
    {:else if currentTab == 0}
      <div class="flex justify-between items-center mb-4">
        <h2 class="text-xl font-bold">
          Embedding APIs ({embeddingProviders.length})
        </h2>
        <button type="button" class="btn px-3 py-1 preset-filled-primary-500 rounded-md" onclick={addApiEmbedding}>
          Add API
        </button>
      </div>
      <div>
        <button type="button" class="btn btn-sm" onclick={onCollapseAll}>collapse all</button>
        |
        <button type="button" class="btn btn-sm" onclick={onExpandAll}>expand all</button>
      </div>
      {#each embeddingProviders as api, i}
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
                  <input
                    type="text"
                    class="input"
                    bind:value={api.id}
                    onfocus={() => (idBefore = api.id)}
                    onchange={() => onIdChange("generation", api)}
                  />
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
                    disabled={i === embeddingProviders.length - 1}
                  >
                    ↓ Down
                  </button>
                </div>
                <button
                  type="button"
                  class="btn btn-sm preset-filled-error-500"
                  onclick={() => removeApi(i)}
                  disabled={embeddingProviders.length === 1}
                >
                  Remove API
                </button>
              </div>
            </div>
          {/if}
        </div>
      {/each}
    {:else if currentTab == 2}
      <div class="flex justify-between items-center mb-4">
        <h2 class="text-xl font-bold">JSON Configuration</h2>
      </div>
      <div class="rounded-md shadow p-4 flex flex-col gap-4">
        <div class="relative">
          <pre class="pre text-left min-h-40 overflow-x-auto">{jsonStr}</pre>
          <button
            class="absolute top-2 right-2 px-2 py-1 text-xs rounded bg-gray-700 text-white hover:bg-gray-600"
            onclick={onCopy}
          >
            {copied ? "Copied!" : "Copy"}
          </button>
        </div>
      </div>
    {:else}
      <div>Invalid tab selection</div>
    {/if}
  </div>
</div>
