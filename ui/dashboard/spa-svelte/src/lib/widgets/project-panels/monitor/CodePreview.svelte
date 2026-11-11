<script lang="ts">
  let { content, source }: { content: string; source: string } = $props();
  const languages: Record<string, string> = {
    c: "c",
    h: "cpp",
    cpp: "cpp",
    hpp: "cpp",
    cc: "cpp",
    cs: "csharp",
    java: "java",
    js: "javascript",
    jsx: "javascript",
    ts: "typescript",
    tsx: "typescript",
    py: "python",
    rs: "rust",
    php: "php",
    html: "html",
    sh: "bash",
    cmake: "cmake",
  };
  const highlighted = $derived.by(() => {
    const extension = source.split(".").pop()?.toLowerCase() ?? "";
    const language = languages[extension];
    if (!language || content.length > 65536 || !window.HLJS_CUSTOM?.hlAuto) return null;
    // The bundled highlighter escapes source text before producing markup.
    return window.HLJS_CUSTOM.hlAuto(content, [language]).value;
  });
</script>

<pre
  class="source-code overflow-auto rounded border border-surface-200-800 bg-surface-100-900 p-3 text-left font-mono text-xs leading-5"><code
    class="nohighlight"
    >{#if highlighted !== null}{@html highlighted}{:else}{content}{/if}</code
  ></pre>

<style>
  .source-code {
    min-width: 0;
    max-width: 100%;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }

  .source-code :global(.hljs) {
    background: none !important;
    color: inherit;
  }

  .source-code :global(.hljs-title) {
    color: inherit;
  }

  :global(html[data-mode="light"]) .source-code :global(.hljs-keyword),
  :global(html[data-mode="light"]) .source-code :global(.hljs-meta) {
    color: #8f08c4 !important;
  }

  :global(html[data-mode="light"]) .source-code :global(.hljs-selector-tag),
  :global(html[data-mode="light"]) .source-code :global(.hljs-built_in),
  :global(html[data-mode="light"]) .source-code :global(.hljs-name),
  :global(html[data-mode="light"]) .source-code :global(.hljs-tag) {
    color: slategray;
  }

  :global(html[data-mode="light"]) .source-code :global(.hljs-number) {
    color: #800080;
  }

  :global(html[data-mode="light"]) .source-code :global(.hljs-section),
  :global(html[data-mode="light"]) .source-code :global(.hljs-attribute),
  :global(html[data-mode="light"]) .source-code :global(.hljs-literal),
  :global(html[data-mode="light"]) .source-code :global(.hljs-template-tag),
  :global(html[data-mode="light"]) .source-code :global(.hljs-template-variable),
  :global(html[data-mode="light"]) .source-code :global(.hljs-type) {
    color: #2b91af;
  }
</style>
