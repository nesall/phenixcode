<script lang="ts">
  import { monitorStore } from "../../../stores/monitor.svelte";
  import * as icons from "@lucide/svelte";

  const cost = $derived(monitorStore.snapshot?.cost);
  const summary = $derived(cost?.summary);
  const runningTotal = $derived(
    cost ? cost.generation_input + cost.generation_output + cost.generation_cached_input : 0,
  );
  const monthlyRows = $derived([...(cost?.monthly ?? [])].reverse());
  const maxMonthlyTotal = $derived(monthlyRows.reduce((max, row) => Math.max(max, row.total), 0));

  const providersByTotal = $derived(Object.entries(cost?.by_provider ?? {}).sort(([, a], [, b]) => b.total - a.total));

  function usd(amount: number | undefined): string {
    return `$${(amount ?? 0).toFixed(6)}`;
  }

  function usdCompact(amount: number | undefined): string {
    const val = amount ?? 0;
    return `$${val.toFixed(Math.abs(val) >= 1 ? 2 : 4)}`;
  }

  function onDownload() {
    if (!cost) return;
    const blob = new Blob([JSON.stringify(cost, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `cost_${new Date().toISOString().slice(0, 10)}.json`;
    a.click();
    URL.revokeObjectURL(url);
  }
</script>

<section class="h-full overflow-auto p-4 space-y-4">
  <header class="flex flex-wrap items-center justify-between gap-2">
    <div class="flex items-center gap-4">
      <h2 class="text-lg font-semibold">Generation cost</h2>
      {#if cost?.pricing_version}
        <span class="text-xs text-surface-500">[Pricing version: {cost.pricing_version}]</span>
      {/if}
    </div>
    <button class="btn input btn-icon preset-outlined" onclick={onDownload} title="Download cost data as JSON">
      <icons.Download />
    </button>
  </header>
  <div class="rounded-md border border-surface-200-800 p-4 space-y-4">
    {#if cost}
      <div class="flex flex-wrap items-end justify-between gap-3">
        <div>
          <div class="text-sm text-surface-500">Running total</div>
          <div class="text-3xl font-bold tabular-nums">
            {usd(runningTotal)}
          </div>
        </div>
        <span class="badge preset-outlined">Estimate</span>
      </div>
      <p class="text-sm text-surface-500">
        Estimate based on the configured generation-provider pricing. Embedding is not priced and is excluded from this
        total.
        {#if cost.note}
          {cost.note}{/if}
      </p>
      <div class="grid gap-3 sm:grid-cols-3">
        <div class="rounded bg-surface-100-900 p-3">
          <div class="text-xs text-surface-500">Input</div>
          <div class="font-semibold tabular-nums">{usd(cost.generation_input)}</div>
        </div>
        <div class="rounded bg-surface-100-900 p-3">
          <div class="text-xs text-surface-500">Cached input</div>
          <div class="font-semibold tabular-nums">{usd(cost.generation_cached_input)}</div>
        </div>
        <div class="rounded bg-surface-100-900 p-3">
          <div class="text-xs text-surface-500">Output</div>
          <div class="font-semibold tabular-nums">{usd(cost.generation_output)}</div>
        </div>
      </div>

      {#if summary}
        <div class="space-y-2 pt-2 border-t border-surface-200-800">
          <h3 class="text-sm font-semibold">By period</h3>
          <div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
            <div class="rounded bg-surface-100-900 p-3">
              <div class="text-xs text-surface-500">Today</div>
              <div class="font-semibold tabular-nums">{usdCompact(summary.today)}</div>
            </div>
            <div class="rounded bg-surface-100-900 p-3">
              <div class="text-xs text-surface-500">This month</div>
              <div class="font-semibold tabular-nums">{usdCompact(summary.this_month)}</div>
              <div class="text-xs text-surface-500 tabular-nums">Projected: {usdCompact(summary.projected_month)}</div>
            </div>
            <div class="rounded bg-surface-100-900 p-3">
              <div class="text-xs text-surface-500">Avg monthly</div>
              <div class="font-semibold tabular-nums">{usdCompact(summary.avg_monthly)}</div>
              <div class="text-xs text-surface-500">across {summary.months_tracked} mo</div>
            </div>
            <div class="rounded bg-surface-100-900 p-3">
              <div class="text-xs text-surface-500">Year to date</div>
              <div class="font-semibold tabular-nums">{usdCompact(summary.ytd)}</div>
            </div>
            <div class="rounded bg-surface-100-900 p-3">
              <div class="text-xs text-surface-500">Last 7 days</div>
              <div class="font-semibold tabular-nums">{usdCompact(summary.last_7d)}</div>
            </div>
            <div class="rounded bg-surface-100-900 p-3">
              <div class="text-xs text-surface-500">Last 30 days</div>
              <div class="font-semibold tabular-nums">{usdCompact(summary.last_30d)}</div>
              <div class="text-xs text-surface-500 tabular-nums">Avg/day: {usdCompact(summary.avg_daily_30d)}</div>
            </div>
          </div>
          <p class="text-xs text-surface-500">
            {#if summary.history_since}
              Period stats use local time and cover generation usage tracked since {summary.history_since}.
            {:else}
              No period data recorded yet.
            {/if}
            {#if runningTotal > summary.tracked_total + 1e-9}
              {" "}Earlier spend appears only in the running total.
            {/if}
          </p>
        </div>
      {/if}

      {#if monthlyRows.length}
        <div class="space-y-2 pt-2 border-t border-surface-200-800">
          <h3 class="text-sm font-semibold">Monthly</h3>
          <div class="overflow-x-auto rounded-md border border-surface-200-800">
            <table class="table table-compact w-full text-left">
              <thead>
                <tr>
                  <th>Month</th>
                  <th class="hidden md:table-cell">Share</th>
                  <th class="text-right">Calls</th>
                  <th class="text-right">Tokens in / out</th>
                  <th class="text-right hidden lg:table-cell">Input</th>
                  <th class="text-right hidden lg:table-cell">Cached</th>
                  <th class="text-right hidden lg:table-cell">Output</th>
                  <th class="text-right">Total</th>
                  <th class="text-right hidden md:table-cell">Avg / call</th>
                </tr>
              </thead>
              <tbody>
                {#each monthlyRows as row (row.month)}
                  <tr class="hover:bg-surface-100-900">
                    <td class="whitespace-nowrap tabular-nums">{row.month}</td>
                    <td class="hidden md:table-cell">
                      <div class="h-2 w-24 rounded bg-surface-200-800 overflow-hidden">
                        <div
                          class="h-full bg-primary-500"
                          style="width: {maxMonthlyTotal > 0 ? (row.total / maxMonthlyTotal) * 100 : 0}%"
                        ></div>
                      </div>
                    </td>
                    <td class="text-right tabular-nums">{row.calls.toLocaleString()}</td>
                    <td class="text-right tabular-nums whitespace-nowrap">
                      {row.tokens_in.toLocaleString()} / {row.tokens_out.toLocaleString()}
                    </td>
                    <td class="text-right tabular-nums hidden lg:table-cell">{usd(row.input)}</td>
                    <td class="text-right tabular-nums hidden lg:table-cell">{usd(row.cached_input)}</td>
                    <td class="text-right tabular-nums hidden lg:table-cell">{usd(row.output)}</td>
                    <td class="text-right tabular-nums">{usd(row.total)}</td>
                    <td class="text-right tabular-nums hidden md:table-cell">{row.calls > 0 ? usd(row.total / row.calls) : "—"}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
        </div>
      {/if}

      {#if cost.by_provider && Object.keys(cost.by_provider).length}
        <div class="overflow-x-auto rounded-md border border-surface-200-800">
          <table class="table table-compact w-full text-left">
            <thead>
              <tr>
                <th>Provider</th>
                <th class="hidden md:table-cell">Model</th>
                <th class="text-right">Tokens in / out</th>
                <th class="text-right hidden lg:table-cell">Input</th>
                <th class="text-right hidden lg:table-cell">Cached input</th>
                <th class="text-right hidden lg:table-cell">Output</th>
                <th class="text-right hidden md:table-cell">Total</th>
                <th class="text-right">Calls</th>
                <th class="text-right hidden md:table-cell">Avg / call</th>
              </tr>
            </thead>
            <tbody>
              {#each providersByTotal as [providerId, provider]}
                <tr class="hover:bg-surface-100-900">
                  <td>{providerId}</td>
                  <td class="hidden md:table-cell">{provider.model || "—"}</td>
                  <td class="text-right tabular-nums whitespace-nowrap">
                    {provider.tokens_in.toLocaleString()} / {provider.tokens_out.toLocaleString()}
                  </td>
                  <td class="text-right tabular-nums hidden lg:table-cell">{usd(provider.input)}</td>
                  <td class="text-right tabular-nums hidden lg:table-cell">{usd(provider.cached_input)}</td>
                  <td class="text-right tabular-nums hidden lg:table-cell">{usd(provider.output)}</td>
                  <td class="text-right tabular-nums hidden md:table-cell">{usd(provider.total)}</td>
                  <td class="text-right tabular-nums">{provider.calls}</td>
                  <td class="text-right tabular-nums hidden md:table-cell">{provider.calls > 0 ? usd(provider.total / provider.calls) : "—"}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    {:else}
      <div class="text-surface-500">
        {monitorStore.error || (monitorStore.loading ? "Loading monitor snapshot…" : "No cost data available.")}
      </div>
    {/if}
  </div>
</section>
