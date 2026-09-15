<script lang="ts">
  import { Tabs } from "@skeletonlabs/skeleton-svelte";
  import Instance from "./project-panels/Instance.svelte";
  import ConfigPanel from "./ConfigPanel.svelte";
  import { projectStore } from "../store.svelte";
</script>

{#if projectStore.selected}
  <Tabs defaultValue="config" class="h-full text-sm">
    <Tabs.List class="">
      <Tabs.Trigger value="config" class="text-sm code">Configuration</Tabs.Trigger>
      <Tabs.Trigger value="instance" class="text-sm bg-tertiary-100-900">Instance</Tabs.Trigger>
      <Tabs.Indicator />
      <div class="ml-auto flex items-center"></div>
    </Tabs.List>
    <Tabs.Content value="config" class="h-0 flex-grow">
      <ConfigPanel />
    </Tabs.Content>
    <Tabs.Content value="instance" class="h-0 flex-grow">
      <Instance />
    </Tabs.Content>
  </Tabs>
{:else}
  <div class="h-full flex items-center justify-center">
    <span class="text-surface-400 italic">No project selected</span>
  </div>
{/if}
