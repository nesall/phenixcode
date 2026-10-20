<script lang="ts">
  import * as icons from "@lucide/svelte";
  import { monitorStore } from "../../../stores/monitor.svelte";

  const snapshot = $derived(monitorStore.snapshot);
  const progress = $derived(snapshot?.progress);
  const chunkProgress = $derived(!!progress && progress.chunks_total > 0);
  const done = $derived(chunkProgress ? progress!.chunks_done : progress?.files_done ?? 0);
  const total = $derived(chunkProgress ? progress!.chunks_total : progress?.files_total ?? 0);
  const percent = $derived(total > 0 ? Math.min(100, Math.max(0, (done / total) * 100)) : 0);
  const unit = $derived(chunkProgress ? "chunks" : "files");
  const eta = $derived(progress && progress.eta_seconds >= 0 ? progress.eta_seconds : null);

  function formatEta(seconds: number): string {
    if (seconds < 60) return `${Math.ceil(seconds)}s`;
    const minutes = Math.floor(seconds / 60);
    const remainingSeconds = Math.ceil(seconds % 60);
    return remainingSeconds ? `${minutes}m ${remainingSeconds}s` : `${minutes}m`;
  }
</script>

<section class="h-full overflow-auto p-4 space-y-4">
  <header class="flex items-center gap-2">
    <h2 class="text-lg font-semibold">Embedding progress</h2>
    {#if snapshot}
      <span class="badge preset-outlined">{snapshot.state}</span>
    {/if}
    {#if !monitorStore.connected}
      <span class="ml-auto text-xs text-surface-500">Snapshot / offline view</span>
    {/if}
  </header>

  {#if monitorStore.error && !snapshot}
    <div class="rounded border border-error-500 p-4 text-error-700-300">{monitorStore.error}</div>
  {:else}
    <div class="rounded-md border border-surface-200-800 p-5 space-y-5">
      <div class="flex items-end justify-between">
        <div>
          <div class="text-sm text-surface-500">{unit} processed</div>
          <div class="text-2xl font-bold">{done} <span class="text-base font-normal">/ {total}</span></div>
        </div>
        <div class="text-3xl font-semibold tabular-nums">{percent.toFixed(1)}%</div>
      </div>
      <div class="h-3 overflow-hidden rounded-full bg-surface-200-800">
        <div class="h-full rounded-full bg-primary-500 transition-[width]" style={`width: ${percent}%`}></div>
      </div>
      <div class="grid gap-3 sm:grid-cols-3">
        <div class="rounded bg-surface-100-900 p-3">
          <div class="text-xs text-surface-500">Phase</div>
          <div class="mt-1 flex items-center gap-2 font-medium"><icons.Activity size={16} />{progress?.phase ?? "—"}</div>
        </div>
        <div class="rounded bg-surface-100-900 p-3">
          <div class="text-xs text-surface-500">Current batch</div>
          <div class="mt-1 font-medium">
            {progress ? `${progress.current_batch || "—"}${progress.current_batch_size ? ` (${progress.current_batch_size} files)` : ""}` : "—"}
          </div>
        </div>
        <div class="rounded bg-surface-100-900 p-3">
          <div class="text-xs text-surface-500">Estimated time remaining</div>
          <div class="mt-1 font-medium">{eta === null ? "—" : formatEta(eta)}</div>
        </div>
      </div>
      {#if monitorStore.loading}
        <div class="text-xs text-surface-500">Loading monitor snapshot…</div>
      {/if}
    </div>
  {/if}
</section>
