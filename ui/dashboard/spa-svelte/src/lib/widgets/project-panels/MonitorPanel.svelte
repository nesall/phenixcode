<script lang="ts">
  import { onDestroy } from "svelte";
  import { Tabs } from "@skeletonlabs/skeleton-svelte";
  import * as icons from "@lucide/svelte";
  import { mapProjectToInstance } from "../../utils";
  import { instances, projectStore } from "../../store.svelte";
  import { monitorStore, selectMonitorProject } from "../../stores/monitor.svelte";
  import Embeddings from "./monitor/Embeddings.svelte";
  import Metrics from "./monitor/Metrics.svelte";
  import Cost from "./monitor/Cost.svelte";
  import Logs from "./monitor/Logs.svelte";

  let value = $state("embeddings");
  const stale = $derived(
    !!monitorStore.snapshot && monitorStore.nowSeconds - monitorStore.snapshot.last_heartbeat > 10,
  );
  const hasInstance = $derived(
    !!projectStore.selected && !!mapProjectToInstance(projectStore.selected, instances),
  );

  function selected(va: string, v: string) {
    return va == v ? "bg-primary-50-950/50 text-primary-950-50" : "";
  }

  function onTabChange(details: { value: string }) {
    value = details.value;
  }

  $effect(() => {
    const project = projectStore.selected;
    selectMonitorProject(project, project ? mapProjectToInstance(project, instances) : null);
  });

  onDestroy(() => selectMonitorProject(null, null));
</script>

{#if projectStore.selected}
  <Tabs {value} class="h-full w-full text-sm" orientation="vertical" onValueChange={onTabChange}>
    <Tabs.List class="items-start">
      <Tabs.Trigger value="embeddings" class="text-sm {selected(value, 'embeddings')}">
        <icons.Layers size={16} /><span class="hidden lg:inline">Embeddings</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="metrics" class="text-sm {selected(value, 'metrics')}">
        <icons.ChartNoAxesCombined size={16} /><span class="hidden lg:inline">Metrics</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="cost" class="text-sm {selected(value, 'cost')}">
        <icons.DollarSign size={16} /><span class="hidden lg:inline">Cost</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="logs" class="text-sm {selected(value, 'logs')}">
        <icons.ScrollText size={16} /><span class="hidden lg:inline">Logs</span>
      </Tabs.Trigger>
      <Tabs.Indicator />
      <div class="ml-auto flex items-center gap-2 px-2 text-xs">
        {#if stale}<span class="badge preset-filled-warning-500">Stale</span>{/if}
        <span class="text-surface-500">{hasInstance ? (monitorStore.connected ? "Live" : "Polling") : "Offline snapshot"}</span>
      </div>
    </Tabs.List>
    <Tabs.Content value="embeddings" class="grow"><Embeddings /></Tabs.Content>
    <Tabs.Content value="metrics" class="grow"><Metrics /></Tabs.Content>
    <Tabs.Content value="cost" class="grow"><Cost /></Tabs.Content>
    <Tabs.Content value="logs" class="grow"><Logs /></Tabs.Content>
  </Tabs>
{:else}
  <div class="h-full flex items-center justify-center">
    <span class="text-surface-400 italic">No project selected</span>
  </div>
{/if}