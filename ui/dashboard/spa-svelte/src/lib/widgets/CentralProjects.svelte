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
  import { instances, selectedProject, projectList } from "../store";
  import { slide } from "svelte/transition";

  const fetchProjects: (notify?: boolean) => void = getContext("FetchProjects");

  onMount(() => {
    fetchProjects();
  });

  async function onItemClick(item: ProjectItem) {
    console.log("Switching to project", $state.snapshot(item.jsonData));
    selectedProject.set(item);
  }

  async function onAddProject() {
    try {
      const res = await helper_createProject();
      if (res.status === "error") throw { message: res.message };
      selectedProject.set(res);
      fetchProjects();
      toaster.success({ title: "Project created successfully." });
    } catch (error: any) {
      console.log("onAddProject", error);
      toaster.error({ title: "Failed to create project. " + error.message });
    }
  }

  async function onDeleteProject() {
    try {
      if (!$selectedProject) return;
      const res = await helper_deleteProject($selectedProject);
      if (res.status === "success") {
        fetchProjects();
        selectedProject.set(null);
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
                console.log(
                  "Failed to import project",
                  config.project_id,
                  res.message,
                );
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
      // openImportState = true;
    }
  }

  $effect(() => {
    if ($projectList)
      console.log(
        `CentralProjects.svelte - nof projects available: ${$projectList.length}`,
      );
  });
</script>

<div class="h-full">
  <div class="h-full flex">
    <div class="h-full max-w-xs flex flex-col space-y-1">
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
          disabled={!$selectedProject ||
            !!mapProjectToInstance($selectedProject, $instances)}
          onclick={onDeleteProject}
        >
          <icons.Trash2 />
        </button>
      </div>
      <ul
        class="w-full h-full p-1 shadow overflow-y-auto border border-surface-200-800 rounded min-w-64"
      >
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
        {#each $projectList as item (item.jsonData.source.project_id)}
          <li
            class="hover:bg-surface-200-800 px-2 flex items-center space-x-2 border-b border-surface-200-800"
            transition:slide
          >
            <button
              type="button"
              class="btn p-1 w-full flex items-center space-x-2 justify-start text-sm
              {item.jsonData.source.project_id ===
              $selectedProject?.jsonData.source.project_id
                ? 'font-bold'
                : ''}
              "
              onclick={() => onItemClick(item)}
            >
              {#if mapProjectToInstance(item, $instances)}
                <span
                  class="font-monospace text-xs bg-success-300-700 rounded px-2 w-8 font-bold"
                  >on</span
                >
              {:else}
                <span
                  class="font-monospace text-xs bg-surface-100-900 rounded text-surface-800-200 px-2 w-8"
                  >off</span
                >
              {/if}
              <span class="">{item.jsonData.source.project_title}</span>
              {#if item.jsonData.source.project_id === $selectedProject?.jsonData.source.project_id}
                <icons.Check size={16} class="ml-auto text-primary-500" />
              {/if}
            </button>
          </li>
        {/each}
      </ul>
    </div>
    <div class="grow h-full pl-4">
      <ProjectPanel />
    </div>
  </div>
</div>
