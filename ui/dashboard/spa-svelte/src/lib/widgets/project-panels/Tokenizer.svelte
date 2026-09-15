<script lang="ts">
  import { onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import { projectStore } from "../../store.svelte";
  import { helper_saveProjectSettings } from "../../utils";

  interface Props {
    onChanged: any;
  }
  let { onChanged }: Props = $props();

  onMount(() => {});

  function onChange() {
    if (projectStore.selected) {
      helper_saveProjectSettings(projectStore.selected);
      onChanged(projectStore.selected);
    }
  }
</script>

{#if projectStore.selected}
  <div class="h-full p-4 overflow-auto">
    <form class="w-full">
      <fieldset class="space-y-4">
        <div class="rounded-md shadow p-4 flex flex-col gap-4">
          <div class="flex justify-between items-center mb-4">
            <h2 class="text-xl font-bold flex items-center gap-2">
              <icons.TextAlignStart size={24} />
              Tokenizer Configuration
            </h2>
            <code class="px-2 rounded text-lg">{projectStore.selected.jsonData.source.project_title}</code>
          </div>

          <!-- Configuration Path -->
          <h3 class="font-semibold text-lg border-b border-surface-500 pb-2">
            Model Configuration
          </h3>
          <label class="label">
            <span class="label-text">Tokenizer Config Path</span>
            <input
              type="text"
              class="input"
              bind:value={projectStore.selected.jsonData.tokenizer.config_path}
              placeholder="./bge_tokenizer.json"
              onchange={onChange}
            />
            <p class="text-sm text-surface-500 mt-1">
              The local file path to the tokenizer configuration (e.g.,
              HuggingFace `tokenizer.json`).
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
