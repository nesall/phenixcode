<script lang="ts">
  import * as icons from "@lucide/svelte";
  import Checkbox from "./Checkbox.svelte";
  import { Dialog, Portal } from "@skeletonlabs/skeleton-svelte";
  import { onMount } from "svelte";
  import { apiUrl, bytesToSize, clog, Consts, hash, stripCommonPrefix } from "../utils";
  import { fade, fly } from "svelte/transition";
  // import Dropdown from "./Dropdown.svelte";

  interface Props {
    loading: boolean;
    onChange: (files: string[]) => void;
  }
  let { loading = false, onChange }: Props = $props();

  let openState = $state(false);
  let filterValue = $state("");

  interface Document {
    path: string;
    size: number;
    lastModified: number;
    _visible?: boolean;
    _name: string;
    _strippedPath: string;
    _checked: boolean;
  }
  let documents: Document[] = $state([]);

  const filteredDocs = $derived(documents.filter((d) => d.path.includes(filterValue)));

  onMount(() => {
    fetchFiles();
  });

  function fetchFiles() {
    const saved = JSON.parse(sessionStorage.getItem(Consts.ContextFilesKey) || "[]");
    fetch(apiUrl("/api/documents"))
      .then((response) => response.json())
      .then((data) => {
        const paths = data.map((d: any) => d.path);
        const strippedPaths = stripCommonPrefix(paths);

        documents = data.map((doc: any, i: number) => ({
          ...doc,
          _name: doc.path.split("/").pop() || doc.path,
          _strippedPath: strippedPaths[i],
          _visible: saved.find((d: any) => d.path === doc.path)?._visible || false,
          _checked: saved.find((d: any) => d.path === doc.path)?._checked || false,
        }));
        onChange(documents.filter((d) => d._checked).map((d) => d.path));
      })
      .catch((error) => {
        clog("Error fetching context files:", error);
      });
  }

  function saveToSession() {
    sessionStorage.setItem(
      "contextFiles",
      JSON.stringify(
        documents.map((d) => ({
          path: d.path,
          _visible: d._visible,
          _checked: d._checked,
        })),
      ),
    );
  }

  function onModalOpen() {
    fetchFiles();
    openState = true;
  }

  async function modalClose() {
    openState = false;
    onChange(documents.filter((d) => d._checked).map((d) => d.path));
    saveToSession();
  }

  function onClose(path: string) {
    const i = documents.findIndex((d) => d.path === path);
    if (i === -1) return;
    documents[i]._visible = false;
    documents[i]._checked = false;
    documents = documents;
    onChange(documents.filter((d) => d._checked).map((d) => d.path));
    saveToSession();
  }

  function onToggle(path: string, b: boolean) {
    const i = documents.findIndex((d) => d.path === path);
    if (i === -1) return;
    documents[i]._checked = b;
    onChange(documents.filter((d) => d._checked).map((d) => d.path));
    saveToSession();
  }
</script>

<div class="space-x-1 w-full flex flex-wrap gap-0.5 mb-0">
  <div class="flex items-center space-x-0">
    <button
      type="button"
      class="btn btn-sm flex items-center space-x-1 preset-filled-secondary-500 px-1 pr-2 h-5 rounded-r-none-dummy"
      disabled={loading}
      title="Explicitly insert files as context"
      onclick={onModalOpen}
    >
      <icons.Plus size={16} />Add context
    </button>
    <!-- <div
      class="text-sm relative"
      title={useUserFilesOnly
        ? "Using attached files only"
        : "Toggle on to use attached files only, otherwise all relevant context files may be used"}
    >
      {#if useUserFilesOnly}
        <span class="absolute top-2 right-0 z-50"> ✓ </span>
      {/if}
      <Dropdown
        classNames="btn btn-sm h-5 preset-filled-secondary-500 pl-1 pr-1 rounded-l-none border-l-1 border-secondary-300-700"
        noButtonText={true}
        disabled={loading}
        values={[
          {
            value: useUserFilesOnly,
            label: "Use attached files only",
            hint: "If selected, only files attached to the message will be used as context",
          },
        ]}
        value=""
        onAboutToShow={async () => {}}
        onChange={(i, v) => {
          console.log(v);
          useUserFilesOnly = v === "true";
        }}
      />
    </div> -->
  </div>
  {#each documents.filter((d) => d._visible) as doc, i (doc.path)}
    <div transition:fly={{ y: 10, duration: 150 }} class="relative2">
      <Checkbox
        name={doc._name}
        checked={doc._checked}
        title={doc.path}
        {loading}
        id={String(hash(doc.path))}
        onToggle={(b: boolean) => onToggle(doc.path, b)}
        onClose={() => onClose(doc.path)}
      />
    </div>
  {/each}
</div>

<Dialog open={openState} onOpenChange={(e) => (openState = e.open)}>
  <Portal>
    <Dialog.Positioner class="fixed inset-0 z-50 flex justify-center items-center">
      <Dialog.Content class="card bg-surface-100-900 w-lg p-4 space-y-2 shadow-xl">
        <Dialog.Title class="text-lg font-bold">Available Context Files</Dialog.Title>
        <hr class="hr" />
        <Dialog.Description>
          {#if documents.length === 0}
            <p>No context files available.</p>
          {:else}
            <div class="whitespace-wrap text-sm mb-4">
              Selected documents will be explicitly included in the context.
            </div>

            <input type="text" class="input text-sm my-1" placeholder="Type to filter" bind:value={filterValue} />
            <div
              class="h-96 max-h-96 overflow-auto scrollbar-hide
                  p-4 border border-surface-200-800 rounded text-xs"
            >
              {#each filteredDocs as doc, i (doc.path)}
                <div
                  class="hover:bg-surface-200-800 odd:bg-surface-100-900 px-2
              flex items-center space-x-2 border-b border-surface-200-800
              {doc._visible ? 'font-bold' : ''}"
                >
                  <input
                    type="checkbox"
                    class="checkbox w-4 h-4 p-0 m-0 accent-primary-500"
                    id={`document-checkbox-${i}`}
                    bind:checked={filteredDocs[i]._visible}
                    onchange={(e: Event) => {
                      doc._checked = ((e as InputEvent).target as HTMLInputElement)?.checked;
                    }}
                    disabled={loading}
                  />
                  <label
                    class="p-2"
                    for={`document-checkbox-${i}`}
                    title={bytesToSize(doc.size) +
                      ", Last modified " +
                      new Date(doc.lastModified * 1000).toLocaleString()}
                  >
                    {doc._strippedPath}
                  </label>
                </div>
              {/each}
            </div>
          {/if}
          <div class="my-4 flex justify-end space-x-2 text-sm">
            <span>Selected documents:</span>
            <span class="font-medium">
              {documents.filter((d) => d._visible).length}/{documents.length}
            </span>
          </div>
        </Dialog.Description>
        <Dialog.CloseTrigger class="btn preset-filled w-full" onclick={modalClose}>Finish</Dialog.CloseTrigger>
      </Dialog.Content>
    </Dialog.Positioner>
  </Portal>
</Dialog>
