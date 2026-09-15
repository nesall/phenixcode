<script lang="ts">
  import { onMount } from "svelte";
  import { apiOptionsGroupedSorted, clog, isGoodArray, testConnection } from "../utils";
  import * as icons from "@lucide/svelte";
  import Dropdown from "./Dropdown.svelte";
  import { bApisGroupedByLabel, bApisSortedByPrice, contextSizeRatio, settings } from "../store";

  interface Props {
    fetchSettings: () => void;
    onConnectionStatusChange: (ok: boolean) => void;
  }
  let { fetchSettings, onConnectionStatusChange = (ok: boolean) => {} }: Props = $props();

  let connected = $state(false);
  let timerId: number | null = $state(null);

  let recheckTimerId: number | null = null;

  onMount(() => {
    tryConnecting();

    return () => {
      if (timerId) clearInterval(timerId);
      if (recheckTimerId) clearInterval(recheckTimerId);
    };
  });

  function tryConnecting() {
    if (timerId) {
      clearInterval(timerId);
    }
    let attempts = 0;
    timerId = setInterval(() => {
      attempts++;
      testConnection()
        .then((res) => {
          connected = res;
          onConnectionStatusChange(connected);
          if (connected) {
            clearInterval(timerId!);
            timerId = null;
            fetchSettings();
            recheckConnectionStatus();
          }
        })
        .finally(() => {
          if (5 <= attempts) {
            clearInterval(timerId!);
            timerId = null;
          }
        });
    }, 1500);
  }

  function recheckConnectionStatus() {
    if (recheckTimerId) {
      clearInterval(recheckTimerId);
    }
    recheckTimerId = setInterval(() => {
      testConnection()
        .then((ok) => {
          if (!ok) {
            clearInterval(recheckTimerId!);
            recheckTimerId = null;
            tryConnecting();
          }
        })
        .catch(() => {
          clearInterval(recheckTimerId!);
          recheckTimerId = null;
          tryConnecting();
        });
    }, 30000);
  }

  function onModelChange(i: number, modelId: string) {
    $settings.completionApis = $settings.completionApis.map((api) => ({
      ...api,
      current: api.id === modelId,
    }));
    clog("Statusbar.onModelChange", modelId);
    $settings.currentApi = modelId;
  }

  function onContextSizeChange(i: number, size: string) {
    clog("Statusbar.onContextSizeChange", size);
    const ratio = contextSizeMap[size] || 0.7;
    $contextSizeRatio = ratio;
  }

const apiOptions = $derived(
  apiOptionsGroupedSorted(
    $settings.completionApis.map((a) => ({
      value: a.id,
      label: a.model,
      hint: `${a.name}${a.model === "auto" ? "" : ` - ${a.model}`} (cost: ${
        typeof a.combinedPrice === "string"
          ? a.combinedPrice
          : a.combinedPrice.toFixed(2)
      })`,
      group: a.name,
      _price: a.combinedPrice,
    })),
    $bApisSortedByPrice,
    $bApisGroupedByLabel,
  ),
);

  const curApi = $derived(
    -1 != $settings.completionApis.findIndex((a) => a.current)
      ? $settings.completionApis[$settings.completionApis.findIndex((a) => a.current)].id
      : "",
  );

  const contextSizeMap: Record<string, number> = {
    compact: 0.5,
    balanced: 0.7,
    detailed: 0.9,
  };

  const contextSizeOptions = $derived([
    {
      value: "compact",
      label: "Compact",
      hint: `Faster, lower cost (${contextSizeMap["compact"] * 100}% of allotted context)`,
    },
    {
      value: "balanced",
      label: "Balanced",
      hint: `Good balance of context and cost (${contextSizeMap["balanced"] * 100}% of allotted context)`,
    },
    {
      value: "detailed",
      label: "Detailed",
      hint: `More context, higher cost (${contextSizeMap["detailed"] * 100}% of allotted context)`,
    },
  ]);

  let curContextSize = $state("balanced");

  $effect(() => {
    if ($settings) console.log("Statusbar $settings changed:", $state.snapshot($settings));
  });

  $effect(() => {
    if ($bApisGroupedByLabel)
      console.log("Statusbar $bApisGroupedByLabel changed:", $state.snapshot($bApisGroupedByLabel));
  });

  $effect(() => {
    if (apiOptions) console.log("Statusbar apiOptions changed:", $state.snapshot(apiOptions));
  });
</script>

<div class="flex space-x-2 items-center w-full bg-surface-100-900 px-2 py-1 text-xs">
  {#if timerId}
    <span>Checking connection...</span>
  {:else}
    <div class="flex items-center space-x-1">
      {#if connected}
        <span>Connected.</span>
      {:else}
        <span>Unable to connect to API server.</span>
      {/if}
      <button
        type="button"
        class="btn btn-sm btn-icon py-[1px] px-1 preset-tonal hover:preset-tonal-surface"
        title="Retry connecting to API server"
        onclick={() => tryConnecting()}
      >
        <icons.RefreshCcw size={16} />
      </button>
    </div>
  {/if}
  <div class="flex-1"></div>
  <div class="flex items-center space-x-1">
    <Dropdown
      values={contextSizeOptions}
      value={curContextSize}
      onChange={onContextSizeChange}
      classNames="py-[2px] min-w-[7rem] preset-tonal"
    />
    <Dropdown
      values={apiOptions}
      value={curApi}
      onChange={onModelChange}
      classNames="py-[2px] min-w-[10rem] preset-tonal"
      showGroups={$bApisGroupedByLabel}
      dropdownWidth="max-content"
    />
  </div>
</div>
