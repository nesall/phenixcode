<script lang="ts">
  import * as icons from "@lucide/svelte";
  import { onMount } from "svelte";
  import { initResizeObserver, isPtInRect, nextRandomId } from "../utils";
  import { autoUpdate, computePosition, flip, offset, shift } from "@floating-ui/dom";
  import { fade } from "svelte/transition";

  interface ValueObject {
    value: string | boolean;
    label: string;
    desc?: string;
    hint?: string;
    group?: string;
  }

  interface Props {
    value?: string;
    values?: Array<string | ValueObject>;
    id?: string;
    classNames?: string;
    dropdownClassNames?: string;
    noButtonText?: boolean;
    disabled?: boolean;
    showGroups?: boolean;
    dropdownWidth?: "auto" | "max-content" | "min-content" | string;
    onChange?: (i: number, value: string) => void;
    onAboutToShow?: () => void;
  }

  let {
    value = $bindable(""),
    values = [],
    id = nextRandomId(4),
    classNames = "preset-outlined-surface-500",
    dropdownClassNames = "",
    noButtonText = false,
    disabled = false,
    showGroups = false,
    dropdownWidth = "",
    onChange = (i: number, value: string) => {},
    onAboutToShow = async () => {},
  }: Props = $props();

  function isBoolean(v: string | ValueObject) {
    if (typeof v === "string") return false;
    return typeof v.value === "boolean";
  }

  function valueBool(v: string | ValueObject) {
    if (typeof v === "string") return !!v;
    if (typeof v.value === "string") return !!v.value;
    return v.value;
  }

  function valueStr(v: string | ValueObject) {
    if (typeof v === "string") return v;
    return String(v.value);
  }

  function labelStr(v: string | ValueObject) {
    if (typeof v === "string") return v;
    return v.label;
  }

  function descStr(v: string | ValueObject) {
    if (typeof v === "string") return "";
    return v.desc;
  }

  function hintStr(v: string | ValueObject) {
    if (typeof v === "string") return "";
    return v.hint;
  }

  function groupStr(v: string | ValueObject) {
    if (typeof v === "string") return "";
    return v.group;
  }

  let delAutoUpdate: (() => void) | undefined;
  function recomputePosition() {
    if (!elemAnchor || !elemFloat) return;
    delAutoUpdate = autoUpdate(elemAnchor, elemFloat, () => {
      if (elemFloat && elemAnchor)
        computePosition(elemAnchor, elemFloat, {
          strategy: "fixed",
          placement: "bottom",
          middleware: [offset(4), flip(), shift()],
        }).then(({ x, y }) => {
          if (elemFloat)
            Object.assign(elemFloat.style, {
              left: `${x}px`,
              top: `${y}px`,
            });
        });
    });
  }

  onMount(() => {
    if (elemAnchor)
      initResizeObserver(elemAnchor, () => {
        if (delAutoUpdate) delAutoUpdate();
        if (elemAnchor) {
          if (0 < elemAnchor.offsetWidth) {
            // console.log("Dropdown resize observed, recomputing position", elemAnchor.offsetWidth);
            anchorWidth = elemAnchor.offsetWidth;
          }
          recomputePosition();
        }
      });
    return () => {
      if (delAutoUpdate) delAutoUpdate();
    };
  });

  $effect(() => {
    if (show && elemFloat && elemAnchor) {
      // Wait for DOM to settle
      requestAnimationFrame(() => {
        recomputePosition();
      });
    } else if (!show && delAutoUpdate) {
      delAutoUpdate();
      delAutoUpdate = undefined;
    }
  });

  const currentLabel = $derived.by(() => {
    for (let v of values) {
      if (valueStr(v) === value) {
        return labelStr(v);
      }
    }
    return value;
  });

  const currentDesc = $derived.by(() => {
    for (let v of values) {
      if (valueStr(v) === value) {
        return descStr(v);
      }
    }
    return "";
  });

  let show = $state(false);

  let currentHint: string | undefined = $state();

  let elemFloat: HTMLElement | undefined = $state();
  let elemAnchor: HTMLElement | undefined = $state();
  let anchorWidth = $state(10);

  const ddWidth = $derived(dropdownWidth ? dropdownWidth : !elemAnchor || noButtonText ? "auto" : `${anchorWidth}px`);

  function onItemClick(i: number, v: string) {
    value = v;
    currentHint = hintStr(values[i]);
    onChange(i, v);
    setTimeout(() => (show = false), 100);
  }

  function onItemToggle(i: number, ev: Event) {
    const elem = ev.target as HTMLInputElement;
    let checked = false;
    if (elem) checked = elem.checked;
    onChange(i, String(checked));
  }

  async function onClick() {
    if (!show) await Promise.resolve(onAboutToShow());
    show = !show;
  }

  function windowPressed(e: MouseEvent) {
    if (
      elemFloat &&
      elemAnchor &&
      !isPtInRect(elemFloat.getBoundingClientRect(), e.clientX, e.clientY) &&
      !isPtInRect(elemAnchor.getBoundingClientRect(), e.clientX, e.clientY)
    ) {
      show = false;
    }
  }
  function windowKeyDown(e: KeyboardEvent) {
    switch (e.key) {
      case "Escape":
        show = false;
        break;
    }
  }

  // function printLastGroup(lastGroup: string | undefined | null, item: string | ValueObject) {
  //   lastGroupLabel = groupStr(item);
  //   return lastGroupLabel;
  // }
</script>

<svelte:window onmousedown={windowPressed} onkeydown={windowKeyDown} />

<div
  class="combobox relative inline"
  role="combobox"
  aria-expanded={show}
  aria-controls="{id}-combobox"
  aria-haspopup="listbox"
  aria-label="Select an option"
  bind:this={elemAnchor}
>
  <button
    type="button"
    class="combobox-btn w-full flex items-center rounded px-2 py-1 {classNames}"
    {disabled}
    onclick={onClick}
    title={currentHint}
  >
    {#if !noButtonText}
      <div class="flex-1 text-left items-center flex space-x-4">
        <span class="text-left">
          {currentLabel}
        </span>
        {#if currentDesc}
          <span class="text-surface-500">
            {currentDesc}
          </span>
        {/if}
      </div>
    {/if}
    {#if show}
      <icons.ChevronUp class="size-elem-base"/>
    {:else}
      <icons.ChevronDown class="size-elem-base" />
    {/if}
  </button>
  {#if show}
    <div
      id="{id}-dropdown-list"
      style="position: fixed; width: {ddWidth}; z-index: 1000;"
      class="dropdown-list rounded shadow flex flex-col max-h-48 overflow-y-auto absolute2 {dropdownClassNames}"
      role="listbox"
      bind:this={elemFloat}
      transition:fade={{ duration: 100 }}
    >
      {#each values as item, i (valueStr(item) + labelStr(item))}
        {#if showGroups && (i == 0 || groupStr(item) !== groupStr(values[i - 1]))}
          <div class="bg-surface-100-900 text-left p-1">{groupStr(item)}</div>
        {/if}
        {#if isBoolean(item)}
          <div
            class="bg-surface-50-950 hover:bg-surface-100-900 text-left px-3 py-1 w-full
            flex items-center gap-2"
            title={hintStr(item)}
          >
            <input
              type="checkbox"
              id={`${id}-checkbox-item${i}`}
              data-value={valueStr(item)}
              class="bg-surface-50-950 hover:bg-surface-100-900 text-left
              flex items-center justify-between"
              role="option"
              aria-selected={valueBool(item)}
              checked={valueBool(item) === true}
              onchange={(ev) => onItemToggle(i, ev)}
            />
            <label for={`${id}-checkbox-item${i}`} class="flex-1">
              {labelStr(item)}
            </label>
          </div>
        {:else}
          <button
            data-value={valueStr(item)}
            class="bg-surface-50-950 hover:bg-primary-50-950 text-left px-3 py-1 w-full
                flex items-center gap-2 justify-between
                {valueStr(item) === value ? 'font-bold' : ''}"
            role="option"
            aria-selected={valueStr(item) === value}
            onclick={() => onItemClick(i, valueStr(item))}
            title={hintStr(item)}
          >
            {#if valueStr(item) === value}✓
            {/if}
            {labelStr(item)}
            {#if descStr(item)}
              <span class="text-surface-500 text-right">
                {descStr(item)}
              </span>
            {/if}
          </button>
        {/if}
      {/each}
    </div>
  {/if}
</div>
