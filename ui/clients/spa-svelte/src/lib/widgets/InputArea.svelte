<script lang="ts">
  import * as icons from "@lucide/svelte";
  import FileAttachments from "./FileAttachments.svelte";
  import ContextFiles from "./ContextFiles.svelte";

  interface Props {
    onSendMessage: (message: string) => void;
    sourceids: string[];
    attachments: File[];
    loading: boolean;
  }

  let { loading = false, sourceids = $bindable([]), attachments = $bindable([]), onSendMessage }: Props = $props();

  let input = $state("");

  let nofInputRows = $state(1);

  // let attachments: File[] = $state([]);

  function onSubmit(e: Event) {
    e.preventDefault();
    if (onSendMessage && (input.trim() || 0 < attachments.length || 0 < sourceids.length)) {
      onSendMessage(input.trim());
      input = "";
      attachments = [];
      nofInputRows = 1;
    }
  }

  function onTextereaKeyDown(ke: KeyboardEvent) {
    if (ke.key === "Enter" && !ke.shiftKey) {
      ke.preventDefault();
      onSubmit(ke);
    }
  }

  function onTextareaChange(e: Event) {
    const el = e.target as HTMLTextAreaElement;
    let str = el.value || "";

    // Count explicit newlines
    const newlineCount = (str.match(/\r\n|\r|\n/g) || []).length;

    // Estimate how many characters fit per line (rough heuristic)

    const style = getComputedStyle(el);

    const ctx = document.createElement("canvas").getContext("2d")!;
    ctx.font = style.font || "16px Arial";
    const sample = "Wow! The quick brown fox jumps over the lazy dog. _Yay 1234567890";
    const avgCharWidth = ctx.measureText(sample).width / sample.length;

    console.log("Font:", ctx.font, "; avgCharWidth:", avgCharWidth);

    // Calculate number of characters that fit in the textarea width

    const usableWidth = el.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);

    const charsPerLine = Math.floor(usableWidth / avgCharWidth);

    // Estimate wrapped lines
    const wrappedLines = Math.ceil(str.length / charsPerLine);

    // Combine both
    const totalLines = newlineCount + wrappedLines;

    nofInputRows = Math.max(1, Math.min(10, totalLines));
  }

  function onContextFiles(files: string[]) {
    sourceids = [...files];
  }
</script>

<form class="flex flex-col space-y-2 w-full" onsubmit={onSubmit}>
  <div
    class="flex flex-col rounded-xl border-1 p-3 w-full bg-surface-50-950 border-surface-200-800 relative"
    role="presentation"
    tabindex="-1"
  >
    <div class="flex flex-row w-full relative" role="presentation">
      <textarea
        class="text-md outline-none border-none w-full resize-vertical resize-none lg:max-h-48 lg:overflow-y-auto pb-2"
        placeholder="Type a message (Shift+Enter to add a new line)"
        id="msg-input"
        dir="auto"
        rows={nofInputRows}
        maxlength="4000"
        bind:value={input}
        disabled={loading}
        onkeydown={onTextereaKeyDown}
        oninput={onTextareaChange}
      ></textarea>
    </div>
    <div class="absolute2 left2-1 bottom2-1 flex flex-col space-y-2">
      <FileAttachments {loading} bind:attachments />

      <div class="flex space-x-1 w-full">
        <ContextFiles {loading} onChange={onContextFiles} />
        <div class="relative">
          <button
            type="submit"
            class="btn btn-sm preset-filled w-7 h-7 px-0 m-0 rounded-lg absolute right-0 bottom-0"
            aria-label="Send message"
            disabled={loading || !input.trim()}
            title="Send message (Enter)"
          >
            <icons.ArrowUp />
          </button>
        </div>
      </div>
    </div>
  </div>
</form>

<style>
  textarea::-webkit-scrollbar {
    width: 6px; /* narrow scrollbar width */
  }

  textarea::-webkit-scrollbar-track {
    background: #f1f1f1; /* track color */
  }

  textarea::-webkit-scrollbar-thumb {
    background: #888; /* thumb color */
    border-radius: 3px;
  }

  textarea::-webkit-scrollbar-thumb:hover {
    background: #555;
  }

  /* For Firefox */
  textarea {
    scrollbar-width: thin; /* "auto" | "thin" | "none" */
    scrollbar-color: #888 #f1f1f1; /* thumb | track */
  }
</style>
