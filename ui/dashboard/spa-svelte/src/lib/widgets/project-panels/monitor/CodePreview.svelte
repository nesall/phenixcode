<script lang="ts">
  let { content, source }: { content: string; source: string } = $props();
  const languages: Record<string, string> = {
    c: "c", h: "c", cpp: "cpp", hpp: "cpp", cc: "cpp", cs: "csharp", java: "java",
    js: "javascript", jsx: "javascript", ts: "typescript", tsx: "typescript", py: "python",
    rs: "rust", php: "php", html: "html", sh: "bash", cmake: "cmake",
  };
  const highlighted = $derived.by(() => {
    const extension = source.split(".").pop()?.toLowerCase() ?? "";
    const language = languages[extension];
    if (!language || content.length > 65536 || !window.HLJS_CUSTOM?.hlAuto) return null;
    // The bundled highlighter escapes source text before producing markup.
    return window.HLJS_CUSTOM.hlAuto(content, [language]).value;
  });
</script>

<pre class="source-code overflow-auto rounded border border-surface-200-800 bg-surface-100-900 p-3 text-left font-mono text-xs leading-5"><code>{#if highlighted !== null}{@html highlighted}{:else}{content}{/if}</code></pre>

<style>
  .source-code :global(.hljs-comment), .source-code :global(.hljs-meta) { color: var(--color-surface-500); }
  .source-code :global(.hljs-keyword), .source-code :global(.hljs-type) { color: var(--color-primary-600); }
  .source-code :global(.hljs-string), .source-code :global(.hljs-title) { color: var(--color-success-600); }
  .source-code :global(.hljs-number), .source-code :global(.hljs-literal) { color: var(--color-warning-600); }
</style>
