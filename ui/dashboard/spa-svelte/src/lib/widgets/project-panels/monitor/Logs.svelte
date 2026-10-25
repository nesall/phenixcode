<script lang="ts">
  import { tick } from "svelte";
  import * as icons from "@lucide/svelte";
  import { monitorStore } from "../../../stores/monitor.svelte";

  let selectedLevels = $state(["debug", "info", "warn", "error"]);
  let autoScroll = $state(true);
  let logContainer: HTMLDivElement;
  let previousFilters = "";

  const logLevels = [
    { value: "debug", label: "Debug" },
    { value: "info", label: "Info" },
    { value: "warn", label: "Warn" },
    { value: "error", label: "Error" },
  ];

  const levelPatterns: Record<string, RegExp> = {
    debug: /\[DEBUG\]/i,
    info: /\[INFO \]/i,
    warn: /\[WARN \]/i,
    error: /\[ERROR\]/i,
  };

  const visibleLines = $derived(
    selectedLevels.length === logLevels.length
      ? monitorStore.logLines
      : monitorStore.logLines.filter((line) =>
          selectedLevels.some((level) => levelPatterns[level]?.test(line)),
        ),
  );

  $effect(() => {
    monitorStore.logLines.length;
    if (autoScroll)
      void tick().then(() => {
        if (logContainer) logContainer.scrollTop = logContainer.scrollHeight;
      });
  });

  $effect(() => {
    const currentFilters = selectedLevels.join(",");
    const filterChanged = currentFilters !== previousFilters;
    previousFilters = currentFilters;

    if (autoScroll || filterChanged) {
      void tick().then(() => {
        if (!logContainer) return;

        if (autoScroll) {
          logContainer.scrollTop = logContainer.scrollHeight;
        }
      });
    }
  });

  function updateAllLevels(event: Event) {
    const checked = (event.currentTarget as HTMLInputElement).checked;
    selectedLevels = checked ? logLevels.map(({ value }) => value) : [];
  }

  function updateAutoScroll(event: Event) {
    const element = event.currentTarget as HTMLDivElement;
    autoScroll = element.scrollHeight - element.scrollTop - element.clientHeight < 32;
  }
</script>

<section class="h-full min-h-0 overflow-hidden p-4 flex flex-col gap-3">
  <header class="flex flex-wrap items-center gap-3">
    <h2 class="text-lg font-semibold">Logs</h2>
    <fieldset class="flex flex-wrap items-center gap-3">
      <legend class="sr-only">Log level filters</legend>
      <label class="flex items-center gap-2 text-sm">
        <input
          type="checkbox"
          class="checkbox checkbox-sm"
          checked={selectedLevels.length === logLevels.length}
          onchange={updateAllLevels}
        />
        All levels
      </label>
      {#each logLevels as logLevel (logLevel.value)}
        <label class="flex items-center gap-2 text-sm">
          <input
            type="checkbox"
            class="checkbox checkbox-sm"
            bind:group={selectedLevels}
            value={logLevel.value}
          />
          {logLevel.label}
        </label>
      {/each}
    </fieldset>
    <label class="ml-auto flex items-center gap-2 text-sm">
      <input type="checkbox" class="checkbox checkbox-sm" bind:checked={autoScroll} />
      Auto-scroll
    </label>
  </header>
  {#if monitorStore.logError}
    <div class="flex items-center gap-2 rounded border border-error-500 p-2 text-sm text-error-700-300">
      <icons.TriangleAlert size={16} />
      {monitorStore.logError}
    </div>
  {/if}
  <div
    class="min-h-0 flex-1 overflow-auto rounded-md border border-surface-200-800 bg-surface-950 p-3 font-mono text-xs"
    bind:this={logContainer}
    onscroll={updateAutoScroll}
  >
    {#if visibleLines.length}
      {#each visibleLines as line, index}
        <div
          class="whitespace-pre-wrap break-all leading-5 text-left"
          class:text-error-400={/\b(error|fatal)\b/i.test(line)}
          class:text-warning-400={/\b(warn|warning)\b/i.test(line)}
          class:text-surface-500={/\b(debug)\b/i.test(line)}
          class:text-surface-300={!/\b(error|fatal|warn|warning|debug)\b/i.test(line)}
        >
          {line}
        </div>
      {/each}
    {:else}
      <div class="text-surface-500">{monitorStore.logError || "No log lines available."}</div>
    {/if}
  </div>
</section>
