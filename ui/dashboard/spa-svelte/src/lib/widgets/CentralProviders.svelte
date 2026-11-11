<script lang="ts">
  import { getContext, onMount } from "svelte";
  import UpDownButton from "./misc/UpDownButton.svelte";
  import * as icons from "@lucide/svelte";
  import { slide } from "svelte/transition";
  import { embeddingProviders, generationProviders, projectStore } from "../store.svelte";
  import {
    helper_readjustProject,
    helper_saveProjectSettings,
    helper_saveProvidersSettings,
    helper_testProvider,
    toaster,
    type Renames,
  } from "../utils";
  import type { GenerationApi, ProjectItem, ProvidersSettings, ReasoningParameter } from "../../app";

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

  function toggleReasoningParameters(api: GenerationApi, enabled: boolean) {
    if (enabled) {
      api.reasoning = { ...api.reasoning, parameters: [] };
      addReasoningParameter(api);
      return;
    }
    if (api.reasoning) {
      delete api.reasoning.parameters;
      if ((api.reasoning.history_replay ?? 0) === 0) delete api.reasoning;
    }
    onChange();
  }

  function changeHistoryReplay(api: GenerationApi, value: number) {
    if (value === 0 && !Array.isArray(api.reasoning?.parameters)) {
      delete api.reasoning;
    } else {
      api.reasoning ??= {};
      api.reasoning.history_replay = value;
    }
    onChange();
  }

  function addReasoningParameter(api: GenerationApi) {
    api.reasoning?.parameters?.push({
      name: "",
      value_type: "scalar",
      values: [],
      default: "",
    });
    onChange();
  }

  function removeReasoningParameter(api: GenerationApi, index: number) {
    api.reasoning?.parameters?.splice(index, 1);
    onChange();
  }

  function addConstantField(parameter: ReasoningParameter) {
    const fields = (parameter.constant_fields ??= {});
    let name = "field";
    let suffix = 2;
    while (name in fields) name = `field_${suffix++}`;
    fields[name] = "";
    onChange();
  }

  function renameConstantField(parameter: ReasoningParameter, oldName: string, event: Event) {
    const newName = (event.currentTarget as HTMLInputElement).value.trim();
    const fields = parameter.constant_fields;
    if (!fields || !newName || (newName !== oldName && newName in fields)) {
      (event.currentTarget as HTMLInputElement).value = oldName;
      return;
    }
    if (newName !== oldName) {
      fields[newName] = fields[oldName];
      delete fields[oldName];
      onChange();
    }
  }

  function updateConstantFieldValue(parameter: ReasoningParameter, name: string, event: Event) {
    const input = event.currentTarget as HTMLInputElement;
    const value = input.value;
    try {
      const parsed: unknown = JSON.parse(value);
      if (parsed === null || typeof parsed === "string" || typeof parsed === "number" || typeof parsed === "boolean") {
        if (parameter.constant_fields) parameter.constant_fields[name] = parsed;
        onChange();
        return;
      }
    } catch {
      // Unquoted input is treated as a string value.
    }
    if (parameter.constant_fields) parameter.constant_fields[name] = value;
    onChange();
  }

  function removeConstantField(parameter: ReasoningParameter, name: string) {
    if (!parameter.constant_fields) return;
    delete parameter.constant_fields[name];
    if (Object.keys(parameter.constant_fields).length === 0) delete parameter.constant_fields;
    onChange();
  }

  function addReasoningValue(input: HTMLInputElement, parameter: ReasoningParameter) {
    const value = input.value.trim();
    if (!value) return;

    parameter.values.push(value);
    input.value = "";
    if (!parameter.default) parameter.default = value;
    onChange();
  }

  function removeReasoningValue(parameter: ReasoningParameter, index: number) {
    const dels = parameter.values.splice(index, 1);
    if (parameter.default === dels[0]) {
      parameter.default = parameter.values[0] ?? "";
    }
    onChange();
  }

  // serialize: async onChange runs must not interleave
  let queue: Promise<void> = Promise.resolve();
  function commit(renames?: Renames) {
    const providers = $state.snapshot({
      embedding_providers: stripUi(embeddingProviders),
      generation_providers: stripUi(generationProviders),
    }) as ProvidersSettings;
    queue = queue.then(() => persist(providers, renames)).catch((error) =>
      toaster.error({ title: "Provider update failed", description: String(error) }),
    );
  }

  // DOM handler for all plain fields: ignores the Event argument
  function onChange() {
    commit();
  }
  async function persist(providers: ProvidersSettings, renames?: Renames) {
    await helper_saveProvidersSettings(providers); // throws => projects untouched

    const reindex: string[] = [];
    const droppedByProject: Record<string, string[]> = {};
    const selections: Record<string, string[]> = {};
    const saves: Promise<unknown>[] = [];

    for (const p of projectStore.list) {
      try {
        const r = await helper_readjustProject(providers, p, renames);
        const title = p.jsonData.source.project_title;
        if (r.needsReindex) reindex.push(title);
        if (r.dropped.length) droppedByProject[title] = r.dropped;
        if (r.needsSelection.length) {
          selections[title] = r.needsSelection;
          continue;
        }
        if (!r.changed) continue;
        saves.push(
          helper_saveProjectSettings($state.snapshot(p) as ProjectItem).then((result) => {
            if (result.status !== "success") throw new Error(result.message);
          }).catch((error) => toaster.error({ title: `Failed to save ${title}`, description: String(error) })),
        );
      } catch (error) {
        toaster.error({ title: `Failed to adjust ${p.jsonData.source.project_title}`, description: String(error) });
      }
    }
    await Promise.all(saves);

    if (reindex.length || Object.keys(droppedByProject).length || Object.keys(selections).length) {
      notifyProjectsAdjusted({ reindex, dropped: droppedByProject, selections });
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

  function notifyProjectsAdjusted(arg: { reindex: string[]; dropped: Record<string, string[]>; selections: Record<string, string[]> }) {
    const details = [
      ...arg.reindex.map((title) => `${title} requires reindexing after choosing an embedding provider.`),
      ...Object.entries(arg.dropped).map(([title, ids]) => `${title} removed unavailable providers (${ids.join(", ")}).`),
      ...Object.entries(arg.selections).map(([title, fields]) => `${title} requires a selection for ${fields.join(", ")}.`),
    ];
    toaster.info({ title: "Project provider settings adjusted", description: details.join("\n") });
  }

  async function onTestGenerationApi(api: GenerationApi) {
    try {
      const result = await helper_testProvider(
        { url: api.api_url, apiKey: api.api_key, model: api.model, id: api.id },
        "generation",
      );
      alert(`API test successful:\n${JSON.stringify(result, null, 2)}`);
    } catch (err) {
      alert(`API test failed:\n${err}`);
    }
  }

  let curlCopied = $state(false);

  async function onCopyCurl(api: GenerationApi) {
    const reasoningFields = Object.fromEntries(
      (api.reasoning?.parameters ?? [])
        .filter((parameter) => parameter.name)
        .map((parameter) => [
          parameter.name,
          parameter.value_type === "object"
            ? {
                ...parameter.constant_fields,
                ...(parameter.value_field ? { [parameter.value_field]: parameter.default } : {}),
              }
            : parameter.default,
        ]),
    );
    const requestBody = JSON.stringify({
      model: api.model,
      messages: [{ role: "user", content: "Say hello!" }],
      ...reasoningFields,
    });
    const escapedBody = requestBody.replace(/'/g, "'\\''");
    const curlCommand = `curl -X POST ${api.api_url} -H "Authorization: Bearer ${api.api_key}" -H "Content-Type: application/json" -d '${escapedBody}'`;
    try {
      await navigator.clipboard.writeText(curlCommand);
      curlCopied = true;
      setTimeout(() => (curlCopied = false), 1200);
    } catch (err) {
      alert(`Failed to copy cURL command:\n${err}`);
    }
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

              <div class="border border-surface-500 p-3 rounded-md">
                <span class="label-text font-semibold mb-2 block">Reasoning</span>
                <div class="flex items-center gap-2 mb-4">
                  <label class="flex items-center gap-2">
                    <span class="label-text">Parameters</span>
                    <input
                      type="checkbox"
                      class="checkbox"
                      checked={Array.isArray(api.reasoning?.parameters)}
                      onchange={(event) => toggleReasoningParameters(api, event.currentTarget.checked)}
                    />
                  </label>
                  <label class="flex items-center gap-2 ml-auto">
                    <span class="label-text whitespace-nowrap">History replay</span>
                    <select
                      class="select"
                      value={api.reasoning?.history_replay ?? 0}
                      onchange={(event) => changeHistoryReplay(api, Number(event.currentTarget.value))}
                    >
                      <option value={0}>Off</option>
                      <option value={-1}>All</option>
                      <option value={5}>Last 5</option>
                      <option value={3}>Last 3</option>
                    </select>
                  </label>
                </div>
                {#if api.reasoning?.parameters}
                  {#each api.reasoning.parameters as parameter, parameterIndex}
                    <div class="border border-surface-300 rounded-md p-3 mb-3">
                      <div
                        class="grid grid-cols-1 {parameter.value_type === 'object'
                          ? 'md:grid-cols-2'
                          : 'md:grid-cols-3'} gap-3"
                      >
                        <label class="label">
                          <span class="label-text text-xs">Name</span>
                          <input
                            type="text"
                            class="input"
                            placeholder="reasoning_effort"
                            bind:value={parameter.name}
                            onchange={onChange}
                          />
                        </label>
                        <label class="label">
                          <span class="label-text text-xs">Value Type</span>
                          <select
                            class="select"
                            bind:value={parameter.value_type}
                            onchange={() => {
                              if (parameter.value_type === "scalar") {
                                delete parameter.value_field;
                                delete parameter.constant_fields;
                              }
                              onChange();
                            }}
                          >
                            <option value="scalar">Scalar</option>
                            <option value="object">Object</option>
                          </select>
                        </label>
                        {#if parameter.value_type === "object"}
                          <label class="label">
                            <span class="label-text text-xs">Value Field</span>
                            <input type="text" class="input" bind:value={parameter.value_field} onchange={onChange} />
                          </label>
                        {/if}
                        <label class="label">
                          <span class="label-text text-xs">Default</span>
                          <select class="select" bind:value={parameter.default} onchange={onChange}>
                            {#each parameter.values as value}
                              <option {value}>{value}</option>
                            {/each}
                          </select>
                        </label>
                      </div>
                      <div class="mt-4">
                        <span class="label-text text-xs font-semibold text-left">Values</span>
                        <div class="flex flex-wrap gap-2 mt-1">
                          {#each parameter.values as value, valueIndex}
                            <div class="flex items-center bg-surface-100-900 rounded pl-3 pr-2 py-1">
                              <span class="text-sm" aria-label="Value {valueIndex + 1}">
                                {value}
                              </span>
                              <button
                                type="button"
                                class="ml-2 text-surface-500 hover:text-error-500"
                                onclick={() => removeReasoningValue(parameter, valueIndex)}
                              >
                                <icons.X size="16" />
                              </button>
                            </div>
                          {/each}
                          <div class="flex">
                            <input
                              type="text"
                              class="grow px-2 py-0 border border-dashed border-surface-300-700 rounded-l w-24 bg-surface-50-950"
                              placeholder="e.g. max"
                              onkeydown={(event) => {
                                if (event.key === "Enter") addReasoningValue(event.currentTarget, parameter);
                              }}
                              onchange={onChange}
                            />
                            <button
                              type="button"
                              class="px-2 py-0 bg-surface-200-800 border border-l-0 border-surface-300-700 rounded-r hover:bg-surface-300-700"
                              onclick={(event) => {
                                const input =
                                  event.currentTarget.parentElement?.querySelector<HTMLInputElement>("input");
                                if (input) addReasoningValue(input, parameter);
                              }}
                            >
                              Add
                            </button>
                          </div>
                        </div>
                        {#if parameter.value_type === "object"}
                          <div class="mt-6">
                            <span class="label-text text-xs font-semibold text-left"> Constant fields (optional) </span>
                            {#each Object.entries(parameter.constant_fields ?? {}) as [fieldName, fieldValue]}
                              <div class="grid grid-cols-[1fr_1fr_auto] items-center gap-2 mt-2">
                                <input
                                  type="text"
                                  class="input text-xs"
                                  id="constant-field-name-{api.id}-{fieldName}"
                                  placeholder="field name"
                                  aria-label="Constant field name"
                                  value={fieldName}
                                  onchange={(event) => renameConstantField(parameter, fieldName, event)}
                                />
                                <input
                                  type="text"
                                  class="input text-xs"
                                  id="constant-field-value-{api.id}-{fieldName}"
                                  placeholder="constant value (e.g. high, 10, true, null)"
                                  aria-label="Constant field value"
                                  value={String(fieldValue ?? "")}
                                  onchange={(event) => updateConstantFieldValue(parameter, fieldName, event)}
                                />
                                <button
                                  type="button"
                                  class="btn btn-sm preset-outlined-error-500 h-full"
                                  aria-label="Remove constant field {fieldName}"
                                  onclick={() => removeConstantField(parameter, fieldName)}
                                >
                                  <icons.X size="16" />
                                </button>
                              </div>
                            {/each}
                            <div class="flex justify-start">
                              <button
                                type="button"
                                class="btn btn-sm preset-outlined-primary-500 mt-2"
                                onclick={() => addConstantField(parameter)}
                              >
                                + Add field
                              </button>
                            </div>
                          </div>
                        {/if}
                      </div>
                      <div class="flex justify-end">
                        <button
                          type="button"
                          class="btn btn-sm preset-tonal-error mt-3"
                          onclick={() => removeReasoningParameter(api, parameterIndex)}
                        >
                          <icons.Trash /> Remove parameter
                        </button>
                      </div>
                    </div>
                  {/each}
                  <button
                    type="button"
                    class="btn btn-sm preset-outlined-primary-500"
                    onclick={() => addReasoningParameter(api)}
                  >
                    + Add parameter
                  </button>
                {/if}
              </div>

              <div class="flex justify-between mt-2">
                <div class="flex items-center gap-2">
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
                  <span class="vr"></span>
                  <button
                    type="button"
                    class="btn btn-sm preset-tonal"
                    disabled={!api.api_url || !api.api_key || !api.model}
                    onclick={() => onTestGenerationApi(api)}
                  >
                    <icons.ArrowRightFromLine />Test API
                  </button>
                  <button
                    type="button"
                    class="btn btn-sm preset-tonal"
                    disabled={!api.api_url || !api.api_key || !api.model}
                    onclick={() => onCopyCurl(api)}
                  >
                    {#if curlCopied}
                      <icons.Check />
                    {:else}
                      <icons.Copy />
                    {/if}
                    cURL
                  </button>
                </div>
                <button
                  type="button"
                  class="btn btn-sm preset-filled-error-500"
                  onclick={() => removeApi(i)}
                  disabled={generationProviders.length === 1}
                >
                  <icons.Trash /> Remove API
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
                    placeholder={"{}"}
                    onchange={onChange}
                  />
                  <p class="text-sm text-surface-500 mt-1">{"Use {} as placeholder for the document text"}</p>
                </label>

                <label class="label">
                  <span class="label-text">Query Format</span>
                  <input
                    type="text"
                    id="query-format-{i}"
                    class="input"
                    bind:value={api.query_format}
                    placeholder={"Represent this sentence for searching relevant passages: {}"}
                    onchange={onChange}
                  />
                  <p class="text-sm text-surface-500 mt-1">{"Use {} as placeholder for the query text"}</p>
                </label>
              </div>

              <div class="flex justify-between">
                <div class="flex items-center gap-2">
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
                  <icons.Trash /> Remove API
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
