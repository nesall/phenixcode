<script lang="ts">
  import { untrack } from "svelte";
  import * as icons from "@lucide/svelte";
  import type { DatabasePage, DatabaseChunks, DatabaseFile, DatabaseSource, DatabaseResult } from "../../../../app";
  import { projectStore } from "../../../store.svelte";
  import {
    helper_listDatabaseSources,
    helper_selectDatabaseDirectory,
    helper_databaseChunks,
    helper_databaseFile,
    helper_openDatabaseDirectory,
  } from "../../../utils";
  import CodePreview from "./CodePreview.svelte";

  let { active = false }: { active?: boolean } = $props();
  let search = $state("");
  let offset = $state(0);
  let page = $state<DatabasePage | null>(null);
  let loading = $state(false);
  let error = $state("");
  let errorCode = $state("");
  let expanded = $state<string | null>(null);
  let view = $state<"chunks" | "file">("chunks");
  let chunks = $state<DatabaseChunks | null>(null);
  let file = $state<DatabaseFile | null>(null);
  let detailLoading = $state(false);
  let detailError = $state("");
  let autoRefresh = $state(true);
  let loadedAt = $state("");
  let request = 0;
  let detailRequest = 0;
  let projectKey = "";
  const pageSize = 50;

  function unwrap<T>(result: DatabaseResult<T>): T {
    if (result.status === "error") throw Object.assign(new Error(result.message), { code: result.code });
    return result;
  }

  function resetDetails() {
    detailRequest++;
    expanded = null;
    chunks = null;
    file = null;
    detailError = "";
    detailLoading = false;
  }

  async function load(chooseDirectory = false) {
    const project = projectStore.selected;
    if (!project || !active || loading) return;
    const id = ++request;
    const params = { settingsFilePath: project.settingsFilePath, search, offset, limit: pageSize };
    loading = true;
    try {
      const result = unwrap(
        await (chooseDirectory ? helper_selectDatabaseDirectory(params) : helper_listDatabaseSources(params)),
      );
      if (id !== request) return;
      if (page && (page.database_path !== result.database_path || page.base_directory !== result.base_directory))
        resetDetails();
      page = result;
      error = "";
      errorCode = "";
      loadedAt = new Date().toLocaleTimeString();
      if (offset !== result.offset) offset = result.offset;
      if (expanded && !result.sources.some((row) => row.source_id === expanded)) resetDetails();
    } catch (failure) {
      if (id !== request) return;
      const problem = failure as Error & { code?: string };
      if (problem.code !== "cancelled") {
        error = problem.message || String(failure);
        errorCode = problem.code ?? "error";
        page = null;
        resetDetails();
      }
    } finally {
      if (id === request) loading = false;
    }
  }

  $effect(() => {
    const project = projectStore.selected;
    const key = project ? `${project.settingsFilePath}|${project.jsonData.database.sqlite_path}` : "";
    const visible = active;
    search;
    offset;
    untrack(() => {
      request++;
      loading = false;
      if (key !== projectKey) {
        projectKey = key;
        page = null;
        error = "";
        errorCode = "";
        offset = 0;
        resetDetails();
      }
      if (!visible) resetDetails();
    });
    if (!project || !visible) return;
    const timer = setTimeout(() => void load(), 200);
    return () => {
      clearTimeout(timer);
      request++;
      detailRequest++;
      detailLoading = false;
    };
  });

  $effect(() => {
    if (!active || !autoRefresh) return;
    const timer = setInterval(() => void load(), 10000);
    return () => clearInterval(timer);
  });

  function filter(value: string) {
    search = value;
    offset = 0;
    resetDetails();
  }

  async function showDetails(row: DatabaseSource, nextView: "chunks" | "file", chunkOffset = 0) {
    const project = projectStore.selected;
    if (!project) return;
    expanded = row.source_id;
    view = nextView;
    detailError = "";
    chunks = null;
    file = null;
    detailLoading = true;
    const id = ++detailRequest;
    const params = {
      settingsFilePath: project.settingsFilePath,
      source_id: row.source_id,
      offset: chunkOffset,
      limit: 5,
    };
    try {
      if (nextView === "chunks") {
        const result = unwrap(await helper_databaseChunks(params));
        if (id === detailRequest) chunks = result;
      } else {
        const result = unwrap(await helper_databaseFile(params));
        if (id === detailRequest) file = result;
      }
    } catch (failure) {
      if (id === detailRequest) detailError = failure instanceof Error ? failure.message : String(failure);
    } finally {
      if (id === detailRequest) detailLoading = false;
    }
  }

  async function openDirectory(row: DatabaseSource) {
    const project = projectStore.selected;
    if (!project) return;
    const id = ++detailRequest;
    expanded = row.source_id;
    detailLoading = false;
    detailError = "";
    try {
      unwrap(
        await helper_openDatabaseDirectory({ settingsFilePath: project.settingsFilePath, source_id: row.source_id }),
      );
    } catch (failure) {
      if (id === detailRequest) detailError = failure instanceof Error ? failure.message : String(failure);
    }
  }

  const bytes = (value: number | null) =>
    value === null ? "—" : value < 1024 ? `${value} B` : `${(value / 1024).toFixed(1)} KiB`;
  const date = (value: number | null) => (value === null ? "—" : new Date(value * 1000).toLocaleString());
  const indexedDate = (value: string | null) =>
    value ? new Date(value.replace(" ", "T") + "Z").toLocaleString() : "—";
</script>

<section class="h-full min-h-0 min-w-0 max-w-full overflow-auto p-4 text-left space-y-3">
  <header class="flex flex-wrap items-center gap-3">
    <h2 class="text-lg font-semibold">Database</h2>
    <input
      class="input min-w-40 flex-1"
      aria-label="Filter by file path or URL"
      placeholder="Filter by file path or URL"
      maxlength="256"
      value={search}
      oninput={(event) => filter(event.currentTarget.value)}
    />
    <label class="flex items-center gap-2 text-sm">
      <input type="checkbox" class="checkbox checkbox-sm" bind:checked={autoRefresh} />
      Auto-refresh
    </label>
    <button class="btn btn-sm preset-tonal" disabled={loading} onclick={() => load()}>
      <icons.RefreshCw size={16} />Refresh
    </button>
  </header>
  <div class="flex flex-wrap items-center gap-3 text-xs text-surface-500">
    <span>{page ? `${page.total} indexed sources` : "Indexed sources"}</span>
    {#if loadedAt && page}<span>Updated {loadedAt}</span>{/if}
    <button class="btn btn-sm preset-tonal ml-auto" title="Choose the folder from which indexing was run. Relative database and source paths are resolved from this folder." disabled={loading} onclick={() => load(true)}>
      Resolve paths…
    </button>
  </div>
  {#if page}
    <div class="text-xs text-surface-500 break-all space-y-1">
      <p>Database · {page.database_path}</p>
      <p>
        Indexing base · {page.base_directory || "Not recorded. Choose it to inspect sources with relative paths."}
      </p>
    </div>
  {/if}
  {#if error}
    <div role="alert" class="rounded border border-warning-500 p-3">
      <p>{error}</p>
      {#if errorCode === "location_required" || errorCode === "not_created"}<p class="text-sm mt-1">
          For an existing database, choose the directory used to run indexing.
        </p>{/if}
    </div>
  {/if}
  {#if loading}<p role="status" class="text-sm text-surface-500">Loading indexed sources…</p>{/if}
  {#if page}
    <div class="min-w-0 max-w-full overflow-x-auto rounded border border-surface-200-800">
      <table class="w-full min-w-[40rem] table-fixed text-sm">
        <thead class="bg-surface-100-900">
          <tr>
            <th class="w-[42%] p-2 text-left">Source</th>
            <th class="w-[14%] p-2 text-right">Indexed size</th>
            <th class="w-[10%] p-2 text-right">Chunks</th>
            <th class="w-[10%] p-2 text-right">Tokens</th>
            <th class="p-2 text-left">Indexed</th>
          </tr>
        </thead>
        <tbody>
          {#each page.sources as row (row.source_id)}
            <tr class="border-t border-surface-200-800 hover:bg-surface-100-900">
              <td class="p-2">
                <button
                  class="min-w-0 w-full text-left break-all flex items-center gap-2"
                  title={row.source_id}
                  aria-expanded={expanded === row.source_id}
                  onclick={() => (expanded === row.source_id ? resetDetails() : showDetails(row, "chunks"))}
                >
                  <icons.ChevronRight
                    size={16}
                    class={expanded === row.source_id ? "rotate-90 shrink-0" : "shrink-0"}
                  />
                  {row.display_path}
                </button>
              </td>
              <td class="p-2 text-right whitespace-nowrap">{bytes(row.file_size)}</td><td class="p-2 text-right">
                {row.chunk_count}
              </td>
              <td class="p-2 text-right">{row.token_count}</td>
              <td class="p-2">{indexedDate(row.indexed_at)}</td>
            </tr>
            {#if expanded === row.source_id}
              <tr class="border-t border-surface-200-800">
                <td colspan="5" class="p-4 bg-surface-50-950 space-y-3">
                  <div class="flex flex-wrap gap-4 text-xs text-surface-500">
                    <span>Source modified · {date(row.last_modified)}</span>
                    <span>
                      Stored line count · {row.line_count ?? "—"}
                    </span>
                    <span>{row.is_url ? "URL source" : "Local file"}</span>
                  </div>
                  <div class="flex flex-wrap gap-2">
                    <button
                      class="btn btn-sm {view === 'chunks' ? 'preset-filled-primary-500' : 'preset-tonal'}"
                      onclick={() => showDetails(row, "chunks")}
                    >
                      Indexed chunks
                    </button>
                    <button
                      class="btn btn-sm {view === 'file' ? 'preset-filled-primary-500' : 'preset-tonal'}"
                      disabled={row.is_url}
                      onclick={() => showDetails(row, "file")}
                    >
                      Current file
                    </button>
                    <button class="btn btn-sm preset-tonal" disabled={row.is_url} onclick={() => openDirectory(row)}>
                      Open parent directory
                    </button>
                    <button class="btn btn-sm preset-tonal ml-auto" onclick={resetDetails}>Close</button>
                  </div>
                  {#if detailError}<p role="alert" class="text-error-600-400 text-sm">{detailError}</p>{/if}
                  {#if detailLoading}<p role="status" class="text-sm">Loading content…</p>{/if}
                  {#if view === "file" && file}
                    <p class="text-xs text-surface-500 break-all">Current file · {file.path}</p>
                    {#if file.may_have_changed}<p class="text-warning-600-400 text-sm">
                        The file's size or modification time differs from the indexed metadata.
                      </p>{:else if file.may_have_changed === null}<p class="text-sm text-surface-500">
                        Indexed file metadata is unavailable for comparison.
                      </p>{/if}
                    {#if file.truncated}<p class="text-sm text-surface-500">
                        Preview limited to the first 256 KiB.
                      </p>{/if}
                    <CodePreview content={file.content} source={row.source_id} />
                  {:else if view === "chunks" && chunks}
                    <p class="text-xs text-surface-500">
                      Stored chunks may overlap. Token totals include overlap. The index timestamp records metadata
                      writes during indexing.
                    </p>
                    {#each chunks.chunks as chunk (chunk.id)}
                      <div class="space-y-1">
                        <p class="text-xs text-surface-500">
                          Chunk {chunk.id} · {chunk.unit}
                          {chunk.start}–{chunk.end} · {chunk.type} · {chunk.tokens} tokens{chunk.truncated
                            ? " · preview truncated"
                            : ""}
                        </p>
                        <CodePreview content={chunk.content} source={row.source_id} />
                      </div>
                    {:else}
                      <p class="text-sm text-surface-500">No stored chunks for this source.</p>
                    {/each}
                    <div class="flex gap-2">
                      <button
                        class="btn btn-sm preset-tonal"
                        disabled={chunks.offset === 0}
                        onclick={() => showDetails(row, "chunks", Math.max(0, chunks!.offset - 5))}
                      >
                        Previous chunks
                      </button>
                      <button
                        class="btn btn-sm preset-tonal"
                        disabled={!chunks.has_more}
                        onclick={() => showDetails(row, "chunks", chunks!.offset + 5)}
                      >
                        Next chunks
                      </button>
                    </div>
                  {/if}
                </td>
              </tr>
            {/if}
          {:else}
            <tr>
              <td colspan="5" class="p-6 text-center text-surface-500">
                {search ? "No file paths or URLs match your filter." : "This database contains no indexed sources."}
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    <footer class="flex items-center gap-3 text-sm">
      <span>
        {page.total
          ? `${page.offset + 1}–${Math.min(page.offset + page.sources.length, page.total)} of ${page.total}`
          : "0 sources"}
      </span>
      <button
        class="btn btn-sm preset-tonal ml-auto"
        disabled={loading || page.offset === 0}
        onclick={() => {
          resetDetails();
          offset = Math.max(0, offset - pageSize);
        }}
      >
        Previous
      </button>
      <button
        class="btn btn-sm preset-tonal"
        disabled={loading || page.offset + pageSize >= page.total}
        onclick={() => {
          resetDetails();
          offset += pageSize;
        }}
      >
        Next
      </button>
    </footer>
  {/if}
</section>
