<script lang="ts">
  import { Dialog, Portal } from "@skeletonlabs/skeleton-svelte";
  import { onDestroy } from "svelte";
  import { deleteChat, listChats, restoreChat, toaster } from "../utils";
  import { bLoading, instances } from "../store";
  import * as icons from "@lucide/svelte";

  let {
    open = $bindable(false),
    onOpen,
    onDeleted,
  }: {
    open: boolean;
    onOpen: (id: string) => Promise<void>;
    onDeleted: (id: string) => void;
  } = $props();

  let view = $state<"active" | "trash">("active");
  let chats = $state<ChatSummary[]>([]);
  let expanded = $state("");
  let busy = $state("");
  let loading = $state(false);
  let loadError = $state("");
  let placeholders = $state<Record<string, { chat: ChatSummary; height: number }>>({});
  const timers = new Map<string, ReturnType<typeof setTimeout>>();
  let request = 0;
  let ignoreExpansionUntil = 0;

  $effect(() => {
    if (open) void load("active");
  });

  onDestroy(() => {
    ++request;
    for (const timer of timers.values()) clearTimeout(timer);
  });

  async function load(next: "active" | "trash") {
    const token = ++request;
    view = next;
    ignoreExpansionUntil = 0;
    expanded = "";
    loading = true;
    loadError = "";
    try {
      const res = await listChats("", next === "trash");
      if (token !== request) return;
      if (res.status !== "success") throw new Error(res.message);
      const rows = res.chats || [];
      // Keep tombstones when switching back to Active during the Undo window.
      chats =
        next === "active"
          ? [...rows.filter((c) => !placeholders[c.id]), ...Object.values(placeholders).map((p) => p.chat)].sort(
              (a, b) => b.updated_at - a.updated_at || a.id.localeCompare(b.id),
            )
          : rows;
    } catch (e) {
      if (token === request) loadError = String(e);
    } finally {
      if (token === request) loading = false;
    }
  }

  function projectName(id: string) {
    return $instances.find((i) => i.project_id === id)?.name || id || "No project";
  }

  async function remove(chat: ChatSummary, event: MouseEvent) {
    event.stopPropagation();
    if (event.detail > 1 || busy || placeholders[chat.id] || $bLoading) return;
    const height = (event.currentTarget as HTMLElement).closest("article")!.getBoundingClientRect().height;
    busy = chat.id; // Lock synchronously, before the bridge resolves.
    ignoreExpansionUntil = Date.now() + 500;
    ++request;
    try {
      const res = await deleteChat(chat.id);
      if (res.status !== "success") throw new Error(res.message);
      placeholders[chat.id] = { chat, height };
      expanded = "";
      onDeleted(chat.id);
      timers.set(
        chat.id,
        setTimeout(() => expire(chat.id), 5000),
      );
    } catch (e) {
      toaster.error({ title: "Failed to delete chat.", description: String(e) });
    } finally {
      busy = "";
    }
  }

  function expire(id: string) {
    // Another tombstone may expire during a click sequence elsewhere in the list.
    ignoreExpansionUntil = Date.now() + 500;
    delete placeholders[id];
    timers.delete(id);
    if (view === "active") chats = chats.filter((c) => c.id !== id);
  }

  async function restore(id: string, event: MouseEvent) {
    event.stopPropagation();
    if (event.detail > 1 || busy) return;
    busy = id;
    ++request;
    const placeholder = placeholders[id];
    clearTimeout(timers.get(id));
    timers.delete(id);
    try {
      const res = await restoreChat(id);
      if (res.status !== "success") throw new Error(res.message);
      delete placeholders[id];
      ignoreExpansionUntil = 0;
      expanded = "";
      if (view === "trash") chats = chats.filter((c) => c.id !== id);
    } catch (e) {
      toaster.error({ title: "Failed to restore chat.", description: String(e) });
      if (placeholder)
        timers.set(
          id,
          setTimeout(() => expire(id), 5000),
        );
    } finally {
      busy = "";
    }
  }

  async function select(id: string, event: MouseEvent) {
    event.stopPropagation();
    if (event.detail > 1 || busy || $bLoading) return;
    busy = id;
    try {
      await onOpen(id);
    } finally {
      busy = "";
    }
  }
</script>

<Dialog {open} onOpenChange={(e) => (open = e.open)}>
  <Portal>
    <Dialog.Backdrop />
    <Dialog.Positioner class="fixed inset-0 z-50 flex justify-center items-center">
      <Dialog.Content class="card bg-surface-100-900 w-xl max-w-[95vw] p-4 space-y-3 shadow-xl">
        <Dialog.Title>
          <div class="flex flex-row items-center justify-between">
            <span class="text-lg font-bold">Chat History</span>
            <div class="flex gap-2 justify-end" role="group" aria-label="Chat history view">
              <button
                type="button"
                class="btn {view === 'active' ? 'preset-filled' : 'preset-tonal'}"
                aria-pressed={view === "active"}
                disabled={!!busy}
                onclick={() => load("active")}
              >
                Active
              </button>
              <button
                type="button"
                class="btn {view === 'trash' ? 'preset-filled' : 'preset-tonal'}"
                aria-pressed={view === "trash"}
                disabled={!!busy}
                onclick={() => load("trash")}
              >
                Trash
              </button>
            </div>
          </div>
        </Dialog.Title>
        <Dialog.Description class="text-sm text-surface-500"></Dialog.Description>
        <hr class="hr" />
        <div class="text-xs max-h-[60vh] overflow-x-hidden overflow-y-auto" aria-busy={loading}>
          {#if loading}
            <p class="text-center py-4">Loading chats…</p>
          {:else if loadError}
            <p role="alert" class="py-4">{loadError}</p>
            <button type="button" class="btn preset-tonal" onclick={() => load(view)}>Retry</button>
          {:else if chats.length === 0}
            <p class="text-surface-500 text-center py-4">{view === "trash" ? "Trash is empty" : "No chats yet"}</p>
          {:else}
            {#each chats as chat (chat.id)}
              <article
                data-chat-id={chat.id}
                class="border-b border-surface-300-700 even:bg-surface-200-800/25"
                style:min-height={view === "active" && placeholders[chat.id]
                  ? `${placeholders[chat.id].height}px`
                  : undefined}
              >
                {#if view === "active" && placeholders[chat.id]}
                  <div class="flex items-center justify-between gap-3 p-3" role="status">
                    <span class="text-surface-500">Chat moved to Trash</span>
                    <button
                      type="button"
                      class="btn preset-tonal"
                      disabled={!!busy}
                      onclick={(e) => restore(chat.id, e)}
                    >
                      <icons.Undo /> Undo
                    </button>
                  </div>
                {:else}
                  <button
                    type="button"
                    class="btn w-full min-w-0 text-left justify-start p-3"
                    aria-expanded={expanded === chat.id}
                    aria-controls={`chat-details-${chat.id}`}
                    disabled={!!busy}
                    onclick={(e) => {
                      e.stopPropagation();
                      if (e.detail <= 1 && Date.now() >= ignoreExpansionUntil)
                        expanded = expanded === chat.id ? "" : chat.id;
                    }}
                  >
                    <span class="flex flex-col min-w-0 w-full gap-1">
                      <span class="truncate text-sm">{chat.title || "Untitled chat"}</span>
                      <span class="text-surface-500 text-xs truncate">
                        {projectName(chat.project_id)} · {new Date(chat.updated_at * 1000).toLocaleString()}
                      </span>
                    </span>
                  </button>
                  {#if expanded === chat.id}
                    {@const [first, ...rest] = (chat.preview || "No user message to preview.").split("\n")}
                    <div id={`chat-details-${chat.id}`} class="px-3 pb-3 space-y-3">
                      {#if chat.deleted_at != null}
                        <p class="text-surface-500">
                          Deleted {new Date(chat.deleted_at * 1000).toLocaleString()}
                        </p>
                      {/if}
                      <p class="text-xs">
                        <span class="block">
                          <span class="text-gray-500 pr-1">You:</span>{first}
                        </span>
                        {#if rest.length}
                          <span class="block">
                            <span class="text-gray-500 pr-1">Assistant:</span>{rest.join("\n").trim()}
                          </span>
                        {/if}
                      </p>
                      <div class="flex gap-2">
                        {#if view === "active"}
                          <button
                            type="button"
                            class="btn preset-filled text-xs h-7"
                            disabled={!!busy || $bLoading}
                            onclick={(e) => select(chat.id, e)}
                          >
                            <icons.MessageSquareText class="size-4" /> Open
                          </button>
                        {:else}
                          <button
                            type="button"
                            class="btn preset-filled text-xs h-7"
                            disabled={!!busy}
                            onclick={(e) => restore(chat.id, e)}
                          >
                            <icons.Undo2 class="size-4" /> Restore
                          </button>
                        {/if}
                        <button
                          type="button"
                          class="btn preset-tonal text-xs h-7"
                          disabled={!!busy}
                          onclick={(e) => {
                            e.stopPropagation();
                            if (e.detail <= 1) expanded = "";
                          }}
                        >
                          <icons.X class="size-4" /> Close
                        </button>
                        {#if view === "active"}
                          <button
                            type="button"
                            class="btn preset-tonal-error ml-auto text-xs h-7"
                            disabled={!!busy || $bLoading}
                            onclick={(e) => remove(chat, e)}
                          >
                            <icons.Trash class="size-4" /> Delete
                          </button>
                        {/if}
                      </div>
                    </div>
                  {/if}
                {/if}
              </article>
            {/each}
          {/if}
        </div>
        <Dialog.CloseTrigger class="btn preset-filled w-full">Close</Dialog.CloseTrigger>
      </Dialog.Content>
    </Dialog.Positioner>
  </Portal>
</Dialog>
