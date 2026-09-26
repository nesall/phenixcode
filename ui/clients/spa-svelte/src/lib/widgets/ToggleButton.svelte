<script lang="ts">
  import type { Component } from "svelte";
  import { nextRandomId } from "../utils";

  interface Props {
    labelOn?: string;
    labelOff?: string;
    checked: boolean;
    icon?: Component<{ size?: number | string; class?: string }>;
    iconOn?: Component<{ size?: number | string; class?: string }>; // optional override when checked
    title?: string;
    onToggle?: (b: boolean) => void;
  }

  let {
    labelOn = "",
    labelOff = "",
    checked = $bindable(false),
    icon,
    iconOn,
    title = "",
    onToggle = (b: boolean) => {},
  }: Props = $props();

  let id = $state(nextRandomId(12));

  let Icon = $derived(checked ? (iconOn ?? icon) : icon);

  let hasLabel = $derived(labelOn || labelOff);

  function onClickInternal() {
    checked = !checked;
    onToggle(checked);
  }
</script>

<button
  type="button"
  aria-pressed={checked}
  onclick={onClickInternal}
  class="btn btn-sm {checked ? 'preset-filled-tertiary-500' : ''} hover:preset-tonal {hasLabel ? '' : 'btn-icon'}"
  {id}
  title={title}
>
  {#if Icon}
    <Icon />
  {/if}
  {#if hasLabel}
    {checked ? labelOn : labelOff}
  {/if}
</button>
