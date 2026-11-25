<script lang="ts">
  import * as icons from "@lucide/svelte";
  import DOMPurify from "dompurify";
  import { tick } from "svelte";
  import ToolCallRow from "./ToolCallRow.svelte";

  let { events, reasoning, active, progress }: {
    events: ChatMetaInfo[]; reasoning: string; active: boolean; progress: string;
  } = $props();
  const calls = $derived(events.filter(event => event.tool));
  const details = $derived(events.filter(event => !event.tool));
  const failures = $derived(calls.filter(call => call.status === "error").length);
  const interrupted = $derived(calls.some(call => call.interrupted));
  const timed = $derived(calls.length > 0 && calls.every(call => call.duration_ms !== undefined));
  const elapsed = $derived(calls.reduce((sum, call) => sum + (call.duration_ms ?? 0), 0));
  let reasoningPane = $state<HTMLDivElement>();
  let reasoningOpen = $state(false);
  let followReasoning = true;

  function trackScroll() {
    if (reasoningPane) followReasoning = reasoningPane.scrollHeight - reasoningPane.scrollTop - reasoningPane.clientHeight < 24;
  }
  $effect(() => {
    if (active && reasoningOpen && reasoning && reasoningPane) {
      void tick().then(() => {
        if (followReasoning && reasoningPane) reasoningPane.scrollTop = reasoningPane.scrollHeight;
      });
    } else followReasoning = true;
  });
</script>

{#if active || reasoning || events.length}
  <div class="min-w-0 space-y-1 text-left text-xs">
    {#if reasoning}
      <details bind:open={reasoningOpen}>
        <summary class="cursor-pointer py-1 text-surface-500">Thought process</summary>
        <div bind:this={reasoningPane} onscroll={trackScroll} role="region" aria-label="Reasoning content"
          class="reasoning-content max-h-36 overflow-y-auto overscroll-y-contain pl-3">
          {#if reasoningOpen}{@html DOMPurify.sanitize(reasoning)}{/if}
        </div>
      </details>
    {/if}
    {#if calls.length}
      <details>
        <summary class="cursor-pointer py-1 font-medium">
          {#if active}<icons.LoaderCircle size={12} class="inline animate-spin" /> Working… · {/if}
          {calls.length} tool {calls.length === 1 ? "call" : "calls"}{#if !active && timed}&nbsp;·&nbsp;{elapsed < 1000 ? elapsed + " ms" : (elapsed / 1000).toFixed(1) + " s"} in tools{/if}
          {#if failures}<span class="text-error-500"> · {failures} failed</span>{/if}
          {#if interrupted}<span class="text-warning-500"> · Interrupted</span>{/if}
        </summary>
        <div role="region" aria-label="Tool calls">
          {#each calls as call, index (call.event_id ?? index)}
            <ToolCallRow info={call} />
          {/each}
        </div>
      </details>
    {:else if active}
      <p role="status" class="text-surface-500"><icons.LoaderCircle size={12} class="inline animate-spin" /> {progress || "Working…"}</p>
    {/if}
    {#if details.length}
      <details>
        <summary class="cursor-pointer py-1 text-surface-500">
          Response details{#if details.some(event => event.status === "error")}<span class="text-error-500"> · Error</span>{/if}
        </summary>
        <!-- svelte-ignore a11y_no_noninteractive_tabindex (Keyboard scroll region.) -->
        <div class="meta-list flex flex-col gap-1 overflow-y-auto overscroll-y-contain pl-3" role="region" aria-label="Response activity" tabindex="0">
          {#each details as info, index (info.event_id ?? index)}
            <div data-meta-id={info.event_id} data-meta-status={info.status} title={info.content}>
              {#if info.status === "error"}<icons.CircleX size={12} class="inline text-error-500" aria-label="Failed" />
              {:else if info.status === "warning"}<icons.TriangleAlert size={12} class="inline text-warning-500" aria-label="Warning" />
              {/if}
              {info.summary ?? info.content}
            </div>
          {/each}
        </div>
      </details>
    {/if}
  </div>
{/if}

<style>
  .meta-list {
    max-height: min(18rem, 40vh);
    scrollbar-gutter: stable;
    overflow-wrap: anywhere;
  }

  .reasoning-content {
    overflow-wrap: anywhere;
    overflow-x: hidden;
  }

  .reasoning-content :global(pre),
  .reasoning-content :global(code) {
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }

  .reasoning-content :global(.overflow-x-auto) {
    overflow-x: hidden;
  }

  .reasoning-content :global(table) {
    table-layout: fixed;
    width: 100%;
  }
</style>
