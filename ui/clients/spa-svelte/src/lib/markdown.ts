import { marked, type Tokens, type RendererObject } from 'marked';
import { apiUrl, escapeHtml } from './utils';

let procUrl = (url: string) => { return url; }

const svgCopy = `
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-copy preview-icon">
        <rect width="14" height="14" x="8" y="8" rx="2" ry="2"/>
        <path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2"/>
      </svg>`;

const svgCheck = `
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-check preview-icon">
      <path d="M20 6 9 17l-5-5"/>
    </svg>`;

// Custom renderer
const renderer: RendererObject = {
  heading({ tokens, depth }) {
    const text = this.parser.parseInline(tokens) as string;
    const className = depth === 3 ? ` class="h${depth}"` : '';
    return `
      <h${depth}${className}>${text}</h${depth}>`;
  },

  list(token: Tokens.List) {
    const listItems = token.items.map(item => this.listitem(item)).join('');
    if (token.ordered) {
      const startAttr = token.start !== 1 ? ` start="${token.start}"` : '';
      return `<ol class="list-inside list-decimal space-y-1 py-2 pl-4"${startAttr}>\n${listItems}\n</ol>\n`;
    }
    return `<ul class="list-inside list-disc space-y-1 pl-4">\n${listItems}</ul>\n`;
  },

  listitem(item: Tokens.ListItem): string {
    const checkbox = item.task
      ? `<input type="checkbox" ${item.checked ? 'checked ' : ''}disabled class="mr-1 align-middle" /> `
      : '';

    const first = item.tokens[0];
    if (first?.type === 'text' || first?.type === 'paragraph') {
      const tokens = ((first as any).tokens ?? []).filter((t: any) => t.type !== 'checkbox');
      const body = tokens.length
        ? this.parser.parseInline(tokens) as string
        : (first as any).text as string;
      const rest = item.tokens.length > 1
        ? this.parser.parse(item.tokens.slice(1)) as string
        : '';
      const liClass = item.task ? ' class="list-none"' : '';
      return `<li${liClass}>${checkbox}${body}${rest}</li>\n`;
    }
    return `<li>${this.parser.parse(item.tokens)}</li>\n`;
  },

  link(token: Tokens.Link): string {
    const href = escapeHtml(token.href || '');
    const text = escapeHtml(token.text || '');
    const title = escapeHtml(token.title || '');
    return `<a href="${href}"${title ? ` title="${title}"` : ''} class="underline text-primary-500">${text}</a>`;
  },

  code(token: Tokens.Code): string {
    let text = token.text;
    if (!token.escaped) {
      text = escapeHtml(text);
    }

    // Unique id for the code block to target copy
    const codeId = `codeblock-${Math.random().toString(36).substring(2, 9)}`;
    return `
      <div class="relative my-4">
        <button
          type="button"
          class="copy-code cursor-pointer absolute top-2 right-2 bg-surface-200-800 hover:bg-surface-300-700 text-xs px-2 py-1 rounded shadow"
          title="Copy code"
          aria-label="Copy code"
          data-code-id="${codeId}">
          <span class="copy-code-icons" aria-hidden="true">
            <span class="copy-code-default">${svgCopy}</span>
            <span class="copy-code-success">${svgCheck}</span>
            <span class="copy-code-error">✗</span>
          </span>
        </button>
        <pre class="bg-surface-100-900 py-4 px-8 rounded leading-none">
          <code id="${codeId}" class="code text-xs whitespace-pre-wrap wrap-break-word">${text}</code>
        </pre>
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

  table(token: Tokens.Table): string {
    const tableId = `markdown-table-${Math.random().toString(36).substring(2, 9)}`;
    const header = token.header.map((cell, i) => {
      const align = token.align[i];
      const alignClass = align ? ` text-${align}` : '';
      const content = this.parser.parseInline(cell.tokens) as string;

      return `<th scope="col" class="px-4 py-3 font-semibold text-xs uppercase tracking-wide text-surface-700-300${alignClass}">${content}</th>`;
    }).join('');

    const rows = token.rows.map(row => {
      const cells = row.map((cell, i) => {
        const align = token.align[i];
        const alignClass = align ? ` text-${align}` : '';
        const content = this.parser.parseInline(cell.tokens) as string;

        return `<td class="px-4 py-3${alignClass}">${content}</td>`;
      }).join('');

      return `<tr class="transition-colors hover:bg-surface-100-900">${cells}</tr>`;
    }).join('\n');

    return `
    <div class="relative my-4 overflow-x-auto rounded-lg border border-surface-200-800">
      <button
        type="button"
        class="copy-table cursor-pointer absolute top-2 right-2 z-10 bg-surface-200-800 hover:bg-surface-300-700 text-xs px-2 py-1 rounded shadow"
        title="Copy table"
        aria-label="Copy table"
        data-copy-label="Copy table"
        data-table-id="${tableId}">
        <span class="copy-code-icons" aria-hidden="true">
          <span class="copy-code-default">${svgCopy}</span>
          <span class="copy-code-success">${svgCheck}</span>
          <span class="copy-code-error">✗</span>
        </span>
      </button>
      <table id="${tableId}" class="w-full text-left text-sm text-surface-700-300">
        <thead class="bg-surface-100-900">
          <tr>${header}</tr>
        </thead>
        <tbody class="divide-y divide-surface-200-800">
          ${rows}
        </tbody>
      </table>
    </div>
  `;
  },

};

marked.use({ renderer, async: false, gfm: true });

export function renderMarkdown(content: string, urlProcessor?: (url: string) => string) {
  if (urlProcessor) {
    procUrl = urlProcessor;
  }
  return marked.parse(content);
}

const copyFeedbackTimers = new WeakMap<HTMLButtonElement, ReturnType<typeof setTimeout>>();

function showCopyFeedback(button: HTMLButtonElement, state: 'success' | 'error') {
  clearTimeout(copyFeedbackTimers.get(button));
  button.dataset.copyState = state;
  const label = state === 'success' ? 'Copied' : 'Copy failed';
  const copyLabel = button.dataset.copyLabel || 'Copy code';
  button.title = label;
  button.setAttribute('aria-label', label);
  copyFeedbackTimers.set(button, setTimeout(() => {
    delete button.dataset.copyState;
    button.title = copyLabel;
    button.setAttribute('aria-label', copyLabel);
    copyFeedbackTimers.delete(button);
  }, 1500));
}

document.addEventListener('click', async (event) => {
  const target = event.target as HTMLElement;
  const button = target.closest<HTMLButtonElement>('.copy-code, .copy-table');
  if (!button) {
    return;
  }

  try {
    const tableId = button.dataset.tableId;
    if (tableId) {
      const table = document.getElementById(tableId);
      if (!table) {
        return;
      }
      const tsv = Array.from(table.querySelectorAll('tr'))
        .map(row => Array.from(row.querySelectorAll('th, td'))
          .map(cell => (cell as HTMLElement).innerText.replace(/[\t\r\n]+/g, ' '))
          .join('\t'))
        .join('\n');
      await navigator.clipboard.writeText(tsv);
      showCopyFeedback(button, 'success');
      return;
    }

    const codeId = button.dataset.codeId;
    if (!codeId) {
      return;
    }
    const code = document.getElementById(codeId);
    if (!code) {
      return;
    }
    await navigator.clipboard.writeText(code.innerText);
    console.log("markdown.ts code copied OK");
    showCopyFeedback(button, 'success');
  } catch (error) {
    console.error('Failed to copy markdown content:', error);
    showCopyFeedback(button, 'error');
  }
});
