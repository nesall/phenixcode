import { renderMarkdown } from "./markdown";
import { normalizeHeaders } from "./utils";

// Dynamically imported by ChatPanel only for the explicit development preview.
// Vite watches these raw imports, so changing/adding a page needs no TS changes.
const pages = import.meta.glob<string>("../../tests/pages/*.md", {
  query: "?raw",
  import: "default",
  eager: true,
});

export async function loadTestMessages(): Promise<ChatMessage[]> {
  const messages: ChatMessage[] = [];
  for (const [path, markdown] of Object.entries(pages).sort(([left], [right]) => left.localeCompare(right))) {
    const filename = path.split("/").pop()!;
    const words = filename.replace(/\.md$/i, "").replace(/[-_]+/g, " ");
    const title = words.charAt(0).toUpperCase() + words.slice(1);
    messages.push(
      { role: "user", content: `Preview: ${title}`, _html: "" },
      {
        role: "assistant",
        content: markdown,
        _html: normalizeHeaders(await renderMarkdown(markdown)),
        _metaInfoArray: [`Loaded ${filename}`],
      },
    );
  }
  return messages;
}
