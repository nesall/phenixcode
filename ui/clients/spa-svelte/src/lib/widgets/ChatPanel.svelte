<script lang="ts">
  import InputArea from "./InputArea.svelte";
  import * as icons from "@lucide/svelte";
  import { fade, slide } from "svelte/transition";
  import { onMount, tick } from "svelte";
  import DOMPurify from "dompurify";
  import { renderMarkdown } from "../markdown";
  import { apiUrl, clog, isGoodArray, stripCommonPrefix, toaster } from "../utils";
  import { contextSizeRatio, messages, settings, temperature } from "../store";

  export function resetUi() {
    loading = false;
    started = false;
    metaInfoArray = [];
  }

  $effect(() => {
    clog("ChatPanel $settings changed:", $state.snapshot($settings));
    clog("ChatPanel $temperature changed:", $state.snapshot($temperature));
  });

  async function insertTestMessages() {
    $messages = [
      {
        role: "user",
        content: "Hello there!",
        _html: "",
      },
      {
        role: "assistant",
        content: "Hello! How can I assist you today?",
        _html: await renderMarkdown("Hello! How can I assist you today?"),
        _metaInfoArray: ["Searching for relevant content", "Processing attachment(s)", "Working on the response"],
      },
      {
        role: "user",
        content: "Can you tell me a joke?",
        _html: "",
      },
      {
        role: "assistant",
        content: "Sure! Why don't scientists trust atoms? Because they make up everything!",
        _html: await renderMarkdown("Sure! Why don't scientists trust atoms? Because they make up everything!"),
      },
      {
        role: "user",
        content: "Can you tell me a joke?",
        _html: "",
      },
      {
        role: "assistant",
        content: "Sure! Why don't scientists trust atoms? Because they make up everything!",
        _html: await renderMarkdown("Sure! Why don't scientists trust atoms? Because they make up everything!"),
      },
      {
        role: "user",
        content: "Can you tell me a joke?",
        _html: "",
      },
      {
        role: "assistant",
        content: "Sure! Why don't scientists trust atoms? Because they make up everything!",
        _html: await renderMarkdown("Sure! Why don't scientists trust atoms? Because they make up everything!"),
      },
    ];
  }

  interface Attachment {
    filename: string;
    content: string;
  }

  let loading = $state(false);
  let messagesEndDiv: HTMLDivElement;
  let started = $state(false);
  // let messages = $state<ChatMessage[]>([]);
  let showScrollBtn = $state(false);

  let metaInfoArray: string[] = $state([]);
  let sourceids: string[] = $state([]);
  let attachments: File[] = $state([]);
  let attachmentsLoaded: Attachment[] = $state([]);

  let attachedFilesOnly = $state(false);

  const metaInfo = $derived(0 < metaInfoArray.length ? metaInfoArray[metaInfoArray.length - 1] : "");

  const hasAttachedFiles = $derived(isGoodArray(sourceids) || isGoodArray(attachments));

  function checkMessagesEndVisibility() {
    if (!messagesEndDiv) return;
    // console.log("checkMessagesEndVisibility");
    const rect = messagesEndDiv.getBoundingClientRect();
    showScrollBtn = window.innerHeight < rect.bottom;
  }

  onMount(() => {
    //insertTestMessages();

    const wrapper = document.querySelector(".chat-panel") as HTMLDivElement | null | undefined;
    if (wrapper) wrapper.addEventListener("scroll", checkMessagesEndVisibility);
    window.addEventListener("resize", checkMessagesEndVisibility);
    tick().then(checkMessagesEndVisibility);
    return () => {
      if (wrapper) wrapper.removeEventListener("scroll", checkMessagesEndVisibility);
      window.removeEventListener("resize", checkMessagesEndVisibility);
    };
  });

  $effect(() => {
    if (messages) checkMessagesEndVisibility();
  });

  function onSendMessage(message: string) {
    if (!message.trim() && attachments.length === 0) return;
    message = message.trim();
    if (attachments.length === 0) {
      sendMessage(message, [], sourceids, true);
    } else {
      let loaded = attachmentsLoaded.length === attachments.length;
      if (loaded) {
        for (const file of attachments) {
          const match = attachmentsLoaded.find((att) => att.filename === file.name);
          if (!match) {
            loaded = false;
            break;
          }
        }
      }
      if (loaded) {
        clog("All attachments already loaded.");
        sendMessage(message, attachmentsLoaded, sourceids, true);
        return;
      }
      const loadFile = (file: File) =>
        new Promise<Attachment>((resolve, reject) => {
          const r = new FileReader();
          r.onload = () => resolve({ filename: file.name, content: r.result as string });
          r.onerror = reject;
          r.readAsText(file);
        });

      Promise.all(attachments.map(loadFile))
        .then((atts) => {
          attachmentsLoaded = atts;
          sendMessage(message, atts, sourceids, true);
        })
        .catch((err) => {
          toaster.error({ title: "Error reading attachment files", description: err.message || err });
          clog("Error reading attachment files:", err);
        });
    }
  }

  function normalizeHeaders(s: string) {
    return s
      .replace(/<h[1-5]\b([^>]*)>/gi, (_, attrs) => {
        const updatedAttrs = attrs.replace(/class="h[1-5]"/gi, 'class="h6"');
        return `<h5${updatedAttrs}>`;
      })
      .replace(/<\/h[1-5]>/gi, "</h6>");
  }

  function processResponse(s: string) {
    return s
      .replace(/(\n){3,}/g, "\n\n") // Replace 3 or more newlines with 2
      .replace(/^\n+/, "") // Remove leading newlines
      .replace(/\n+$/, ""); // Remove trailing newlines
  }

  const metaTagBegin = "[meta]";

  function parseFromSSE(chunk: string): { parsed: string; remainder: string } {
    // console.log("parseFromSSE chunk:", chunk);
    let len = chunk.length;
    if (len === 0) return { parsed: "", remainder: "" };
    let fullResponse: string = "";
    let buffer: string = ""; // holds leftover partial data
    // SSE format: "data: <payload>\n\n"
    buffer += chunk.substring(0, len);
    let pos: number;
    while ((pos = buffer.indexOf("\n\n")) !== -1) {
      const event = buffer.substring(0, pos); // one SSE event
      buffer = buffer.substring(pos + 2);
      if (event.startsWith("data: ")) {
        // console.log(event);
        const jsonStr = event.substring(6);
        if (jsonStr === "[DONE]") {
          break;
        }
        const chunkJson = JSON.parse(jsonStr); // validate JSON
        if (chunkJson.sources && chunkJson.type == "context_sources") {
          let sources: string[] = [];
          for (const a of chunkJson.sources as string[]) {
            sources.push(a);
          }
          sources = stripCommonPrefix(sources);
          fullResponse += `\n\nSources:  \n`;
          console.log("Sources: ", sources);
          for (const a of sources as string[]) {
            fullResponse += `*${a}*  \n`;
          }
        } else {
          const content = chunkJson.content || "";
          fullResponse += content;
          if (content.startsWith(metaTagBegin)) {
            return { parsed: content, remainder: buffer };
          }
        }
      }
    }
    return { parsed: fullResponse, remainder: buffer };
  }

  async function sendMessage(input: string, attachments: Attachment[], sourceids: string[], appendQ = true) {
    if (loading) return;
    loading = true;
    started = false;
    if (appendQ) {
      if (!input.trim()) return;
      $messages = [
        ...$messages,
        {
          role: "user",
          content: input.trim(),
          _html: "",
        },
      ];
      input = "";
      tick().then(() => messagesEndDiv?.scrollIntoView({ behavior: "smooth" }));
    }
    console.log("ChatPanel.sendMessage");
    try {
      const messagesToSend = $messages.map((m) => ({
        role: m.role,
        content: m.content,
      }));
      clog("Sending message to server...", {
        messagesToSend,
        attachments,
        sourceids,
        temperature: $state.snapshot($temperature),
        settings: $state.snapshot($settings),
      });
      const response = await fetch(apiUrl("/api/chat"), {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        body: JSON.stringify({
          messages: messagesToSend,
          attachments,
          sourceids,
          targetapi: $settings.currentApi,
          temperature: $temperature,
          ctxratio: $contextSizeRatio,
          attachedonly: attachedFilesOnly,
        }),
      });
      if (!response.ok) {
        throw new Error("Failed to send message");
      }
      let appended = false;
      const reader = response.body?.getReader();
      const decoder = new TextDecoder();
      while (reader) {
        const { done, value } = await reader.read();
        if (done) break;

        let bufferToParse = decoder.decode(value, { stream: true });
        while (bufferToParse) {
          const { parsed: chunk, remainder } = parseFromSSE(bufferToParse);
          bufferToParse = remainder;
          if (!chunk && !appended) continue; // skip empty starting text
          if (chunk.includes(metaTagBegin)) {
            // console.log(chunk);
            metaInfoArray = [...metaInfoArray, chunk.substring(6)];
            continue;
          }
          if (appended) {
            let lm = $messages[$messages.length - 1];
            lm.content += chunk;
            lm._html = normalizeHeaders(await renderMarkdown(lm.content));
            $messages = $messages;
            tick().then(checkMessagesEndVisibility);
          } else {
            $messages = [
              ...$messages,
              {
                role: "assistant",
                content: chunk,
                _html: normalizeHeaders(await renderMarkdown(chunk)),
              },
            ];
            appended = true;
            started = true;
          }
        }
      }
      let lm = $messages[$messages.length - 1];
      lm.content = processResponse(lm.content);
      lm._html = normalizeHeaders(await renderMarkdown(lm.content));
      lm._metaInfoArray = [...metaInfoArray];
      $messages = $messages;
      console.log("lm._metaInfoArray", lm._metaInfoArray);
    } catch (error) {
      clog("Error sending message:", error);
      $messages = [
        ...$messages,
        {
          role: "assistant",
          content: "Sorry, there was an error processing your request.",
          _html: "",
        },
      ];
    } finally {
      resetUi();
      // if (window.PR && window.PR.prettyPrint) {
      //   window.PR.prettyPrint();
      // }
      setTimeout(() => {
        if (window.HLJS_CUSTOM && window.HLJS_CUSTOM.initHljs) window.HLJS_CUSTOM.initHljs();
      }, 250);
    }
  }

  function onCopyMsg(content: string) {
    clog("Copying message:", content);
    navigator.clipboard.writeText(content).then(
      () => {
        toaster.success({ title: "Message copied to clipboard" });
        clog("Text copied to clipboard");
      },
      (err) => {
        toaster.error({
          title: "Unable to copy message",
          description: err.message,
        });
        clog("Could not copy text: ", err);
      },
    );
  }

  function onEditMsg(index: number) {
    const msg = $messages[index];
    if (msg.role !== "user") return;
    const div = document.querySelector(`#user-message-${index}`) as HTMLElement | null;
    if (div) {
      div.contentEditable = "plaintext-only";
      div.focus();
      const range = document.createRange();
      range.selectNodeContents(div);
      range.collapse(false);
      const sel = window.getSelection();
      sel?.removeAllRanges();
      sel?.addRange(range);
      div.addEventListener(
        "blur",
        () => {
          div.contentEditable = "false";
          const newContent = (div.textContent || "").trim();
          console.log("Edited content:", newContent);
          div.innerHTML = newContent;
          $messages[index].content = newContent;
          $messages = $messages;
        },
        { once: true },
      );
    }
  }

  function onRetry(index: number) {
    const msg = $messages[index];
    if (msg.role !== "assistant") return;
    if ((index & 1) === 1) {
      const userMsg = $messages[index - 1];
      if (userMsg && userMsg.role === "user") {
        $messages = $messages.slice(0, index);
        sendMessage(userMsg.content, [], sourceids, false);
      } else {
        toaster.error({
          title: "Unpredicted error occurred when retrying an answer.",
        });
      }
    }
  }

  function onThumbsFeedback(index: number, feedback: "good" | "bad") {
    const msg = $messages[index];
    if (msg.role !== "assistant") return;
    // Send feedback to the backend or handle it accordingly
    // This is a placeholder; actual implementation may vary
    toaster.info({ title: `Feedback received: ${feedback}` });
  }
</script>

<div class="chat-panel p-3 w-full h-full flex flex-col space-y-8 overflow-y-auto">
  <div class="flex flex-col space-y-6 mb-4 grow p-4" id="chat-messages">
    {#if $messages.length === 0}
      <p class="text text-center text-surface-500">No messages yet. Start the conversation!</p>
    {/if}
    {#each $messages as msg, i}
      {#if msg.role === "user"}
        <div class="flex flex-col items-end overflow-y-hidden box-border message" data-role="user">
          <div
            class="bg-primary-50-950 shadow2 rounded-xl whitespace-pre-wrap p-4 m-1 break-normal text-left message-content"
            id="user-message-{i}"
          >
            {msg.content}
          </div>
          <div class="flex gap-2 mt-1">
            <button
              type="button"
              class="btn btn-sm px-1"
              onclick={() => onCopyMsg(msg.content)}
              title="Copy to clipboard"
            >
              <icons.Copy size={16} />
            </button>
            <button
              type="button"
              class="btn btn-sm px-1"
              onclick={() => onEditMsg(i)}
              disabled={loading}
              title="Edit your original question"
            >
              <icons.SquarePen size={16} />
            </button>
          </div>
        </div>
      {:else}
        <div class="flex flex-col overflow-y-hidden box-border pb-4 space-y-1 message" data-role="assistant">
          {#if isGoodArray(msg._metaInfoArray) && msg._metaInfoArray}
            <div class="text-xs flex flex-col space-y-0 self-end text-right">
              <button
                type="button"
                class="btn btn-sm text-surface-500 border-surface-500 flex text-right justify-end"
                onclick={() => ($messages[i]._metaVisible = !$messages[i]._metaVisible)}
              >
                <span>Ready</span>
                {#if msg._metaVisible}
                  <icons.ChevronUp size={16} />
                {:else}
                  <icons.ChevronDown size={16} />
                {/if}
              </button>
              {#if msg._metaVisible}
                <div class="flex flex-col" transition:slide>
                  {#each msg._metaInfoArray as info, i}
                    <span>{msg._metaInfoArray[msg._metaInfoArray?.length - 1 - i]} ✓</span>
                  {/each}
                </div>
              {/if}
            </div>
          {/if}
          <div
            class="border2 border-surface-100-900 bg-surface-500/5 shadow2 rounded-xl whitespace-normal p-4 break-normal text-left message-content"
          >
            {#if msg._html}
              {@html DOMPurify.sanitize(msg._html, {
                ADD_ATTR: ["onclick"],
              })}
            {:else}
              {msg.content}
            {/if}
          </div>
          <div class="flex items-center">
            <button
              type="button"
              class="btn btn-sm px-2"
              onclick={() => onCopyMsg(msg.content)}
              disabled={loading}
              title="Copy to clipboard"
            >
              <icons.Copy size={16} />
            </button>
            <button
              type="button"
              class="btn btn-sm px-1"
              onclick={() => onThumbsFeedback(i, "good")}
              disabled={loading}
              title="Good answer"
            >
              <icons.ThumbsUp size={16} />
            </button>
            <button
              type="button"
              class="btn btn-sm px-1"
              onclick={() => onThumbsFeedback(i, "bad")}
              disabled={loading}
              title="Incorrect or unhelpful answer"
            >
              <icons.ThumbsDown size={16} />
            </button>
            <span class="vr mx-1 h-[1rem]"></span>
            <button
              type="button"
              class="btn btn-sm px-1"
              onclick={() => onRetry(i)}
              disabled={loading}
              title="Retry the answer"
            >
              <icons.RefreshCw size={16} />
            </button>
          </div>
        </div>
      {/if}
    {/each}
    {#if loading && !started}
      <div class="italic text-right text-surface-500 text-sm">
        {metaInfo || "Thinking..."}
      </div>
    {/if}
    <div class="min-h-[4rem]"></div>
    <div bind:this={messagesEndDiv}></div>
  </div>

  <div class="sticky bottom-0 flex items-end pb-0 pt-4 relative gradient-to-t from-surface-50-950">
    {#if showScrollBtn}
      <div class="absolute top-[-1.5rem] w-full flex" transition:fade>
        <button
          type="button"
          class="btn preset-filled-surface-100-900 w-8 h-8 p-0 rounded-full mx-auto"
          aria-label="Scroll to bottom"
          id="scroll-to-bottom-btn"
          onclick={() => messagesEndDiv?.scrollIntoView({ behavior: "smooth" })}
        >
          <icons.ArrowDown />
        </button>
      </div>
    {/if}
    <InputArea {onSendMessage} bind:sourceids bind:attachments {loading} />

    <div
      class="flex items-center absolute left-4 bottom-0 z-50 bg-surface-50-950 px-2 rounded gap-1 translate-y-1/3"
      title="If on, only files attached to the message will be used as context"
    >
      <input
        type="checkbox"
        id="checkbox-attached-files-only"
        checked={attachedFilesOnly && hasAttachedFiles}
        disabled={!hasAttachedFiles}
        onchange={(ev) => {
          attachedFilesOnly = (ev.target as HTMLInputElement)?.checked;
        }}
      />
      <label class="text-xs {hasAttachedFiles ? '' : 'text-surface-500'}" for="checkbox-attached-files-only">
        Use attached files only
      </label>
    </div>
  </div>
</div>
