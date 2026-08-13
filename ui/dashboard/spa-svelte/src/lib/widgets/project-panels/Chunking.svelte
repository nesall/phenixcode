<script lang="ts">
  import { onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import { selectedProject } from "../../store";
  import { helper_saveProjectSettings } from "../../utils";

  interface Props {
    onChanged: any;
  }
  let { onChanged }: Props = $props();

  const projectTitle = $derived(
    $selectedProject?.jsonData.source.project_title,
  );

  onMount(() => {});

  function onChange() {
    if ($selectedProject) {
      helper_saveProjectSettings($selectedProject);
      onChanged($selectedProject);
    }
  }
</script>

{#if $selectedProject}
  <div class="h-full p-4 overflow-auto">
    <form class="w-full">
      <fieldset class="space-y-4">
        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="flex justify-between items-center mb-4">
            <h2 class="text-xl font-bold flex items-center gap-2">
              <icons.Split size={24} />
              Document Chunking Settings
            </h2>
            <code class="px-2 rounded text-lg">{projectTitle}</code>
          </div>

          <!-- Semantic Chunking Toggle -->
          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2">
            Chunking Strategy
          </h3>
          <div
            class="flex items-center space-x-3 p-3 bg-surface-50-900 rounded-lg"
          >
            <input
              type="checkbox"
              class="checkbox checkbox-lg preset-filled-primary-500"
              bind:checked={$selectedProject.jsonData.chunking.semantic}
              id="semantic-toggle"
              onchange={onChange}
            />
            <label for="semantic-toggle" class="flex flex-col cursor-pointer">
              <span class="font-medium text-left text-surface-900-50"
                >Enable Semantic Chunking</span
              >
              <span class="text-xs text-surface-500-400"
                >Use sentence boundaries and embedding distances to
                intelligently split documents.</span
              >
            </label>
          </div>

          <!-- Chunk Size Parameters -->
          <h3
            class="font-semibold text-lg border-b border-surface-500 pb-2 pt-2"
          >
            Chunk Size Limits
          </h3>
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <label class="label">
              <span class="label-text">Min Tokens per Chunk</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.chunking.nof_min_tokens}
                min="1"
                max={$selectedProject.jsonData.chunking.nof_max_tokens}
                onchange={onChange}
              />
              <p class="text-sm text-surface-500 mt-1">
                Minimum required token count for a valid chunk.
              </p>
            </label>

            <label class="label">
              <span class="label-text">Max Tokens per Chunk</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.chunking.nof_max_tokens}
                min={$selectedProject.jsonData.chunking.nof_min_tokens}
                onchange={onChange}
              />
              <p class="text-sm text-surface-500 mt-1">
                Maximum token count before a chunk is forced to split.
              </p>
            </label>
          </div>

          <!-- Overlap -->
          <h3
            class="font-semibold text-lg border-b border-surface-500 pb-2 pt-2"
          >
            Overlap
          </h3>
          <label class="label md:w-1/2">
            <span class="label-text">Overlap Percentage</span>
            <input
              type="number"
              class="input"
              bind:value={$selectedProject.jsonData.chunking.overlap_percentage}
              min="0"
              max="1"
              step="0.05"
              onchange={onChange}
            />
            <p class="text-sm text-surface-500 mt-1">
              The ratio (0.0 to 1.0) of previous content to include in the start
              of the next chunk for context preservation.
            </p>
          </label>
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
