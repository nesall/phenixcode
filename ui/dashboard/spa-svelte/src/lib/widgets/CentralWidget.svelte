<script lang="ts">
  import { Tabs } from "@skeletonlabs/skeleton-svelte";
  import CentralProjects from "./CentralProjects.svelte";
  import * as icons from "@lucide/svelte";
  import { Consts, getPersistentKey, setPersistentKey } from "../utils";
  import CentralInstances from "./CentralInstances.svelte";
  import CentralSettings from "./CentralSettings.svelte";
  import { onMount } from "svelte";
    import CentralProviders from "./CentralProviders.svelte";

  // interface Props {
  //   fetchInstances: () => void;
  // }
  // let { fetchInstances }: Props = $props();

  function setDarkOrLight(dl: string | null) {
    console.log("setDarkOrLight", dl);
    const htmlEl = document.documentElement;
    if (dl === "dark") {
      htmlEl.setAttribute("data-mode", "dark");
    } else {
      htmlEl.setAttribute("data-mode", "light");
    }
  }

  onMount(() => {
    getPersistentKey(Consts.DarkOrLightKey).then((dl) => {
      setDarkOrLight(dl);
    });
  });

  function onToggleDarkMode() {
    const htmlEl = document.documentElement;
    const newDl = htmlEl.getAttribute("data-mode") === "dark" ? "light" : "dark";
    setDarkOrLight(newDl);
    try {
      setPersistentKey(Consts.DarkOrLightKey, newDl);
    } catch (e) {
      console.log("Unable to access localStorage", e);
    }
  }
</script>

<Tabs defaultValue="projects" class="h-full">
  <Tabs.List>
    <Tabs.Trigger value="projects"><icons.Settings size={20} /> Projects</Tabs.Trigger>
    <Tabs.Trigger value="providers"><icons.Target size={20} /> Providers</Tabs.Trigger>
    <Tabs.Trigger value="instances"><icons.Activity size={20} /> Instances</Tabs.Trigger>
    <Tabs.Trigger value="activity"><icons.Settings2 size={20} /> Settings</Tabs.Trigger>
    <Tabs.Indicator />
    <div class="ml-auto flex items-center">
      <button
        type="button"
        class="btn btn-icon hover:preset-tonal h-full"
        aria-label="Dark/Light Mode"
        onclick={onToggleDarkMode}
      >
        <icons.SunMoon />
      </button>
    </div>
  </Tabs.List>
  <Tabs.Content value="projects" class="h-full">
    <CentralProjects />
  </Tabs.Content>
  <Tabs.Content value="providers" class="h-0 flex-grow">
    <CentralProviders />
  </Tabs.Content>
  <Tabs.Content value="instances" class="h-0 flex-grow">
    <CentralInstances />
  </Tabs.Content>
  <Tabs.Content value="activity" class="h-0 flex-grow">
    <CentralSettings />
  </Tabs.Content>
</Tabs>
