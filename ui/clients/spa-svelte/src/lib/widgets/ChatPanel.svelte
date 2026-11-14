<script lang="ts">
  import InputArea from "./InputArea.svelte";
  import ReasoningPicker from "./ReasoningPicker.svelte";
  import * as icons from "@lucide/svelte";
  import { fade, slide } from "svelte/transition";
  import { onMount, tick } from "svelte";
  import DOMPurify from "dompurify";
  import { renderMarkdown } from "../markdown";
  import { apiUrl, clog, isGoodArray, newUUID, normalizeHeaders, saveChat, stripCommonPrefix, toaster } from "../utils";
  import { reasoningParameters, reasoningReady, reasoningRequest } from "../reasoning";
  import {
    contextSizeRatio,
    messages,
    settings,
    temperature,
    bIncognito,
    instances,
    curInstance,
    currentChatId,
    bLoading,
    sessionId,
    bFreeChat,
  } from "../store";

  const reasoningProvider = $derived($settings.completionApis.find((api) => api.id === $settings.currentApi));
  const hasReasoning = $derived(reasoningParameters(reasoningProvider).length > 0);

  let inputArea = $state<InputArea>();

  export function resetUi() {
    $bLoading = false;
    started = false;
    activeAssistantIndex = -1;
    metaInfoArray = [];
    reasoningTextRaw = "";
    sourceids = [];
    attachments = [];
    attachmentsLoaded = [];
    inputArea?.resetContext();
  }

  $effect(() => {
    clog("ChatPanel $settings changed:", $state.snapshot($settings));
    clog("ChatPanel $temperature changed:", $state.snapshot($temperature));
  });

  interface Attachment {
    filename: string;
    content: string;
  }

  // let loading = $state(false);
  let messagesEndDiv: HTMLDivElement;
  let started = $state(false);
  let activeAssistantIndex = $state(-1);
  let liveReasoningOpen = $state(true);
  let reasoningUserChanged = false;
  let liveReasoningDiv = $state<HTMLDivElement>();
  let followLiveReasoning = true;

  function toggleLiveReasoning() {
    reasoningUserChanged = true;
    liveReasoningOpen = !liveReasoningOpen;
    followLiveReasoning = true;
  }

  function trackReasoningScroll() {
    if (liveReasoningDiv)
      followLiveReasoning = liveReasoningDiv.scrollHeight - liveReasoningDiv.scrollTop - liveReasoningDiv.clientHeight < 24;
  }
  // let messages = $state<ChatMessage[]>([]);
  let showScrollBtn = $state(false);

  let reasoningTextRaw = $state("");
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
    let mounted = true;
    if (import.meta.env.DEV && new URLSearchParams(location.search).has("testMessages")) {
      const initialMessages = $messages;
      const initialChatId = $currentChatId;
      const initialSession = $sessionId;
      void (async () => {
        try {
          const { loadTestMessages } = await import("../test-messages");
          const previews = await loadTestMessages();
          if (mounted && !$bLoading && $messages === initialMessages &&
              $currentChatId === initialChatId && $sessionId === initialSession)
            $messages = previews;
        } catch (error) {
          clog("Unable to load Markdown test pages:", error);
          if (mounted) toaster.error({ title: "Could not load test pages." });
        }
      })();
    }

    const wrapper = document.querySelector(".chat-panel") as HTMLDivElement | null | undefined;
    if (wrapper) wrapper.addEventListener("scroll", checkMessagesEndVisibility);
    window.addEventListener("resize", checkMessagesEndVisibility);
    tick().then(checkMessagesEndVisibility);
    return () => {
      mounted = false;
      if (wrapper) wrapper.removeEventListener("scroll", checkMessagesEndVisibility);
      window.removeEventListener("resize", checkMessagesEndVisibility);
    };
  });

  $effect(() => {
    if (messages) checkMessagesEndVisibility();
  });

  function constructPersistedChat(inst: AppInstance) {
    const persistedChat: PersistedChat = {
      project_id: inst.project_id || "",
      title: ($messages.find((m) => m.role === "user")?.content || inst.name).slice(0, 100),
      id: $currentChatId,
      messages: $messages.map((m) => ({
        role: m.role,
        content: m.content,
        replay: m.replay?.content === m.content ? { id: m.replay.id, session: m.replay.session } : undefined,
      })),
      free_chat: $bFreeChat ? 1 : 0,
    };
    // Filter out consecutive user messages that have identical content.
    // Might happen e.g. when server stopped responding and user retries the same message.
    persistedChat.messages = persistedChat.messages.filter((m, i, arr) => {
      const prev = arr[i - 1];
      return !(prev && m.role === "user" && prev.role === "user" && m.content === prev.content);
    });
    return persistedChat;
  }

  async function persistChat(inst: AppInstance | undefined) {
    if ($bIncognito || !inst) return;
    try {
      const res = await saveChat(constructPersistedChat(inst));
      if (res.status === "success") {
        if (!$currentChatId) $currentChatId = res.id || ""; // set-once, don't overwrite
      } else {
        console.log("Failed to save chat:", res.message);
      }
    } catch (err) {
      console.log("saveChat failed:", err);
    }
  }

  function onSendMessage(message: string) {
    if (!message.trim() && attachments.length === 0) return;
    message = message.trim();
    const request = reasoningRequest($settings.currentApi, $settings.completionApis);
    if (attachments.length === 0) {
      sendMessage(message, [], sourceids, true, request);
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
        sendMessage(message, attachmentsLoaded, sourceids, true, request);
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
          sendMessage(message, atts, sourceids, true, request);
        })
        .catch((err) => {
          toaster.error({
            title: "Error reading attachment files",
            description: err.message || err,
          });
          clog("Error reading attachment files:", err);
        });
    }
  }

  function processResponse(s: string) {
    return s
      .replace(/(\n){3,}/g, "\n\n") // Replace 3 or more newlines with 2
      .replace(/^\n+/, "") // Remove leading newlines
      .replace(/\n+$/, ""); // Remove trailing newlines
  }

  function stripModelEchoedSources(content: string): string {
    const blocks = [...content.matchAll(/\n*Sources:\s*(?:\n\s*[*_]?[\w./-]+[*_]?\s*)+/gi)];
    if (blocks.length < 2) return content;
    console.log("stripModelEchoedSources: found", blocks.length, "matches");
    // remove every match except the last (the officially-appended one)
    for (let i = blocks.length - 2; i >= 0; i--) {
      const m = blocks[i];
      content = content.slice(0, m.index) + content.slice(m.index! + m[0].length);
    }
    return content;
  }

  // const metaTagBegin = "[meta]";
  let receivedReplayId = "";

  function parseFromSSE(chunk: string): { parsed: string; remainder: string; reasoning?: string; meta?: string[] } {
    // console.log("parseFromSSE chunk:", chunk);
    let len = chunk.length;
    if (len === 0) return { parsed: "", remainder: "" };
    let fullResponse: string = "";
    let fullReasoning: string = "";
    const meta: string[] = [];
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
        if (chunkJson.type === "context_sources") {
          if (typeof chunkJson.replay_id === "string") {
            receivedReplayId = chunkJson.replay_id;
          }
          if (chunkJson.sources) {
            let sources: string[] = [];
            for (const a of chunkJson.sources as string[]) {
              sources.push(a);
            }
            sources = stripCommonPrefix(sources);
            console.log("parseFromSSE. sources: ", sources);
            fullResponse += `\n\nSources:  \n`;
            console.log("Sources: ", sources);
            for (const a of sources as string[]) {
              fullResponse += `*${a}*  \n`;
            }
            if (chunkJson.model) {
              console.log("parseFromSSE. model: ", chunkJson.model);
              fullResponse += `\n\n<span class="text-surface-500 text-xs">Model: ${chunkJson.model}</span>  \n`;
            }
          }
        } else if (chunkJson.type === "meta") {
          if (typeof chunkJson.content === "string" && 0 < chunkJson.content?.length) {
            meta.push(chunkJson.content);
          }
        } else if (chunkJson.type === "reasoning_delta") {
          if (typeof chunkJson.content === "string" && 0 < chunkJson.content?.length) {
            fullReasoning += chunkJson.content;
          }
        } else if (chunkJson.error) {
          fullResponse += `\n\nError: ${chunkJson.error}  \n`;
          return { parsed: fullResponse, remainder: buffer, reasoning: fullReasoning, meta };
        } else {
          fullResponse += chunkJson.content || "";
        }
      }
    }
    return { parsed: fullResponse, remainder: buffer, reasoning: fullReasoning, meta };
  }

  async function sendMessage(
    input: string,
    attachments: Attachment[],
    sourceids: string[],
    appendQ = true,
    request = reasoningRequest($settings.currentApi, $settings.completionApis),
  ) {
    if ($bLoading) return;
    let renderTimer: ReturnType<typeof setTimeout> | undefined;
    let pendingRender = Promise.resolve();
    let answerDirty = false;
    let reasoningDirty = false;
    liveReasoningOpen = true;
    reasoningUserChanged = false;
    followLiveReasoning = true;
    activeAssistantIndex = -1;
    $bLoading = true;
    started = false;
    reasoningTextRaw = "";
    metaInfoArray = [];
    if (appendQ) {
      if (!input.trim()) {
        toaster.error({
          title: "Cannot send empty message",
        });
        $bLoading = false;
        return;
      }
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

    if (!$sessionId) $sessionId = newUUID();
    const requestSessionId = $sessionId;
    const responseId = newUUID();
    receivedReplayId = "";

    const inst = $instances.find((i) => i.id === $curInstance);

    persistChat(inst);

    console.log("ChatPanel.sendMessage");
    let assistantMessage: ChatMessage | undefined;
    const renderPending = () => {
      const message = assistantMessage;
      if (!message) return pendingRender;
      const answer = answerDirty ? message.content : undefined;
      const reasoning = reasoningDirty ? reasoningTextRaw : undefined;
      answerDirty = false;
      reasoningDirty = false;
      pendingRender = pendingRender.then(async () => {
        if (answer !== undefined) message._html = normalizeHeaders(await renderMarkdown(answer));
        if (reasoning !== undefined) message._reasoningText = normalizeHeaders(await renderMarkdown(reasoning));
        $messages = $messages;
        await tick();
        if (liveReasoningDiv && followLiveReasoning) liveReasoningDiv.scrollTop = liveReasoningDiv.scrollHeight;
        checkMessagesEndVisibility();
      });
      // Timer-triggered work is awaited again by the final flush/error path.
      void pendingRender.catch(() => {});
      return pendingRender;
    };
    const flushRender = () => {
      clearTimeout(renderTimer);
      renderTimer = undefined;
      return renderPending();
    };
    const scheduleRender = () => {
      if (renderTimer === undefined)
        renderTimer = setTimeout(() => {
          renderTimer = undefined;
          void renderPending();
        }, 75);
    };
    try {
      const messagesToSend = $messages.map((m) => ({
        role: m.role,
        content: m.content,
        replay_id: m.replay?.content === m.content ? m.replay.id : undefined,
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
          session_id: requestSessionId,
          response_id: responseId,
          messages: messagesToSend,
          attachments,
          sourceids,
          targetapi: request.targetapi,
          temperature: $temperature,
          reasoning: request.reasoning,
          ctxratio: $contextSizeRatio,
          attachedonly: attachedFilesOnly,
          free_chat: $bFreeChat,
        }),
      });
      if (!response.ok) {
        throw new Error("Failed to send message");
      }
      let leftover = ""; // holds an incomplete trailing SSE frame across reads
      const reader = response.body?.getReader();
      const decoder = new TextDecoder();
      while (reader) {
        const { done, value } = await reader.read();
        if (done) break;

        let bufferToParse = leftover + decoder.decode(value, { stream: true });
        leftover = "";
        while (bufferToParse) {
          const { parsed: chunk, remainder, reasoning, meta = [] } = parseFromSSE(bufferToParse);
          if (remainder.length === bufferToParse.length) {
            // no complete "\n\n" event in here — stash and wait for more data
            leftover = remainder;
            break;
          }
          bufferToParse = remainder;

          if (reasoning) {
            reasoningTextRaw += reasoning;
            reasoningDirty = true;
          }
          if (meta.length) metaInfoArray = [...metaInfoArray, ...meta];
          const firstFrame = !assistantMessage && !!(chunk || reasoning);
          if (firstFrame) {
            assistantMessage = { role: "assistant", content: "", _html: "" };
            activeAssistantIndex = $messages.length;
            $messages = [...$messages, assistantMessage];
            started = true;
          }
          if (!assistantMessage) continue;
          const hadAnswer = !!assistantMessage.content.trim();
          assistantMessage.content += chunk;
          if (chunk) answerDirty = true;
          const firstAnswer = !hadAnswer && !!assistantMessage.content.trim();
          if (firstAnswer && !reasoningUserChanged) liveReasoningOpen = false;
          assistantMessage._metaInfoArray = [...metaInfoArray];
          if (firstFrame || firstAnswer) await flushRender();
          else if (chunk || reasoning) scheduleRender();
          $messages = $messages;
          tick().then(checkMessagesEndVisibility);
        }
      }
      await flushRender();
      if (assistantMessage) {
        assistantMessage.content = stripModelEchoedSources(processResponse(assistantMessage.content));
        assistantMessage._html = normalizeHeaders(await renderMarkdown(assistantMessage.content));
        assistantMessage._metaInfoArray = [...metaInfoArray];
        assistantMessage._reasoningText = normalizeHeaders(await renderMarkdown(reasoningTextRaw));
        if (receivedReplayId === responseId)
          assistantMessage.replay = { id: responseId, session: requestSessionId, content: assistantMessage.content };
        $messages = $messages;
      }
    } catch (error) {
      clearTimeout(renderTimer);
      await pendingRender.catch(() => {});
      clog("Error sending message:", error);
      if (!assistantMessage) {
        assistantMessage = { role: "assistant", content: "", _html: "" };
        $messages = [...$messages, assistantMessage];
      }
      assistantMessage.content +=
        (assistantMessage.content ? "\n\n" : "") + "Sorry, there was an error processing your request.";
      assistantMessage._html = normalizeHeaders(await renderMarkdown(assistantMessage.content));
      assistantMessage._metaInfoArray = [...metaInfoArray];
      assistantMessage._reasoningText = normalizeHeaders(await renderMarkdown(reasoningTextRaw));
      $messages = $messages;
    } finally {
      clearTimeout(renderTimer);
      if (assistantMessage) {
        assistantMessage._reasoningVisible = reasoningUserChanged && liveReasoningOpen;
        if (assistantMessage._reasoningVisible) assistantMessage._metaVisible = true;
        $messages = $messages;
      }
      resetUi();
      setTimeout(() => {
        initHljs();
      }, 250);

      persistChat(inst);
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
          if ($messages[index].content !== newContent) {
            $messages = $messages.map((message, i) => (i > index ? { ...message, replay: undefined } : message));
          }
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

  function initHljs() {
    if (window.HLJS_CUSTOM && window.HLJS_CUSTOM.initHljs) {
      window.HLJS_CUSTOM.initHljs();
    }
  }
</script>

<div class="chat-panel p-3 w-full h-full flex flex-col space-y-8 overflow-y-auto">
  <div class="flex flex-col space-y-6 mb-4 grow p-4" id="chat-messages">
    {#if $messages.length === 0}
      <p class="text text-center text-surface-500">No messages yet. Start the conversation!</p>
    {/if}
    {#each $messages as msg, i}
      {@const isActive = $bLoading && i === activeAssistantIndex}
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
              class="btn btn-sm px-1 preset-filled-surface-50-950 cursor-pointer"
              onclick={() => onCopyMsg(msg.content)}
              title="Copy to clipboard"
            >
              <icons.Copy />
            </button>
            <button
              type="button"
              class="btn btn-sm px-1 preset-filled-surface-50-950 cursor-pointer"
              onclick={() => onEditMsg(i)}
              disabled={$bLoading}
              title="Edit your original question"
            >
              <icons.SquarePen />
            </button>
          </div>
        </div>
      {:else}
        <div class="flex flex-col overflow-y-hidden box-border pb-4 space-y-1 message" data-role="assistant">
          <div class="flex items-start justify-end">
            {#if isActive || isGoodArray(msg._metaInfoArray) || msg._reasoningText}
              <div class="text-xs flex flex-col space-y-0 self-end text-right min-w-0 max-w-full">
                <button
                  type="button"
                  class="btn btn-sm text-surface-500 border-surface-500 flex text-right justify-end"
                  onclick={() => ($messages[i]._metaVisible = !$messages[i]._metaVisible)}
                  aria-expanded={!!msg._metaVisible}
                >
                  <span class="text-xs">{isActive ? "Working…" : "Ready"}</span>
                  {#if msg._metaVisible}
                    <icons.ChevronUp />
                  {:else}
                    <icons.ChevronDown />
                  {/if}
                </button>
                {#if msg._metaVisible}
                  <div class="flex flex-col" transition:slide>
                    {#each [...(msg._metaInfoArray ?? [])].reverse() as info}
                      <span>{info} ✓</span>
                    {/each}
                  </div>
                  <div class="text-xs flex flex-col space-y-0 self-end text-right">
                    {#if !isActive && msg._reasoningText}
                      <button
                        type="button"
                        class="btn btn-sm text-surface-500 border-surface-500 flex text-right justify-end"
                        onclick={() => ($messages[i]._reasoningVisible = !$messages[i]._reasoningVisible)}
                        aria-expanded={!!msg._reasoningVisible}
                      >
                        <span class="text-xs">Reasoning</span>
                        {#if msg._reasoningVisible}
                          <icons.ChevronUp />
                        {:else}
                          <icons.ChevronDown />
                        {/if}
                      </button>
                      {#if msg._reasoningVisible}
                        <div
                          class="max-h-36 overflow-y-auto text-left whitespace-normal wrap-break-word text-surface-500"
                          role="region"
                          aria-label="Reasoning content"
                          transition:slide
                        >
                          <div class="text-xs">
                            {@html DOMPurify.sanitize(msg._reasoningText)}
                          </div>
                        </div>
                      {/if}
                    {/if}
                  </div>
                {/if}
              </div>
            {/if}
          </div>
          {#if isActive}
            <span role="status" class="text-xs text-right text-surface-500">{metaInfo || "Working…"}</span>
            {#if msg._reasoningText}
              <div class="flex flex-col min-w-0 max-w-full">
                <button
                  type="button"
                  class="btn btn-sm self-end text-surface-500"
                  onclick={toggleLiveReasoning}
                  aria-expanded={liveReasoningOpen}
                >
                  <span class="text-xs">{msg.content.trim() ? "Reasoning" : "Reasoning…"}</span>
                  {#if liveReasoningOpen}<icons.ChevronUp />{:else}<icons.ChevronDown />{/if}
                </button>
                {#if liveReasoningOpen}
                  <div
                    bind:this={liveReasoningDiv}
                    onscroll={trackReasoningScroll}
                    class="max-h-36 overflow-y-auto text-left whitespace-normal wrap-break-word text-surface-500 text-xs"
                    role="region"
                    aria-label="Reasoning content"
                  >
                    {@html DOMPurify.sanitize(msg._reasoningText)}
                  </div>
                {/if}
              </div>
            {/if}
          {/if}
          {#if msg.content.trim()}
            <div
              class="text-sm border2 border-surface-100-900 bg-surface-500/5 shadow2 rounded-xl whitespace-normal p-4 pb-1 break-normal text-left message-content"
            >
              {#if msg._html}
                {@html DOMPurify.sanitize(msg._html, {
                  ADD_ATTR: ["onclick"],
                })}
              {:else}
                {msg.content}
              {/if}
            </div>
          {/if}
          {#if msg.content.trim() || !isActive}
            <div class="flex items-center">
              {#if msg.content.trim()}
                <button
                  type="button"
                  class="btn btn-sm px-2 preset-filled-surface-50-950 cursor-pointer"
                  onclick={() => onCopyMsg(msg.content)}
                  disabled={$bLoading}
                  title="Copy to clipboard"
                >
                  <icons.Copy />
                </button>
              {/if}
              <!-- Feedbacks are temporarily off-->
              <!-- <button
                type="button"
                class="btn btn-sm px-1"
                onclick={() => onThumbsFeedback(i, "good")}
                disabled={$bLoading}
                title="Good answer"
              >
                <icons.ThumbsUp />
              </button>
              <button
                type="button"
                class="btn btn-sm px-1"
                onclick={() => onThumbsFeedback(i, "bad")}
                disabled={$bLoading}
                title="Incorrect or unhelpful answer"
              >
                <icons.ThumbsDown />
              </button> -->
              {#if msg.content.trim()}<span class="vr mx-1 h-4"></span>{/if}
              <button
                type="button"
                class="btn btn-sm px-1 preset-filled-surface-50-950 cursor-pointer"
                onclick={() => onRetry(i)}
                disabled={$bLoading}
                title="Retry the answer"
              >
                <icons.RefreshCw />
              </button>
            </div>
          {/if}
        </div>
      {/if}
    {/each}
    {#if $bLoading && !started}
      <div role="status" class="italic text-right text-surface-500 text-sm">
        {metaInfo || "Thinking..."}
      </div>
    {/if}
    <div class="min-h-16"></div>
    <div bind:this={messagesEndDiv}></div>
  </div>

  <div class="sticky bottom-0 flex items-end pb-0 pt-4 gradient-to-t from-surface-50-950">
    {#if showScrollBtn}
      <div class="absolute -top-6 w-full flex" transition:fade>
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
    <InputArea bind:this={inputArea} {onSendMessage} bind:sourceids bind:attachments loading={$bLoading} />

    {#if !$bFreeChat}
      <div
        class="flex items-center absolute left-4 bottom-0 z-50 bg-surface-50-950 px-2 rounded gap-1 translate-y-1/3"
        style:max-width={hasReasoning ? "calc(100% - 7.5rem)" : undefined}
        title="If on, only files attached to the message will be used as context"
      >
        {#if hasAttachedFiles}
          <input
            type="checkbox"
            class="shrink-0"
            id="checkbox-attached-files-only"
            checked={attachedFilesOnly && hasAttachedFiles}
            disabled={!hasAttachedFiles}
            onchange={(ev) => {
              attachedFilesOnly = (ev.target as HTMLInputElement)?.checked;
            }}
          />
          <label
            class="min-w-0 text-xs {hasAttachedFiles ? '' : 'text-surface-500'}"
            for="checkbox-attached-files-only"
          >
            Use attached files only
          </label>
        {:else}
          <span class="text-xs text-surface-500">No attached files</span>
        {/if}
      </div>
    {/if}

    {#if hasReasoning}
      <div class="absolute right-4 bottom-0 z-50 translate-y-1/3">
        {#key JSON.stringify([reasoningProvider?.id, reasoningProvider?.reasoning])}
          <ReasoningPicker provider={reasoningProvider} disabled={$bLoading || !$reasoningReady} />
        {/key}
      </div>
    {/if}
  </div>
</div>
