<script lang="ts">
  import { monitorStore } from "../../../stores/monitor.svelte";

  const metrics = $derived(monitorStore.snapshot?.metrics);
  const latencyP50 = $derived(metrics && metrics.latency_samples > 0 ? `${metrics.latency_p50_ms.toFixed(1)} ms` : "—");
  const latencyP95 = $derived(metrics && metrics.latency_samples > 0 ? `${metrics.latency_p95_ms.toFixed(1)} ms` : "—");

  function number(value: number | undefined): string {
    return value === undefined ? "—" : value.toLocaleString();
  }
</script>

<section class="h-full overflow-auto p-4 space-y-4">
  <h2 class="text-lg font-semibold">Request metrics</h2>
  {#if !metrics}
    <div class="rounded border border-surface-200-800 p-4 text-surface-500">
      {monitorStore.error || (monitorStore.loading ? "Loading monitor snapshot…" : "No metrics available.")}
    </div>
  {:else}
    <div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Requests</div><div class="mt-1 text-2xl font-semibold">{number(metrics.requests)}</div>
      </div>
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Errors</div><div class="mt-1 text-2xl font-semibold">{number(metrics.errors)}</div>
      </div>
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Latency p50</div><div class="mt-1 text-2xl font-semibold">{latencyP50}</div>
      </div>
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Latency p95</div><div class="mt-1 text-2xl font-semibold">{latencyP95}</div>
      </div>
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Tokens in / out</div>
        <div class="mt-1 text-2xl font-semibold">{number(metrics.tokens_in)} / {number(metrics.tokens_out)}</div>
      </div>
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Chunks completed / total</div>
        <div class="mt-1 text-2xl font-semibold">
          {number(monitorStore.snapshot?.progress.chunks_done)} / {number(monitorStore.snapshot?.progress.chunks_total)}
        </div>
      </div>
    </div>
    {#if metrics.by_endpoint && Object.keys(metrics.by_endpoint).length}
      <div class="overflow-x-auto rounded-md border border-surface-200-800">
        <table class="table table-compact w-full">
          <thead><tr><th>Endpoint</th><th>Requests</th><th>Errors</th><th>p50</th><th>p95</th><th>Tokens in / out</th></tr></thead>
          <tbody>
            {#each Object.entries(metrics.by_endpoint) as [name, endpoint]}
              <tr>
                <td>{name}</td><td>{number(endpoint.requests)}</td><td>{number(endpoint.errors)}</td>
                <td>{endpoint.latency_samples ? `${endpoint.latency_p50_ms.toFixed(1)} ms` : "—"}</td>
                <td>{endpoint.latency_samples ? `${endpoint.latency_p95_ms.toFixed(1)} ms` : "—"}</td>
                <td>{number(endpoint.tokens_in)} / {number(endpoint.tokens_out)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  {/if}
</section>
