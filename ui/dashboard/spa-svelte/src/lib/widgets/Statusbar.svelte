<script lang="ts">
  import { instances, projectStore } from "../store.svelte";

  const selectedProject = $derived(projectStore.selected?.jsonData.source.project_title || "No project selected");
  const availableProjects = $derived(projectStore.list.length);
  const runningInstances = $derived(instances.filter((instance) => instance.status === "healthy").length);
</script>

<div class="flex space-x-2 items-center w-full bg-surface-100-900 px-2 py-1 text-xs justify-between">
  <span>{selectedProject}</span>
  <div>
    <span>{availableProjects} {availableProjects === 1 ? "project" : "projects"}</span>
    <span>|</span>
    <span>{runningInstances} {runningInstances === 1 ? "running instance" : "running instances"}</span>
  </div>
</div>
