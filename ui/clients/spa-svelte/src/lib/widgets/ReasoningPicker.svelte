<script lang="ts">
  import { Popover, Portal } from "@skeletonlabs/skeleton-svelte";
  import { applyReasoning, reasoningParameters, reasoningSelections, selectedReasoning } from "../reasoning";

  let { provider, disabled = false }: { provider: ModelItem | undefined; disabled?: boolean } = $props();
  const id = $props.id();
  const parameters = $derived(reasoningParameters(provider));
  const applied = $derived(selectedReasoning(provider, $reasoningSelections));
  const summary = $derived(parameters.map(parameter => `${parameter.name}: ${applied[parameter.name]}`).join("; "));
  let open = $state(false);
  let draft = $state<Record<string, string>>({});

  $effect(() => {
    if (disabled) open = false;
  });

  function changeOpen(next: boolean) {
    if (next && disabled) return;
    if (next) draft = { ...applied };
    open = next;
  }

  function apply() {
    if (!disabled && provider && applyReasoning(provider, draft)) open = false;
  }
</script>

{#if parameters.length}
  <Popover {open} onOpenChange={(event) => changeOpen(event.open)} positioning={{ placement: "top-end", gutter: 8, strategy: "fixed" }}>
    <Popover.Trigger type="button" class="btn bg-surface-50-950 hover:bg-surface-100-900 focus-visible:ring-2 focus-visible:ring-primary-500 text-xs h-6 px-2" {disabled} title={summary}>
      Reasoning
    </Popover.Trigger>
    <Portal>
      <Popover.Positioner class="z-50">
        <Popover.Content class="card bg-surface-100-900 border border-surface-300-700 p-4 space-y-4 shadow-xl w-72 max-w-[calc(100vw-2rem)] max-h-[70vh] overflow-y-auto">
          <Popover.Title class="text-sm font-bold">Reasoning</Popover.Title>
          <Popover.Description class="text-xs text-surface-500">Choose a value for each parameter.</Popover.Description>
          {#each parameters as parameter, parameterIndex (parameter.name)}
            <fieldset class="space-y-2" {disabled}>
              <legend class="text-xs font-medium wrap-break-word mb-2">{parameter.name}</legend>
              {#each parameter.values as value, valueIndex (value)}
                <label class="flex items-center gap-2 text-sm cursor-pointer" for={`${id}-${parameterIndex}-${valueIndex}`}>
                  <input type="radio" class="radio accent-primary-500 shrink-0"
                    id={`${id}-${parameterIndex}-${valueIndex}`} name={`${id}-${parameterIndex}`} {value}
                    checked={draft[parameter.name] === value} onchange={() => (draft[parameter.name] = value)} />
                  <span class="min-w-0 wrap-break-word">{value}</span>
                </label>
              {/each}
            </fieldset>
          {/each}
          <div class="flex justify-end">
            <button type="button" class="btn preset-filled text-xs" {disabled} onclick={apply}>Apply</button>
          </div>
        </Popover.Content>
      </Popover.Positioner>
    </Portal>
  </Popover>
{/if}
