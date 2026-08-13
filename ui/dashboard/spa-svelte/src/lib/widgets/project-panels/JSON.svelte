<script lang="ts">
  import * as icons from "@lucide/svelte";
  import { selectedProject } from "../../store";
  import type { ProjectItem } from "../../../app";

  interface Props {
    projectItem: ProjectItem | null;
  }
  let { projectItem }: Props = $props();

  let copied = $state(false);
  async function onCopy() {
    await navigator.clipboard.writeText(jsonSettingsStr);
    copied = true;
    setTimeout(() => (copied = false), 1500);
  }

  const jsonData = $derived(projectItem?.jsonData);
  const projectTitle = $derived(projectItem?.jsonData.source.project_title);

  const jsonSettingsStr = $derived(JSON.stringify(jsonData || {}, null, 2));
</script>

{#if jsonData}
  <div class="h-full p-4 overflow-y-auto">
    <div class="rounded-md shadow p-4 flex flex-col gap-4">
      <div class="mb-4 flex items-center justify-between">
        <h2 class="text-xl font-bold flex items-center gap-2">
          <icons.FileBraces size={24} />
          JSON Configuration
        </h2>
        <code class="px-2 rounded text-lg">{projectTitle}</code>
      </div>
      <div class="flex items-center space-x-1">
        <span class="font-semibold">File location:</span>
        <span class="font-semibold2">{$selectedProject?.settingsFilePath}</span>
      </div>
      <div class="relative">
        <pre
          class="pre text-left min-h-40 overflow-x-auto">{jsonSettingsStr}</pre>
        <button
          class="absolute top-2 right-2 px-2 py-1 text-xs rounded bg-gray-700 text-white hover:bg-gray-600"
          onclick={onCopy}
        >
          {copied ? "Copied!" : "Copy"}
        </button>
      </div>
    </div>
  </div>
{:else}
  <div>Unable to generate the settings JSON</div>
{/if}
