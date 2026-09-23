<script lang="ts">
  import { onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import { helper_saveProjectSettings } from "../../utils";
  import { generationProviders, projectStore } from "../../store.svelte";

  interface Props {
    onChanged: any;
  }
  let { onChanged }: Props = $props();

  const jsonData = $derived(projectStore.selected?.jsonData);
  const projectTitle = $derived(projectStore.selected?.jsonData.source.project_title);

  let checkedProviders: string[] = $state([...(projectStore.selected?.jsonData.generation.enabled_providers || [])]);

  // Clean any legacy "auto" out of checkedProviders if present
  onMount(() => {
    if (checkedProviders.includes("auto")) {
      const filtered = checkedProviders.filter((id) => id !== "auto");
      checkedProviders = filtered;
      if (jsonData) {
        jsonData.generation.enabled_providers = [...filtered];
        if (jsonData.generation.current_api === "auto") {
          jsonData.generation.current_api = filtered[0] || "";
        }
        onChange();
      }
    }
  });

  function onCurApiChange(event: Event) {
    if (!jsonData) return;
    const selectElem = event.target as HTMLSelectElement;
    jsonData.generation.current_api = selectElem.value;
    onChange();
  }

  function onChange() {
    if (projectStore.selected) {
      projectStore.selected = projectStore.selected;
      helper_saveProjectSettings(projectStore.selected);
      onChanged(projectStore.selected);
    }
  }

  function onToggle(api: string) {
    if (!jsonData) return;
    const index = checkedProviders.indexOf(api);
    if (index === -1) {
      checkedProviders.push(api);
    } else {
      checkedProviders.splice(index, 1);
    }
    jsonData.generation.enabled_providers = [...checkedProviders];

    // If current_api was unchecked, point to the first available
    if (!checkedProviders.includes(jsonData.generation.current_api) && checkedProviders.length > 0) {
      jsonData.generation.current_api = checkedProviders[0];
    }

    onChange();
  }

  // Pure list of enabled concrete models (never contains "auto")
  const availableModelIds = $derived(checkedProviders.filter((id) => id !== "auto"));

  function toggleAutoRouter(enabled: boolean) {
    if (!jsonData) return;
    if (!jsonData.generation.auto_router) {
      const fallbackModel = availableModelIds[0] || "mistral-small";
      jsonData.generation.auto_router = {
        enabled: enabled,
        _validate_ids_exist: true,
        classifier: {
          type: "internal",
          api_id: fallbackModel,
          max_tokens: 20,
          prompt:
            "Classify the user programming task into exactly one tier:\n[TIER_1_SIMPLE]: quick syntax, single function, lookup, explanation\n[TIER_2_REFACTOR]: multi-file changes, bug fixing, medium edits\n[TIER_3_COMPLEX]: deep architectural reasoning, tricky algorithms, math/threading\nAnswer ONLY with the tag.",
          temperature: 0.0,
          timeout_ms: 3500,
        },
        fallback: {
          _comment: "used on classifier timeout, transport error, or unparseable tag",
          default_model_id: fallbackModel,
        },
        routing_rules: {
          tier_1_simple: { direct_model_id: fallbackModel, strategy: "direct" },
          tier_2_refactor: { direct_model_id: fallbackModel, strategy: "direct" },
          tier_3_complex: { direct_model_id: fallbackModel, strategy: "direct" },
        },
      };
    } else {
      jsonData.generation.auto_router.enabled = enabled;
    }
    onChange();
  }
</script>

{#if projectStore.selected}
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
                bind:value={projectStore.selected.jsonData.generation.timeout_ms}
                min="1000"
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Max Context Tokens</span>
              <input
                type="number"
                class="input"
                bind:value={projectStore.selected.jsonData.generation.max_context_tokens}
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
                bind:value={projectStore.selected.jsonData.generation.max_chunks}
                min="1"
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Max Full Sources</span>
              <input
                type="number"
                class="input"
                bind:value={projectStore.selected.jsonData.generation.max_full_sources}
                min="0"
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Max Related / Source</span>
              <input
                type="number"
                class="input"
                bind:value={projectStore.selected.jsonData.generation.max_related_per_source}
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
                bind:value={projectStore.selected.jsonData.generation.default_temperature}
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
                bind:value={projectStore.selected.jsonData.generation.default_max_tokens}
                min="1"
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Default Max Tokens Param Name</span>
              <input
                type="text"
                class="input"
                bind:value={projectStore.selected.jsonData.generation.default_max_tokens_name}
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
                bind:value={projectStore.selected.jsonData.generation.prepend_label_format}
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
              bind:checked={projectStore.selected.jsonData.generation.excerpt.enabled}
              onchange={onChange}
            />
          </h3>
          <div class="grid grid-cols-1 md:grid-cols-3 gap-4">
            <label class="label">
              <span class="label-text">Min Chunks</span>
              <input
                type="number"
                class="input"
                bind:value={projectStore.selected.jsonData.generation.excerpt.min_chunks}
                disabled={!projectStore.selected.jsonData.generation.excerpt.enabled}
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Max Chunks</span>
              <input
                type="number"
                class="input"
                bind:value={projectStore.selected.jsonData.generation.excerpt.max_chunks}
                disabled={!projectStore.selected.jsonData.generation.excerpt.enabled}
                onchange={onChange}
              />
            </label>
            <label class="label">
              <span class="label-text">Threshold Ratio</span>
              <input
                type="number"
                class="input"
                step="0.05"
                bind:value={projectStore.selected.jsonData.generation.excerpt.threshold_ratio}
                disabled={!projectStore.selected.jsonData.generation.excerpt.enabled}
                onchange={onChange}
              />
            </label>
          </div>

          <div class="grid grid-cols-1 md:grid-cols-2 gap-4 pt-4 border-t border-surface-500">
            <label class="label">
              <span class="label-text">Default / Direct API</span>
              <select
                id="current-api-gen"
                class="select"
                value={projectStore.selected.jsonData.generation.current_api}
                onchange={onCurApiChange}
              >
                {#each projectStore.selected.jsonData.generation.enabled_providers as api}
                  <option value={api}>{api}</option>
                {/each}
              </select>
              <p class="text-xs text-surface-500 mt-1">
                Used when auto-routing is disabled or bypassed by a direct request.
              </p>
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

        <!-- AUTO-ROUTER SECTION -->
        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="flex justify-between items-center border-b border-surface-500 pb-2">
            <div>
              <h2 class="text-xl font-bold text-left">Auto-Router</h2>
              <p class="text-xs text-surface-500">Dynamically routes prompts to models by complexity tier.</p>
            </div>
            <label class="flex items-center gap-2 cursor-pointer">
              <span class="text-sm font-semibold">
                {projectStore.selected.jsonData.generation.auto_router?.enabled ? "Enabled" : "Disabled"}
              </span>
              <input
                type="checkbox"
                class="checkbox"
                checked={projectStore.selected.jsonData.generation.auto_router?.enabled ?? false}
                onchange={(e) => toggleAutoRouter((e.target as HTMLInputElement).checked)}
              />
            </label>
          </div>

          {#if projectStore.selected.jsonData.generation.auto_router}
            <div
              class={!projectStore.selected.jsonData.generation.auto_router.enabled
                ? "opacity-50 pointer-events-none"
                : ""}
            >
              <!-- Classifier Settings -->
              <h3 class="font-semibold text-lg border-b border-surface-500 pb-1 pt-2">Classifier</h3>
              <div class="flex items-center gap-4 mt-4 mb-2 justify-start">
                <label class="label flex items-center gap-2 flex-1">
                  <input
                    type="checkbox"
                    class="checkbox"
                    checked={projectStore.selected.jsonData.generation.auto_router.classifier.type === "internal"}
                    onchange={(e) => {
                      const checked = (e.currentTarget as HTMLInputElement).checked;
                      if (projectStore.selected?.jsonData.generation.auto_router) {
                        projectStore.selected.jsonData.generation.auto_router.classifier.type = checked
                          ? "internal"
                          : "api";
                      }
                      onChange();
                    }}
                  />
                  <span class="label-text">Internal classifier</span>
                </label>
                <span class="text-xs text-surface-500 mt-1 flex-grow text-left">
                  Use "internal" for built-in, or specify an external API type.
                </span>
              </div>
              {#if projectStore.selected.jsonData.generation.auto_router.classifier.type === "api"}
                <div class="grid grid-cols-1 md:grid-cols-3 gap-4 mt-8">
                  <label class="label">
                    <span class="label-text">Classifier Model</span>
                    <select
                      class="select"
                      bind:value={projectStore.selected.jsonData.generation.auto_router.classifier.api_id}
                      onchange={onChange}
                    >
                      {#each availableModelIds as api}
                        <option value={api}>{api}</option>
                      {/each}
                    </select>
                  </label>
                  <label class="label">
                    <span class="label-text">Timeout (ms)</span>
                    <input
                      type="number"
                      class="input"
                      bind:value={projectStore.selected.jsonData.generation.auto_router.classifier.timeout_ms}
                      min="500"
                      onchange={onChange}
                    />
                  </label>
                  <label class="label">
                    <span class="label-text">Temperature</span>
                    <input
                      type="number"
                      class="input"
                      step="0.05"
                      min="0"
                      max="2"
                      bind:value={projectStore.selected.jsonData.generation.auto_router.classifier.temperature}
                      onchange={onChange}
                    />
                  </label>
                </div>

                <label class="label mt-4">
                  <span class="label-text">Classifier Prompt</span>
                  <textarea
                    class="textarea font-mono text-sm"
                    rows="4"
                    bind:value={projectStore.selected.jsonData.generation.auto_router.classifier.prompt}
                    onchange={onChange}
                  ></textarea>
                </label>
              {/if}

              <!-- Fallback Settings -->
              <h3 class="font-semibold text-lg border-b border-surface-500 pb-1 pt-4">Fallback</h3>
              <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
                <label class="label">
                  <span class="label-text">Fallback Model</span>
                  <select
                    class="select"
                    bind:value={projectStore.selected.jsonData.generation.auto_router.fallback.default_model_id}
                    onchange={onChange}
                  >
                    {#each availableModelIds as api}
                      <option value={api}>{api}</option>
                    {/each}
                  </select>
                </label>
              </div>

              <!-- Routing Rules -->
              <h3 class="font-semibold text-lg border-b border-surface-500 pb-1 pt-4">Routing Rules</h3>
              <div class="grid grid-cols-1 md:grid-cols-3 gap-4">
                <label class="label">
                  <span class="label-text">Tier 1 Simple</span>
                  <select
                    class="select"
                    bind:value={
                      projectStore.selected.jsonData.generation.auto_router.routing_rules.tier_1_simple.direct_model_id
                    }
                    onchange={onChange}
                  >
                    {#each availableModelIds as api}
                      <option value={api}>{api}</option>
                    {/each}
                  </select>
                </label>
                <label class="label">
                  <span class="label-text">Tier 2 Refactor</span>
                  <select
                    class="select"
                    bind:value={
                      projectStore.selected.jsonData.generation.auto_router.routing_rules.tier_2_refactor
                        .direct_model_id
                    }
                    onchange={onChange}
                  >
                    {#each availableModelIds as api}
                      <option value={api}>{api}</option>
                    {/each}
                  </select>
                </label>
                <label class="label">
                  <span class="label-text">Tier 3 Complex</span>
                  <select
                    class="select"
                    bind:value={
                      projectStore.selected.jsonData.generation.auto_router.routing_rules.tier_3_complex.direct_model_id
                    }
                    onchange={onChange}
                  >
                    {#each availableModelIds as api}
                      <option value={api}>{api}</option>
                    {/each}
                  </select>
                </label>
              </div>
            </div>
          {/if}
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
