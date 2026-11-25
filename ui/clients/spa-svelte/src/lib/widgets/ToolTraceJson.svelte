<script lang="ts">
  let { text }: { text: string } = $props();

  type Token = { text: string; color?: string };
  // A preview can end inside a string or immediately after a backslash.
  const jsonToken = /"(?:[^"\\\r\n]|\\[^\r\n])*(?:"|\\?$)|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?|\b(?:true|false|null)\b/g;

  function tokenize(text: string): Token[] {
    if (!/^\s*(?:\{|\[)/.test(text)) return [{ text }];

    const tokens: Token[] = [];
    let offset = 0;
    for (const match of text.matchAll(jsonToken)) {
      const value = match[0];
      const start = match.index;
      if (start > offset) tokens.push({ text: text.slice(offset, start) });
      offset = start + value.length;

      let color = "hljs-literal";
      if (value.startsWith('"')) {
        color = /^\s*:/.test(text.slice(offset)) ? "hljs-attr" : "hljs-string";
      } else if (/^[-\d]/.test(value)) {
        color = "hljs-number";
      }
      tokens.push({ text: value, color });
    }
    if (offset < text.length) tokens.push({ text: text.slice(offset) });
    return tokens;
  }

  const tokens = $derived(tokenize(text));
</script>

<code class="nohighlight">{#each tokens as token}{#if token.color}<span class={token.color}>{token.text}</span>{:else}{token.text}{/if}{/each}</code>
