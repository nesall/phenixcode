<script lang="ts">
  import { tick } from "svelte";
  import * as icons from "@lucide/svelte";
  import { monitorStore } from "../../../stores/monitor.svelte";

  let level = $state("all");
  let autoScroll = $state(true);
  let logContainer: HTMLDivElement;
  let previousLevel = $state("all");

  const levelPatterns: Record<string, RegExp> = {
    debug: /\[DEBUG\]/i,
    info: /\[INFO \]/i,
    warn: /\[WARN \]/i,
    error: /\[ERROR\]/i,
  };

  const visibleLines = $derived(
    level === "all" ? monitorStore.logLines : monitorStore.logLines.filter((line) => levelPatterns[level]?.test(line)),
  );

  $effect(() => {
    monitorStore.logLines.length;
    if (autoScroll)
      void tick().then(() => {
        if (logContainer) logContainer.scrollTop = logContainer.scrollHeight;
      });
  });

  $effect(() => {
    const currentLevel = level;
    const filterChanged = currentLevel !== previousLevel;
    previousLevel = currentLevel;

    if (autoScroll || filterChanged) {
      void tick().then(() => {
        if (!logContainer) return;

        if (autoScroll) {
          logContainer.scrollTop = logContainer.scrollHeight;
        }
      });
    }
  });

  function updateAutoScroll(event: Event) {
    const element = event.currentTarget as HTMLDivElement;
    autoScroll = element.scrollHeight - element.scrollTop - element.clientHeight < 32;
  }
</script>

<section class="h-full min-h-0 overflow-hidden p-4 flex flex-col gap-3">
  <header class="flex flex-wrap items-center gap-3">
    <h2 class="text-lg font-semibold">Logs</h2>
    <label class="flex items-center gap-2 text-sm">
      <span>Level</span>
      <select class="select select-sm" bind:value={level}>
        <option value="all">All levels</option>
        <option value="debug">Debug</option>
        <option value="info">Info</option>
        <option value="warn">Warn</option>
        <option value="error">Error</option>
      </select>
    </label>
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
        >
          {line}
        </div>
      {/each}
    {:else}
      <div class="text-surface-500">{monitorStore.logError || "No log lines available."}</div>
    {/if}
  </div>
</section>
