<script lang="ts">
  import { Tabs } from "@skeletonlabs/skeleton-svelte";
  import Instance from "./project-panels/InstancePanel.svelte";
  import ConfigPanel from "./project-panels/ConfigPanel.svelte";
  import { instances, projectStore } from "../store.svelte";
  import Monitor from "./project-panels/MonitorPanel.svelte";
  import { monitorStore } from "../stores/monitor.svelte";
  import { mapProjectToInstance } from "../utils";
  let { active = true }: { active?: boolean } = $props();
  let tab = $state("config");
  const stale = $derived(!!monitorStore.snapshot && monitorStore.nowSeconds - monitorStore.snapshot.last_heartbeat > 10);
  const hasInstance = $derived(!!projectStore.selected && !!mapProjectToInstance(projectStore.selected, instances));
</script>

{#if projectStore.selected}
  <Tabs value={tab} onValueChange={details => tab = details.value} class="h-full min-w-0 text-sm">
    <Tabs.List class="flex-wrap">
      <Tabs.Trigger value="config" class="text-sm bg-primary-100-900 h-8">Configuration</Tabs.Trigger>
      <Tabs.Trigger value="instance" class="text-sm bg-secondary-100-900 h-8">Instance</Tabs.Trigger>
      <Tabs.Trigger value="monitor" class="text-sm bg-tertiary-100-900 h-8">Monitor</Tabs.Trigger>
      <Tabs.Indicator />
      {#if tab === "monitor"}
        <div class="ml-auto flex flex-wrap items-center justify-end gap-2 px-2 text-xs" aria-label="Monitor status">
          {#if stale}<span class="badge preset-filled-warning-500">Stale</span>{/if}
          <span class="text-surface-500">{hasInstance ? (monitorStore.connected ? "Live" : "Polling") : "Offline snapshot"}</span>
        </div>
      {/if}
    </Tabs.List>
    <Tabs.Content value="config" class="h-0 flex-grow">
      <ConfigPanel />
    </Tabs.Content>
    <Tabs.Content value="instance" class="h-0 flex-grow">
      <Instance />
    </Tabs.Content>
    <Tabs.Content value="monitor" class="h-0 min-w-0 flex-grow">
      <Monitor active={active && tab === "monitor"} />
    </Tabs.Content>
  </Tabs>
{:else}
  <div class="h-full flex items-center justify-center">
    <span class="text-surface-400 italic">No project selected</span>
  </div>
{/if}
