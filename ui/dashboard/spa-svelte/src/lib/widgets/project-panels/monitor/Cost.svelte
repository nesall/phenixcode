<script lang="ts">
  import { monitorStore } from "../../../stores/monitor.svelte";

  const cost = $derived(monitorStore.snapshot?.cost);

  function usd(amount: number | undefined): string {
    return `$${(amount ?? 0).toFixed(6)}`;
  }
</script>

<section class="h-full overflow-auto p-4 space-y-4">
  <header class="flex flex-wrap items-center justify-between gap-2">
    <h2 class="text-lg font-semibold">Generation cost</h2>
    {#if cost?.pricing_version}
      <span class="text-xs text-surface-500">Pricing version: {cost.pricing_version}</span>
    {/if}
  </header>
  <div class="rounded-md border border-surface-200-800 p-4 space-y-4">
    {#if cost}
      <div class="flex flex-wrap items-end justify-between gap-3">
        <div>
          <div class="text-sm text-surface-500">Running total</div>
          <div class="text-3xl font-bold tabular-nums">{usd(cost.generation_input + cost.generation_output + cost.generation_cached_input)}</div>
        </div>
        <span class="badge preset-outlined">Estimate</span>
      </div>
      <p class="text-sm text-surface-500">
        Estimate based on the configured generation-provider pricing. Embedding is not priced and is excluded from this total.
        {#if cost.note} {cost.note}{/if}
      </p>
      <div class="grid gap-3 sm:grid-cols-3">
        <div class="rounded bg-surface-100-900 p-3"><div class="text-xs text-surface-500">Input</div><div class="font-semibold">{usd(cost.generation_input)}</div></div>
        <div class="rounded bg-surface-100-900 p-3"><div class="text-xs text-surface-500">Cached input</div><div class="font-semibold">{usd(cost.generation_cached_input)}</div></div>
        <div class="rounded bg-surface-100-900 p-3"><div class="text-xs text-surface-500">Output</div><div class="font-semibold">{usd(cost.generation_output)}</div></div>
      </div>
      {#if cost.by_provider && Object.keys(cost.by_provider).length}
        <div class="overflow-x-auto rounded-md border border-surface-200-800">
          <table class="table table-compact w-full">
            <thead><tr><th>Provider</th><th>Model</th><th>Input</th><th>Cached input</th><th>Output</th><th>Total</th></tr></thead>
            <tbody>
              {#each Object.entries(cost.by_provider) as [providerId, provider]}
                <tr>
                  <td>{providerId}</td><td>{provider.model || "—"}</td>
                  <td>{usd(provider.input)}</td><td>{usd(provider.cached_input)}</td><td>{usd(provider.output)}</td><td>{usd(provider.total)}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    {:else}
      <div class="text-surface-500">{monitorStore.error || (monitorStore.loading ? "Loading monitor snapshot…" : "No cost data available.")}</div>
    {/if}
  </div>
</section>
