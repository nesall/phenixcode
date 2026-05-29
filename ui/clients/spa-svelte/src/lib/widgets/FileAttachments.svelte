<script lang="ts">
  import * as icons from "@lucide/svelte";

  interface Props {
    loading?: boolean;
    // onChange: (files: File[]) => void;
    attachments: File[];
  }
  let { loading = false, attachments = $bindable([]) }: Props = $props();

  // let attachments: File[] = $state([]);

  function onFileChange(e: Event) {
    const inputEl = e.target as HTMLInputElement;
    if (inputEl.files) {
      attachments = [...attachments, ...Array.from(inputEl.files)].flat();
      // if (onChange) {
      //   onChange(attachments);
      // }
      // Clear the input value to allow re-uploading the same file if needed
      inputEl.value = "";
    }
  }
</script>

<div class="flex space-x-2">
  <button
    type="button"
    class="btn btn-sm preset-filled-surface-100-900 w-5 h-5 px-0 rounded-full"
    aria-label="Attach file"
    disabled={loading}
    onclick={() => {
      document.getElementById("file-input")?.click();
    }}
    title="Attach file"
  >
    <icons.Paperclip size={12} />
  </button>
  {#if attachments.length > 0}
    <div
      class="text-xs p-0 flex space-x-2 max-w-lg overflow-x-auto scrollbar-hide"
    >
      {#each attachments as file, i (file.name)}
        <div
          class="flex items-center space-x-1 border border-surface-200-800 rounded px-1 py-0 pr-0"
          title={file.name}
        >
          <span class="max-w-20 overflow-hidden text-ellipsis">
            {file.name}
          </span>
          <button
            type="button"
            class="btn btn-sm w-4 h-4 px-0 hover:text-primary-500 2rounded-full"
            title={`Remove ${file.name}`}
            aria-label={`Remove ${file.name}`}
            onclick={() => {
              attachments = attachments.filter((_, j) => j !== i);
            }}
          >
            <icons.X size={12} />
          </button>
        </div>
      {/each}
    </div>
  {/if}
  <input
    type="file"
    id="file-input"
    class="hidden"
    accept=".txt,.md,.h,.cpp,.cc,.py,.java,.json,.xml,.csv,.log,.html,.js,.ts,.css,.scss,.rs,.go,.rb,.php"
    multiple
    onchange={onFileChange}
  />
</div>
