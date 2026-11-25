<script lang="ts">
  import * as icons from "@lucide/svelte";
  import { toaster } from "../utils";
  import ToolTraceJson from "./ToolTraceJson.svelte";

  let { info }: { info: ChatMetaInfo } = $props();
  const parameters = $derived(info.args && JSON.stringify(info.args, null, 2));
  const argsNotice = { missing: "No parameters supplied.", invalid: "Invalid parameters.", oversized: "Parameters exceed the 4 KiB display limit." };

  async function copy(text: string) {
    try {
      await navigator.clipboard.writeText(text);
      toaster.success({ title: "Copied to clipboard" });
    } catch {
      toaster.error({ title: "Unable to copy to clipboard" });
    }
  }
</script>

<details class="group/call min-w-0 border-l border-surface-300-700 pl-3" data-meta-id={info.event_id} data-meta-status={info.status}>
  <summary class="flex cursor-pointer list-none items-baseline gap-2 rounded py-1 text-left">
    <icons.ChevronRight size={12} class="shrink-0 self-start mt-1 group-open/call:rotate-90" />
    {#if info.status === "running"}
      <icons.LoaderCircle size={12} class="shrink-0 self-start mt-1 animate-spin" aria-label="Running" />
    {:else if info.status === "error"}
      <icons.CircleX size={12} class="shrink-0 self-start mt-1 text-error-500" aria-label="Failed" />
    {:else if info.status === "warning"}
      <icons.TriangleAlert size={12} class="shrink-0 self-start mt-1 text-warning-500" aria-label="Warning" />
    {:else if info.status === "success"}
      <icons.Check size={12} class="shrink-0 self-start mt-1" aria-label="Completed" />
    {:else}
      <icons.Info size={12} class="shrink-0 self-start mt-1" aria-label="Information" />
    {/if}
    <span class="min-w-0 flex-1 wrap-anywhere">
      <code class="font-semibold">{info.tool}</code>
      {info.action ?? info.summary ?? info.content}
      {#if info.outcome_summary}<span class="text-surface-500"> · {info.outcome_summary}</span>{/if}
    </span>
    {#if info.duration_ms !== undefined}<span class="shrink-0 tabular-nums text-surface-500">{info.duration_ms} ms</span>{/if}
  </summary>
  <!-- svelte-ignore a11y_no_noninteractive_tabindex (Keyboard scroll region.) -->
  <div class="tool-detail ml-5 space-y-2 overflow-y-auto overscroll-y-contain pb-2"
    role="region" aria-label={`${info.tool} details`} tabindex="0">
    {#if info.round || info.call_id}
      <p class="text-surface-500 wrap-anywhere">{info.round ? `Round ${info.round}` : ""}{info.round && info.call_id ? " · " : ""}{info.call_id ? `Call ID ${info.call_id}` : ""}</p>
    {/if}
    {#if parameters !== undefined}
      {@render block("Parameters", parameters, false)}
    {:else if info.args_status}
      <p>{argsNotice[info.args_status]}</p>
    {/if}
    {#if info.result_preview !== undefined}
      {@render block("Result", info.result_preview, !!info.result_preview_truncated)}
    {:else if !info.action && info.content}
      <pre class="trace-block">{info.content}</pre>
    {/if}
  </div>
</details>

{#snippet block(label: string, text: string, clipped: boolean)}
  <div>
    <div class="flex items-center justify-between gap-2">
      <span class="font-semibold">{label}{clipped ? " (preview truncated)" : ""}</span>
      <button type="button" class="btn btn-sm" aria-label={clipped ? "Copy preview" : `Copy ${label.toLowerCase()}`} onclick={() => copy(text)}>
        <icons.Copy size={12} />
      </button>
    </div>
    <pre class="trace-block"><ToolTraceJson {text} /></pre>
  </div>
{/snippet}

<style>
  .tool-detail {
    max-height: min(18rem, 40vh);
  }

  .trace-block {
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    padding: 0.5rem;
    border-radius: 0.25rem;
    background: var(--color-surface-100-900);
  }
</style>
