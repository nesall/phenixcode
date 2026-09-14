<script lang="ts">
  import { onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import { slide } from "svelte/transition";
  import { selectedProject } from "../../store";
  import UpDownButton from "../misc/UpDownButton.svelte";
  import { helper_saveProjectSettings } from "../../utils";

  const jsonData = $derived($selectedProject?.jsonData);
  const projectTitle = $derived(
    $selectedProject?.jsonData.source.project_title,
  );

  interface Props {
    onChanged: any;
  }
  let { onChanged }: Props = $props();

  onMount(() => {});

  // function addApi() {
  //   if (!jsonData) {
  //     return;
  //   }

  //   const newId = `api_${Date.now()}`;
  //   jsonData.embedding.apis.push({
  //     api_key: "",
  //     api_url: "",
  //     id: newId,
  //     model: "",
  //     name: "New API",
  //     document_format: "{}",
  //     query_format:
  //       "Represent this sentence for searching relevant passages: {}",
  //   });
  //   jsonData.embedding.current_api = newId;
  //   // selectedJsonSettings.set(jsonData);
  //   onChange();
  // }

  // function removeApi(index: number) {
  //   if (!jsonData) {
  //     return;
  //   }

  //   if (1 < jsonData.embedding.apis.length) {
  //     jsonData.embedding.apis.splice(index, 1);
  //     // If we removed the current API, switch to the first one
  //     if (
  //       jsonData.embedding.current_api === jsonData.embedding.apis[index]?.id
  //     ) {
  //       jsonData.embedding.current_api = jsonData.embedding.apis[0]?.id || "";
  //     }
  //     // selectedJsonSettings.set(jsonData);
  //     onChange();
  //   }
  // }

  // function moveApiUp(index: number) {
  //   if (!jsonData) {
  //     return;
  //   }

  //   if (0 < index) {
  //     const temp = jsonData.embedding.apis[index];
  //     jsonData.embedding.apis[index] = jsonData.embedding.apis[index - 1];
  //     jsonData.embedding.apis[index - 1] = temp;
  //     // selectedJsonSettings.set(jsonData);
  //     onChange();
  //   }
  // }

  // function moveApiDown(index: number) {
  //   if (!jsonData) {
  //     return;
  //   }

  //   if (index < jsonData.embedding.apis.length - 1) {
  //     const temp = jsonData.embedding.apis[index];
  //     jsonData.embedding.apis[index] = jsonData.embedding.apis[index + 1];
  //     jsonData.embedding.apis[index + 1] = temp;
  //     // selectedJsonSettings.set(jsonData);
  //     onChange();
  //   }
  // }

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
      onChanged($selectedProject);
    }
  }

  // function onExpandAll() {
  //   if (!$selectedProject) {
  //     return;
  //   }
  //   for (const api of $selectedProject?.jsonData.embedding.apis) {
  //     api._hidden = false;
  //   }
  //   $selectedProject = $selectedProject;
  // }

  // function onCollapseAll() {
  //   if (!$selectedProject) {
  //     return;
  //   }
  //   for (const api of $selectedProject?.jsonData.embedding.apis) {
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
                bind:value={
                  $selectedProject.jsonData.embedding.prepend_label_format
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
                value={$selectedProject.jsonData.embedding.current_api}
                onchange={onCurApiChange}
              >
                {#each $selectedProject.jsonData.embedding.enabled_providers as api}
                  <option value={api}>{api}</option>
                {/each}
              </select>
            </label>
          </div>
        </div>

        <div class="rounded-md shadow p-4 flex flex-col gap-4">

          {#each $selectedProject.jsonData.embedding.enabled_providers as api, i}
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
