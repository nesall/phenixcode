<script lang="ts">
  import { onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import { selectedProject } from "../../store";
  import type { DatabaseSettings } from "../../../app";
  import { helper_saveProjectSettings, isParentDirValid } from "../../utils";

  const projectTitle = $derived($selectedProject?.jsonData.source.project_title);

  let indexParentDirError = $state("");
  let dbParentDirError = $state("");

  onMount(() => {});

  function onChange() {
    if ($selectedProject) {
      helper_saveProjectSettings($selectedProject);
    }
  }

  async function onDbChange() {
    if ($selectedProject) {
      let path = $selectedProject.jsonData.database.sqlite_path;
      const res = await isParentDirValid(path);
      if (res.status !== "success") {
        dbParentDirError = res.message;
      } else {
        dbParentDirError = "";
      }
      onChange();
    }
  }

  async function onIndexChange() {
    if ($selectedProject) {
      let path = $selectedProject.jsonData.database.index_path;
      const res = await isParentDirValid(path);
      if (res.status !== "success") {
        dbParentDirError = res.message;
      } else {
        dbParentDirError = "";
      }
      onChange();
    }
  }

  // Available distance metrics for the dropdown
  const distanceMetrics: DatabaseSettings["distance_metric"][] = ["cosine", "l2"];

  // Helper to format large numbers
  const formatNumber = (num: number) => new Intl.NumberFormat().format(num);
</script>

{#if $selectedProject}
  <div class="h-full p-4 overflow-auto">
    <form class="w-full">
      <fieldset class="space-y-4">
        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="mb-4 flex justify-between items-center">
            <h2 class="text-xl font-bold flex items-center gap-2">
              <icons.Database size={24} />
              Vector Database Configuration
            </h2>
            <code class="px-2 rounded text-lg">{projectTitle}</code>
          </div>

          <!-- File Paths -->
          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2">Storage Paths</h3>
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <label class="label">
              <span class="label-text">SQLite Metadata Path</span>
              <input
                type="text"
                class="input"
                bind:value={$selectedProject.jsonData.database.sqlite_path}
                placeholder="./db_metadata.db"
                onchange={onDbChange}
              />
              <span>{dbParentDirError}</span>
              <p class="text-sm text-surface-500 mt-1">Path to the SQLite file for document metadata.</p>
            </label>
            <label class="label">
              <span class="label-text">Vector Index Path</span>
              <input
                type="text"
                class="input"
                bind:value={$selectedProject.jsonData.database.index_path}
                placeholder="./db_embeddings.index"
                onchange={onIndexChange}
              />
              <span>{indexParentDirError}</span>
              <p class="text-sm text-surface-500 mt-1">Path to the FASS/HNSW index file for embeddings.</p>
            </label>
          </div>

          <!-- Index Parameters -->
          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2 pt-2">Index Parameters</h3>
          <div class="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
            <label class="label">
              <span class="label-text">Vector Dimension (dim)</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.database.vector_dim}
                min="1"
                onchange={onChange}
              />
              <p class="text-sm text-surface-500 mt-1">Must match the dimension of your embedding model (e.g., 768).</p>
            </label>

            <label class="label">
              <span class="label-text">Max Elements</span>
              <input
                type="number"
                class="input"
                bind:value={$selectedProject.jsonData.database.max_elements}
                min="1000"
                onchange={onChange}
              />
              <p class="text-sm text-surface-500 mt-1">
                Maximum number of vectors the index can hold ({formatNumber(
                  $selectedProject.jsonData.database.max_elements,
                )}).
              </p>
            </label>

            <label class="label">
              <span class="label-text">Distance Metric</span>
              <select class="select" bind:value={$selectedProject.jsonData.database.distance_metric}>
                {#each distanceMetrics as metric}
                  <option value={metric}>{metric.toUpperCase()}</option>
                {/each}
              </select>
              <p class="text-sm text-surface-500 mt-1">
                Choose `cosine` (default) for similarity or `l2` (Euclidean) for distance.
              </p>
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
