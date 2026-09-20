<script lang="ts">
  import { Tabs } from "@skeletonlabs/skeleton-svelte";
  import Source from "./project-panels/Source.svelte";
  import Embedding from "./project-panels/Embedding.svelte";
  import Generation from "./project-panels/Generation.svelte";
  import Database from "./project-panels/Database.svelte";
  import Chunking from "./project-panels/Chunking.svelte";
  import Logging from "./project-panels/Logging.svelte";
  import Tokenizer from "./project-panels/Tokenizer.svelte";
  import JSON from "./project-panels/JSON.svelte";
  import type { ProjectItem } from "../../app";
  import { projectStore } from "../store.svelte";
  import * as icons from "@lucide/svelte";

  let value = $state("sources");

  let changedProject: ProjectItem | null = $state(projectStore.selected);

  const projectItem = $derived(changedProject || projectStore.selected);

  function onChanged(p: ProjectItem) {
    changedProject = p;
  }

  function onTabChange(details: any) {
    value = details.value;
  }
  function selected(va: string, v: string) {
    return va == v ? "bg-primary-50-950/50 text-primary-950-50" : "";
  }
</script>

{#if projectStore.selected}
  <Tabs {value} class="h-full w-full text-sm" orientation="vertical" onValueChange={onTabChange}>
    <Tabs.List class="items-start">
      <Tabs.Trigger value="sources" class="text-sm {selected(value, 'sources')}">
        <icons.FileText size={16} /><span class="hidden lg:inline">Sources</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="generation" class="text-sm {selected(value, 'generation')}">
        <icons.Merge size={16} /><span class="hidden lg:inline">Generation</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="embedding" class="text-sm {selected(value, 'embedding')}">
        <icons.Split size={16} /><span class="hidden lg:inline">Embedding</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="chunking" class="text-sm {selected(value, 'chunking')}">
        <icons.Scissors size={16} /><span class="hidden lg:inline">Chunking</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="database" class="text-sm {selected(value, 'database')}">
        <icons.Database size={16} /><span class="hidden lg:inline">Database</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="logging" class="text-sm {selected(value, 'logging')}">
        <icons.Logs size={16} /><span class="hidden lg:inline">Logging</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="tokenizer" class="text-sm {selected(value, 'tokenizer')}">
        <icons.CodeXml size={16} /><span class="hidden lg:inline">Tokenizer</span>
      </Tabs.Trigger>
      <Tabs.Trigger value="json" class="text-sm {selected(value, 'json')}">
        <icons.FileBraces size={16} /><span class="hidden lg:inline">JSON</span>
      </Tabs.Trigger>
      <Tabs.Indicator />
      <div class="ml-auto flex items-center"></div>
    </Tabs.List>
    <Tabs.Content value="sources" class="grow">
      <Source {onChanged} />
    </Tabs.Content>
    <Tabs.Content value="generation" class="grow">
      <Generation {onChanged} />
    </Tabs.Content>
    <Tabs.Content value="embedding" class="grow">
      <Embedding {onChanged} />
    </Tabs.Content>
    <Tabs.Content value="chunking" class="grow">
      <Chunking {onChanged} />
    </Tabs.Content>
    <Tabs.Content value="database" class="grow">
      <Database {onChanged} />
    </Tabs.Content>
    <Tabs.Content value="logging" class="grow">
      <Logging {onChanged} />
    </Tabs.Content>
    <Tabs.Content value="tokenizer" class="grow">
      <Tokenizer {onChanged} />
    </Tabs.Content>
    <Tabs.Content value="json" class="grow">
      <JSON {projectItem} />
    </Tabs.Content>
  </Tabs>
{:else}
  <div class="h-full flex items-center justify-center">
    <span class="text-surface-400 italic">No project selected</span>
  </div>
{/if}
