import { marked, type Tokens, type RendererObject } from 'marked';
import { apiUrl, escapeHtml } from './utils';

let procUrl = (url: string) => { return url; }

// Custom renderer
const renderer: RendererObject = {
  heading({ tokens, depth }) {
    const text = this.parser.parseInline(tokens) as string;
    const className = depth === 3 ? ` class="h${depth}"` : '';
    return `
      <h${depth}${className}>${text}</h${depth}>`;
  },

  // list(token: Tokens.List) {
  //   const listItems = token.items.map(item => {
  //     // Render each list item using the default listitem renderer
  //     // or a custom one if defined
  //     return this.listitem(item);
  //   }).join('');
  //   if (token.ordered) {
  //     const startAttr = token.start !== 1 ? ` start="${token.start}"` : '';
  //     return `<ol class="list-inside list-decimal space-y-1 py-2 pl-4"${startAttr}>\n${listItems}\n</ol>\n`;
  //   }
  //   return `<ul class="list-inside list-disc space-y-1 pl-4">
  //     ${listItems}</ul>\n`;
  // },

  listitem(item: Tokens.ListItem): string {
    const first = item.tokens[0];
    if (first?.type === 'text' || first?.type === 'paragraph') {
      const tokens = (first as any).tokens ?? [];
      const body = tokens.length
        ? this.parser.parseInline(tokens) as string
        : (first as any).text as string;
      const rest = item.tokens.length > 1
        ? this.parser.parse(item.tokens.slice(1)) as string
        : '';
      return `<li>${body}${rest}</li>\n`;
    }
    return `<li>${this.parser.parse(item.tokens)}</li>\n`;
  },

  link(token: Tokens.Link): string {
    let href = token.href || "";
    const prefix = "/scratch/packages";
    if (href.startsWith(prefix) && prefix.length < href.length) {
      const file = encodeURIComponent(href.substring(prefix.length + 1));
      href = procUrl(apiUrl(`/api/download?file=${file}`));
    }
    return `<a href="${href}" title="${token.title || 'Click to download ' + token.text}" download="${token.text}" class="underline text-primary-500">${token.text}</a>`;
  },

  code(token: Tokens.Code): string {
    let text = token.text;
    if (!token.escaped) {
      text = escapeHtml(text);
    }
    // return `<pre class="bg-surface-100-900 py-4 px-8 m-4 rounded"><code class="code text-xs whitespace-pre-wrap break-words">${text}</code></pre>`;

    // Unique id for the code block to target copy
    const codeId = `codeblock-${Math.random().toString(36).substring(2, 9)}`;
    return `
      <div class="relative my-4">
        <button
          class="absolute top-2 right-2 bg-surface-200-800 hover:bg-surface-300-700 text-xs px-2 py-1 rounded shadow"
          onclick="navigator.clipboard.writeText(document.getElementById('${codeId}').innerText)">
          Copy
        </button>
        <pre class="bg-surface-100-900 py-4 px-8 rounded leading-none"><code id="${codeId}" class="code text-xs whitespace-pre-wrap break-words">${text}</code></pre>
      </div>
    `;
  },

  codespan(token: Tokens.Codespan): string {
    return `<code class="code">${token.text}</code>`;
  },

  hr(token: Tokens.Hr): string {
    return `<hr class="hr" />`;
  },

  paragraph(token: Tokens.Paragraph): string {
    const text = this.parser.parseInline(token.tokens) as string;
    return `<p class="py-2">${text}</p>\n`;
  },

};

marked.use({ renderer, async: false });

export function renderMarkdown(content: string, urlProcessor?: (url: string) => string) {
  if (urlProcessor) {
    procUrl = urlProcessor;
  }
  return marked.parse(content);
}
