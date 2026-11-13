<script lang="ts">
  import { Toast } from "@skeletonlabs/skeleton-svelte";
  import CentralWidget from "./lib/widgets/CentralWidget.svelte";
  import Statusbar from "./lib/widgets/Statusbar.svelte";
  import { helper_getInstances, helper_getProjectList, helper_getProviders, toaster } from "./lib/utils";
  import { projectStore, setAgentEnabled, setInstances } from "./lib/store.svelte";
  import { onMount, setContext } from "svelte";
    import { setEmbeddingProviders, setGenerationProviders } from "./lib/store.svelte";

  async function fetchProviders() {
    try {
      const res = await helper_getProviders();
      console.log("fetchProviders", res);
      if (res.status === "success") {
        setGenerationProviders(res.providers.generation_providers || []);
        setEmbeddingProviders(res.providers.embedding_providers || []);
        setAgentEnabled(!!res.providers.agent_enabled);
      } else {
        toaster.error({ title: "Unable to fetch providers", description: res.message });
      }
    } catch (err: any) {
      console.log("Error fetching providers", err);
      setGenerationProviders([]);
      setEmbeddingProviders([]);
    }
  }

  async function fetchInstances() {
    try {
      const res = await helper_getInstances();
      console.log("fetchInstances", res);
      if (res.status === "success") {
        setInstances(res.instances);
      } else {
        toaster.error({ title: "Unable to fetch instances", description: res.message });
      }
    } catch (err: any) {
      console.log("Error fetching instances", err);
      setInstances([]);
    }
  }

  async function fetchProjects(notify?: boolean) {
    try {
      const res = await helper_getProjectList();
      console.log("fetchProjects", res);
      if (res.status === "success") {
        projectStore.list = res.projects;
        if (notify) toaster.success({ title: `${projectStore.list.length} project(s) fetched` });
      } else {
        toaster.error({ title: "Unable to fetch projects", description: res.message });
      }
    } catch (err: any) {
      console.log("Error fetching projects", err);
      projectStore.list = [];
    }
  }

  setContext("FetchProviders", fetchProviders);
  setContext("FetchInstances", fetchInstances);
  setContext("FetchProjects", fetchProjects);

  let intervalId: number;
  onMount(() => {
    intervalId = setInterval(fetchInstances, 20000);
    fetchInstances();
    fetchProviders();
    return () => clearInterval(intervalId);
  });
</script>

<main
  class="mx-auto flex flex-col space-y-2 items-center justify-center
    w-screen h-screen min-w-sx min-h-sx max-h-screen"
>
  <div class="p-4 m-0 flex flex-col w-full h-full gap-2">
    <!-- <Toolbar /> -->
    <div class="grow w-full h-0 -mb-2">
      <!-- Main content goes here -->
      <CentralWidget />
    </div>
  </div>
  <div class="w-full">
    <Statusbar />
  </div>
</main>

<Toast.Group {toaster}>
  {#snippet children(toast)}
    <Toast {toast} class="w-auto max-w-md">
      <Toast.Message>
        <Toast.Title>{toast.title}</Toast.Title>
        <Toast.Description>{toast.description}</Toast.Description>
      </Toast.Message>
      <Toast.CloseTrigger />
    </Toast>
  {/snippet}
</Toast.Group>
