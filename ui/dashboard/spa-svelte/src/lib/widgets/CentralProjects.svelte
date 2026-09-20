<script lang="ts">
  import { getContext, onMount } from "svelte";
  import * as icons from "@lucide/svelte";
  import ProjectPanel from "./ProjectPanel.svelte";
  import {
    helper_createProject,
    helper_deleteProject,
    helper_importProject,
    mapProjectToInstance,
    toaster,
  } from "../utils";
  import type { ProjectItem } from "../../app";
  import { slide, fly, fade } from "svelte/transition";
  import { projectStore, instances } from "../store.svelte";

  const fetchProjects: (notify?: boolean) => void = getContext("FetchProjects");

  onMount(() => {
    fetchProjects();
  });

  let drawerOpen = $state(false);

  async function onItemClick(item: ProjectItem) {
    console.log("Switching to project", $state.snapshot(item.jsonData));
    projectStore.selected = item;
  }

  async function onAddProject() {
    try {
      const res = await helper_createProject();
      if (res.status === "error") throw { message: res.message };
      projectStore.selected = res;
      fetchProjects();
      toaster.success({ title: "Project created successfully." });
    } catch (error: any) {
      console.log("onAddProject", error);
      toaster.error({ title: "Failed to create project. " + error.message });
    }
  }

  async function onDeleteProject() {
    try {
      if (!projectStore.selected) return;
      const res = await helper_deleteProject(projectStore.selected);
      if (res.status === "success") {
        fetchProjects();
        projectStore.selected = null;
        toaster.success({ title: "Project deleted successfully." });
      } else {
        toaster.error({ title: res.message });
      }
    } catch (error) {
      toaster.error({ title: "Failed to delete project." });
    }
  }

  async function onImportProject() {
    if (window.cppApi) {
      window.cppApi
        .pickSettingsJsonFile()
        .then((config: { project_id: string; path: string } | null) => {
          if (config) {
            helper_importProject(config.project_id, config.path).then((res) => {
              fetchProjects();
              if (res.status === "success") {
                toaster.success({
                  title: "Import Successful",
                  description: `Project ${config.project_id} imported into Projects.`,
                });
              } else {
                console.log("Failed to import project", config.project_id, res.message);
                toaster.error({
                  title: "Failed to import project.",
                  description: `Project ${config.project_id} failed with error ${res.message}`,
                });
              }
            });
          }
        })
        .catch((err) => {
          toaster.error({
            title: "Unable to pick a settings file.",
            description: err.message || err,
          });
        });
    } else {
      toaster.info({ title: "Import not available in web mode." });
    }
  }

  $effect(() => {
    if (projectStore.list) console.log(`CentralProjects.svelte - nof projects available: ${projectStore.list.length}`);
  });
</script>

<svelte:window
  onkeydown={(e) => {
    if (e.key === "Escape") drawerOpen = false;
  }}
/>

{#snippet sidebar()}
  <div class="h-full flex flex-col space-y-1 w-72">
    <div class="flex items-center gap-2">
      <button
        type="button"
        class="btn-icon btn-sm preset-filled-primary-500"
        title="Add new project"
        onclick={onAddProject}
      >
        <icons.Plus />
      </button>
      <button
        type="button"
        class="btn-icon btn-sm preset-filled-primary-500"
        title="Import a project"
        onclick={onImportProject}
      >
        <icons.Import />
      </button>
      <button
        type="button"
        class="btn-icon btn-sm preset-filled-error-500 ml-auto"
        title="Delete selected project"
        disabled={!projectStore.selected || !!mapProjectToInstance(projectStore.selected, instances)}
        onclick={onDeleteProject}
      >
        <icons.Trash2 />
      </button>
    </div>
    <ul class="w-full h-full p-1 shadow overflow-y-auto border border-surface-200-800 rounded min-w-64">
      <div class="bg-surface-100-900 rounded p-2 mb-2 flex items-center">
        Available Projects
        <button
          type="button"
          class="btn-icon btn-sm preset-tonal ml-auto"
          title="Refresh project list"
          onclick={() => fetchProjects(true)}
        >
          <icons.RefreshCw />
        </button>
      </div>
      {#each projectStore.list as item (item.jsonData.source.project_id)}
        <li
          class="hover:bg-surface-200-800 px-2 flex items-center space-x-2 border-b border-surface-200-800"
          transition:slide
        >
          <button
            type="button"
            class="btn p-1 w-full flex items-center space-x-2 justify-start text-sm
            {item.jsonData.source.project_id === projectStore.selected?.jsonData.source.project_id ? 'font-bold' : ''}
            "
            onclick={() => onItemClick(item)}
          >
            {#if mapProjectToInstance(item, instances)}
              <span class="font-monospace text-xs bg-success-300-700 rounded px-2 w-8 font-bold">on</span>
            {:else}
              <span class="font-monospace text-xs bg-surface-100-900 rounded text-surface-800-200 px-2 w-8">off</span>
            {/if}
            <span>{item.jsonData.source.project_title}</span>
            {#if item.jsonData.source.project_id === projectStore.selected?.jsonData.source.project_id}
              <icons.Check size={16} class="ml-auto text-primary-500" />
            {/if}
          </button>
        </li>
      {/each}
    </ul>
  </div>
{/snippet}

<div class="h-full relative">
  <div class="h-full flex">
    <!-- Desktop static sidebar -->
    <div class="h-full max-w-xs hidden lg:block w-72">
      {@render sidebar()}
    </div>

    <!-- Main content panel -->
    <div class="grow h-full lg:pl-4 overflow-hidden">
      <ProjectPanel />
    </div>
  </div>

  <!-- Bottom-left trigger button for screens < lg -->
  <button
    type="button"
    class="lg:hidden fixed bottom-8 left-1 z-30 btn preset-filled-primary-500 shadow-lg flex items-center gap-2 rounded-xl px-4 py-2"
    onclick={() => (drawerOpen = true)}
  >
    <icons.PanelLeftOpen size={18} />
    <span class="text-sm font-medium">Projects</span>
  </button>

  <!-- Mobile Drawer + Backdrop -->
  {#if drawerOpen}
    <button
      type="button"
      tabindex="-1"
      aria-label="Close drawer backdrop"
      class="fixed inset-0 bg-black/50 backdrop-blur-xs z-40 lg:hidden cursor-default w-full h-full border-none p-0"
      onclick={() => (drawerOpen = false)}
      transition:fade={{ duration: 150 }}
    ></button>

    <aside
      class="fixed inset-y-0 left-0 w-80 max-w-[85vw] bg-surface-50-950 p-4 shadow-xl z-50 flex flex-col space-y-3 lg:hidden"
      transition:fly={{ x: -280, duration: 200 }}
    >
      <div class="flex items-center justify-between pb-2 border-b border-surface-200-800">
        <span class="font-semibold text-sm">Projects</span>
        <button type="button" class="btn-icon btn-sm preset-tonal" onclick={() => (drawerOpen = false)}>
          <icons.X size={18} />
        </button>
      </div>

      <div class="grow overflow-hidden">
        {@render sidebar()}
      </div>
    </aside>
  {/if}
</div>
