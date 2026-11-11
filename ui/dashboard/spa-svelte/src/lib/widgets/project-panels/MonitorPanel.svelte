<script lang="ts">
  import { onDestroy } from "svelte";
  import { Tabs } from "@skeletonlabs/skeleton-svelte";
  import * as icons from "@lucide/svelte";
  import { mapProjectToInstance } from "../../utils";
  import { instances, projectStore } from "../../store.svelte";
  import { selectMonitorProject } from "../../stores/monitor.svelte";
  import Database from "./monitor/Database.svelte";
  import Embeddings from "./monitor/Embeddings.svelte";
  import Metrics from "./monitor/Metrics.svelte";
  import Cost from "./monitor/Cost.svelte";
  import Logs from "./monitor/Logs.svelte";

  let { active = true }: { active?: boolean } = $props();
  let value = $state("embeddings");
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
  <Tabs {value} class="h-full min-w-0 w-full text-sm" orientation="vertical" onValueChange={onTabChange}>
    <Tabs.List class="items-start">
      <Tabs.Trigger value="embeddings" class="text-sm {selected(value, 'embeddings')} w-full justify-start">
        <icons.Layers size={16} /><span class="hidden lg:inline">Embeddings</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="database" class="text-sm {selected(value, 'database')} w-full justify-start">
        <icons.Database size={16} /><span class="hidden lg:inline">Database</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="metrics" class="text-sm {selected(value, 'metrics')} w-full justify-start">
        <icons.ChartNoAxesCombined size={16} /><span class="hidden lg:inline">Metrics</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="cost" class="text-sm {selected(value, 'cost')} w-full justify-start">
        <icons.DollarSign size={16} /><span class="hidden lg:inline">Cost</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="logs" class="text-sm {selected(value, 'logs')} w-full justify-start">
        <icons.ScrollText size={16} /><span class="hidden lg:inline">Logs</span>
      </Tabs.Trigger>
      <Tabs.Indicator />
    </Tabs.List>
    <Tabs.Content value="embeddings" class="grow"><Embeddings /></Tabs.Content>
    <Tabs.Content value="database" class="grow min-h-0 min-w-0"><Database active={active && value === "database"} /></Tabs.Content>
    <Tabs.Content value="metrics" class="grow"><Metrics /></Tabs.Content>
    <Tabs.Content value="cost" class="grow"><Cost /></Tabs.Content>
    <Tabs.Content value="logs" class="grow"><Logs /></Tabs.Content>
  </Tabs>
{:else}
  <div class="h-full flex items-center justify-center">
    <span class="text-surface-400 italic">No project selected</span>
  </div>
{/if}