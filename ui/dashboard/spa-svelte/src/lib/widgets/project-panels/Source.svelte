<!-- Source.svelte -->
<script lang="ts">
  import * as icons from "@lucide/svelte";
  import { selectedProject } from "../../store";
  import { onMount } from "svelte";
  import { helper_saveProjectSettings } from "../../utils";

  const jsonData = $derived($selectedProject?.jsonData);
  const projectTitle = $derived($selectedProject?.jsonData.source.project_title);

  let newExtension = $state("");
  let newGlobalExclude = $state("");
  let newPathExclude = $state("");
  let newPathExtension = $state("");

  onMount(() => {});

  function onChange() {
    if ($selectedProject) {
      $selectedProject = $selectedProject;
      helper_saveProjectSettings($selectedProject);
    }
  }

  function addExtension() {
    if (!jsonData) {
      return;
    }
    if (newExtension.trim()) {
      jsonData.source.default_extensions.push(newExtension.trim());
      newExtension = "";
      onChange();
    }
  }

  function removeExtension(index: number) {
    if (!jsonData) {
      return;
    }
    jsonData.source.default_extensions.splice(index, 1);
    onChange();
  }

  function removePathExtension(pathIndex: number, extensionIndex: number) {
    if (!jsonData) {
      return;
    }
    jsonData.source.paths[pathIndex].extensions.splice(extensionIndex, 1);
    onChange();
  }

  function addPathExtension(pathIndex: number) {
    if (!jsonData) {
      return;
    }
    if (newPathExtension.trim()) {
      jsonData.source.paths[pathIndex].extensions.push(newPathExtension.trim());
      newPathExtension = "";
    }
    onChange();
  }

  function addGlobalExclude() {
    if (!jsonData) {
      return;
    }
    if (newGlobalExclude.trim()) {
      jsonData.source.global_exclude.push(newGlobalExclude.trim());
      newGlobalExclude = "";
    }
    onChange();
  }

  function removeGlobalExclude(index: number) {
    if (!jsonData) {
      return;
    }
    jsonData.source.global_exclude.splice(index, 1);
    onChange();
  }

  function addPath() {
    if (!jsonData) {
      return;
    }
    jsonData.source.paths.push({
      exclude: [],
      extensions: [],
      path: "./",
      recursive: true,
      type: "directory",
    });
    onChange();
  }

  function removePath(index: number) {
    if (!jsonData) {
      return;
    }
    jsonData.source.paths.splice(index, 1);
    onChange();
  }

  function addPathExclude(pathIndex: number) {
    if (!jsonData) {
      return;
    }
    if (newPathExclude.trim()) {
      jsonData.source.paths[pathIndex].exclude.push(newPathExclude.trim());
      newPathExclude = "";
    }
    onChange();
  }

  function removePathExclude(pathIndex: number, excludeIndex: number) {
    if (!jsonData) {
      return;
    }
    jsonData.source.paths[pathIndex].exclude.splice(excludeIndex, 1);
    onChange();
  }

  function movePathUp(index: number) {
    if (!jsonData) {
      return;
    }
    if (index > 0) {
      const temp = jsonData.source.paths[index];
      jsonData.source.paths[index] = jsonData.source.paths[index - 1];
      jsonData.source.paths[index - 1] = temp;
    }
    onChange();
  }

  function movePathDown(index: number) {
    if (!jsonData) {
      return;
    }
    if (index < jsonData.source.paths.length - 1) {
      const temp = jsonData.source.paths[index];
      jsonData.source.paths[index] = jsonData.source.paths[index + 1];
      jsonData.source.paths[index + 1] = temp;
    }
    onChange();
  }
</script>

{#if $selectedProject === undefined}
  <div class="h-full flex items-center justify-center">
    <span class="text-surface-400 italic">Loading...</span>
  </div>
{:else if $selectedProject}
  <div class="flex flex-col h-full p-4 overflow-auto space-y-6">
    <div class="rounded-lg shadow p-4">
      <div class="mb-4 flex items-center justify-between">
        <h2 class="text-xl font-bold flex items-center gap-2">
          <icons.CodeXml size={24} />
          Sources Settings
        </h2>
        <code class="bg-primary-50-9502 px-2 rounded text-lg">{projectTitle}</code>
      </div>

      <div class="grid grid-cols-1 md:grid-cols-2 gap-4 mb-4">
        <label class="label">
          <span class="label-text">Project ID</span>
          <input
            type="text"
            id="project-id"
            class="input"
            bind:value={$selectedProject.jsonData.source.project_id}
            placeholder="Leave empty to auto-generate"
            onchange={onChange}
          />
        </label>

        <label class="label">
          <span class="label-text">Project Title</span>
          <input
            type="text"
            id="project-title"
            class="input"
            bind:value={$selectedProject.jsonData.source.project_title}
            onchange={onChange}
          />
        </label>
      </div>

      <label class="label">
        <span class="lable-text">Project Description</span>
        <textarea
          id="project-description"
          class="input"
          bind:value={$selectedProject.jsonData.source.project_description}
          rows="3"
          onchange={onChange}
        ></textarea>
      </label>

      <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
        <label class="label">
          <span class="label-text">Encoding</span>
          <input
            type="text"
            id="encoding"
            class="input"
            bind:value={$selectedProject.jsonData.source.encoding}
            onchange={onChange}
          />
        </label>

        <label class="label">
          <span class="label-text">Max File Size (MB)</span>
          <input
            type="number"
            id="max-file-size"
            class="input"
            bind:value={$selectedProject.jsonData.source.max_file_size_mb}
            min="1"
            onchange={onChange}
          />
        </label>
      </div>
    </div>

    <div class="rounded-lg shadow p-4">
      <h2 class="font-semibold text-lg mb-4">Default Extensions</h2>

      <div class="flex flex-wrap gap-2">
        {#each $selectedProject.jsonData.source.default_extensions as ext, i}
          <div class="flex items-center bg-primary-100-900 rounded pl-3 pr-2 py-1">
            <span class="text-sm">{ext}</span>
            <button type="button" class="ml-2 text-blue-500 hover:text-red-500" onclick={() => removeExtension(i)}>
              ×
            </button>
          </div>
        {/each}
        <div class="flex">
          <input
            type="text"
            class="flex-grow px-2 py-0 border border-dashed border-surface-300-700 rounded-l w-24 bg-surface-50-950"
            bind:value={newExtension}
            placeholder="+ .tsx"
            onkeydown={(e) => e.key === "Enter" && addExtension()}
            onchange={onChange}
          />
          <button
            type="button"
            class="px-2 py-0 bg-surface-200-800 border border-l-0 border-surface-300-700 rounded-r hover:bg-surface-300-700"
            onclick={addExtension}
          >
            Add
          </button>
        </div>
      </div>
    </div>

    <div class="rounded-lg shadow p-4">
      <h2 class="text-lg font-semibold mb-4">Global Exclude Patterns</h2>

      <div class="flex flex-wrap gap-2">
        {#each $selectedProject.jsonData.source.global_exclude as pattern, i}
          <div class="flex items-center bg-error-100-900 rounded pl-3 pr-2 py-1">
            <span class="text-sm">{pattern}</span>
            <button
              type="button"
              class="ml-2 text-error-500 hover:text-error-700-300"
              onclick={() => removeGlobalExclude(i)}
            >
              ×
            </button>
          </div>
        {/each}
        <div class="flex">
          <input
            type="text"
            class="flex-grow px-2 py-0 border border-dashed border-surface-300-700 rounded-l w-24 bg-surface-50-950"
            bind:value={newGlobalExclude}
            placeholder="+ */temp/*"
            onkeydown={(e) => e.key === "Enter" && addGlobalExclude()}
            onchange={onChange}
          />
          <button
            type="button"
            class="px-2 py-0 bg-surface-200-800 border border-l-0 border-surface-300-700 rounded-r hover:bg-surface-300-700"
            onclick={addGlobalExclude}
          >
            Add
          </button>
        </div>
      </div>
    </div>

    <div class="rounded-lg shadow p-4">
      <div class="flex justify-between items-center mb-4">
        <h2 class="text-xl font-bold">Paths ({$selectedProject.jsonData.source.paths.length})</h2>
        <button type="button" class="btn px-3 py-1 preset-filled-primary-500 rounded-md" onclick={addPath}>
          Add Path
        </button>
      </div>

      {#each $selectedProject.jsonData.source.paths as path, i}
        <div class="border border-surface-200-800 rounded-md p-4 mb-4">
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4 mb-4">
            <label class="label">
              <span class="label-text">Path</span>
              <input type="text" id="path-{i}" class="input" bind:value={path.path} onchange={onChange} />
            </label>

            <label class="label">
              <span class="label-text">Type</span>
              <select id="type-{i}" class="input" bind:value={path.type} onchange={onChange}>
                <option value="directory">Directory</option>
                <option value="file">File</option>
              </select>
            </label>
          </div>

          <div class="mb-4">
            {#if path.type === "directory"}
              <div class="flex items-center">
                <input
                  type="checkbox"
                  class="checkbox"
                  bind:checked={path.recursive}
                  id="recursive-{i}"
                  onchange={onChange}
                />
                <label for="recursive-{i}" class="ml-2 block text-sm text-surface-700-300"> Recursive </label>
              </div>
            {/if}
          </div>

          <div class="mb-8">
            <span class="label-text text-left">Exclude Patterns</span>
            <div class="mt-2 flex flex-wrap gap-2">
              {#each path.exclude as exclude, j}
                <div class="flex items-center bg-surface-100-900 rounded pl-3 pr-2 py-1">
                  <span class="text-sm">{exclude}</span>
                  <button
                    type="button"
                    class="ml-2 text-surface-500 hover:text-red-500"
                    onclick={() => removePathExclude(i, j)}
                  >
                    ×
                  </button>
                </div>
              {/each}
              <div class="flex">
                <input
                  type="text"
                  id="exclude-{i}"
                  class="px-2 py-1 border border-dashed border-surface-300-700 rounded-l w-36 bg-surface-50-950"
                  bind:value={newPathExclude}
                  placeholder="+ exclude pattern"
                  onkeydown={(e) => e.key === "Enter" && addPathExclude(i)}
                  onchange={onChange}
                />
                <button
                  type="button"
                  class="px-2 py-1 bg-surface-200-800 border border-l-0 border-surface-300-700 rounded-r hover:bg-surface-300-700"
                  onclick={() => addPathExclude(i)}
                >
                  Add
                </button>
              </div>
            </div>
          </div>

          <div class="mb-8">
            <span class="label-text text-left"> Extensions (empty = default extensions) </span>
            <div class="mt-2 flex flex-wrap gap-2">
              {#each path.extensions as ext, j}
                <div class="flex items-center bg-surface-100-900 rounded pl-3 pr-2 py-1">
                  <span class="text-sm">{ext}</span>
                  <button
                    type="button"
                    class="ml-2 text-surface-500 hover:text-red-500"
                    onclick={() => removePathExtension(i, j)}
                  >
                    ×
                  </button>
                </div>
              {/each}
              <div class="flex">
                <input
                  type="text"
                  id="extension-{i}"
                  class="px-2 py-1 border border-dashed border-surface-300-700 rounded-l w-36 bg-surface-50-950"
                  bind:value={newPathExtension}
                  placeholder="+ exclude pattern"
                  onkeydown={(e) => e.key === "Enter" && addPathExtension(i)}
                  onchange={onChange}
                />
                <button
                  type="button"
                  class="px-2 py-1 bg-surface-200-800 border border-l-0 border-surface-300-700 rounded-r hover:bg-surface-300-700"
                  onclick={() => addPathExtension(i)}
                >
                  Add
                </button>
              </div>
            </div>
          </div>

          <div class="flex justify-between">
            <div class="space-x-2">
              <button type="button" class="btn preset-tonal-primary" onclick={() => movePathUp(i)} disabled={i === 0}>
                ↑ Up
              </button>
              <button
                type="button"
                class="preset-tonal-primary btn"
                onclick={() => movePathDown(i)}
                disabled={i === $selectedProject.jsonData.source.paths.length - 1}
              >
                ↓ Down
              </button>
            </div>
            <button
              type="button"
              class="btn preset-filled-error-500"
              onclick={() => removePath(i)}
              disabled={$selectedProject.jsonData.source.paths.length === 1}
            >
              Remove Path
            </button>
          </div>
        </div>
      {/each}
    </div>
  </div>
{/if}

<style>
  .label {
    text-align: left;
  }
</style>
