<script lang="ts">
  import { monitorStore } from "../../../stores/monitor.svelte";
  import * as icons from "@lucide/svelte";

  const metrics = $derived(monitorStore.snapshot?.metrics);
  const latencyP50 = $derived(metrics && metrics.latency_samples > 0 ? `${metrics.latency_p50_ms.toFixed(1)} ms` : "—");
  const latencyP95 = $derived(metrics && metrics.latency_samples > 0 ? `${metrics.latency_p95_ms.toFixed(1)} ms` : "—");

  function number(value: number | undefined): string {
    return value === undefined ? "—" : value.toLocaleString();
  }
  function onDownload() {
    if (!metrics) return;
    const blob = new Blob([JSON.stringify(metrics, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `metrics_${new Date().toISOString().slice(0, 10)}.json`;
    a.click();
    URL.revokeObjectURL(url);
  }
</script>

<section class="h-full overflow-auto p-4 space-y-4">
  <header class="flex flex-wrap items-center justify-between gap-2">
    <div class="flex items-center gap-4">
      <h2 class="text-lg font-semibold">Request metrics</h2>
    </div>
    <button class="btn input btn-icon preset-outlined" onclick={onDownload} title="Download metrics data as JSON">
      <icons.Download />
    </button>
  </header>
  {#if !metrics}
    <div class="rounded border border-surface-200-800 p-4 text-surface-500">
      {monitorStore.error || (monitorStore.loading ? "Loading monitor snapshot…" : "No metrics available.")}
    </div>
  {:else}
    <div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Requests</div>
        <div class="mt-1 text-2xl font-semibold">{number(metrics.requests)}</div>
      </div>
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Errors</div>
        <div class="mt-1 text-2xl font-semibold">{number(metrics.errors)}</div>
      </div>
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Latency p50</div>
        <div class="mt-1 text-2xl font-semibold">{latencyP50}</div>
      </div>
      <div class="rounded-md border border-surface-200-800 p-4">
        <div class="text-sm text-surface-500">Latency p95</div>
        <div class="mt-1 text-2xl font-semibold">{latencyP95}</div>
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
        <table class="table table-compact w-full text-left">
          <thead>
            <tr>
              <th scope="col">Endpoint</th>
              <th scope="col" class="text-right">Requests</th>
              <th scope="col" class="text-right">Errors</th>
              <th scope="col" class="text-right">p50</th>
              <th scope="col" class="text-right">p95</th>
              <th scope="col" class="text-right">Tokens in / out</th>
            </tr>
          </thead>
          <tbody>
            {#each Object.entries(metrics.by_endpoint) as [name, endpoint]}
              <tr class="hover:bg-surface-100-900">
                <td>{name}</td>
                <td class="text-right tabular-nums">{number(endpoint.requests)}</td>
                <td class="text-right tabular-nums">{number(endpoint.errors)}</td>
                <td class="text-right tabular-nums">{endpoint.latency_samples ? `${endpoint.latency_p50_ms.toFixed(1)} ms` : "-"}</td>
                <td class="text-right tabular-nums">{endpoint.latency_samples ? `${endpoint.latency_p95_ms.toFixed(1)} ms` : "-"}</td>
                <td class="text-right tabular-nums">{number(endpoint.tokens_in)} / {number(endpoint.tokens_out)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
    <section class="space-y-2" aria-labelledby="tool-usage-heading">
      <h3 class="text-lg font-semibold" id="tool-usage-heading">Tool usage</h3>
      {#if metrics.tool_usage?.length}
        <div class="overflow-x-auto rounded-md border border-surface-200-800">
          <table class="table table-compact w-full text-left" aria-labelledby="tool-usage-heading">
            <thead>
              <tr>
                <th scope="col">Tool</th>
                <th scope="col">Caller</th>
                <th scope="col" class="text-right">Calls</th>
                <th scope="col" class="text-right">Errors</th>
                <th scope="col">Last used</th>
              </tr>
            </thead>
            <tbody>
              {#each metrics.tool_usage as usage (`${usage.tool}:${usage.caller}`)}
                <tr class="hover:bg-surface-100-900">
                  <td>{usage.tool}</td>
                  <td>{usage.caller === "chat" ? "Chat" : "MCP"}</td>
                  <td class="text-right tabular-nums">{number(usage.calls)}</td>
                  <td class="text-right tabular-nums">{number(usage.errors)}</td>
                  <td class="whitespace-nowrap">{new Date(usage.last_used * 1000).toLocaleString()}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {:else}
        <p class="text-sm text-surface-500">
          {metrics.tool_usage ? "No tool calls recorded." : "Tool usage metrics unavailable."}
        </p>
      {/if}
    </section>
  {/if}
</section>
