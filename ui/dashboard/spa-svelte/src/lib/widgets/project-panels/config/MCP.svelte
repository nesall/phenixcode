<script lang="ts">
  import * as icons from "@lucide/svelte";
  import { onDestroy, onMount, tick } from "svelte";
  import { projectStore } from "../../../store.svelte";
  import {
    helper_generateMcpCredential,
    helper_getMcpCredentialStatus,
    helper_regenerateMcpCredential,
    helper_revealMcpCredential,
    helper_saveProjectSettings,
    toaster,
  } from "../../../utils";
  import type { ProjectItem } from "../../../../app";

  interface Props {
    onChanged: (project: ProjectItem) => void;
    active?: boolean;
  }
  let { onChanged, active = true }: Props = $props();

  let portDraft = $state("8765");
  let portError = $state("");
  let copied = $state(false);

  let credentialLoading = $state(false);
  let credentialBusy = $state(false);
  let credentialState = $state<"missing" | "configured" | "unknown">("unknown");
  let credentialError = $state("");
  let credentialSuccess = $state("");
  let revealedToken = $state("");
  let tokenCopied = $state(false);
  let copyTokenTimer: ReturnType<typeof setTimeout> | undefined;
  let regenerateDialog: HTMLDialogElement | undefined = $state();

  let statusRequestId = 0;
  let lastLoadedKey = "";

  const selectedProject = $derived(projectStore.selected);
  const projectId = $derived(selectedProject?.jsonData.source.project_id?.trim() ?? "");
  const settingsFilePath = $derived(selectedProject?.settingsFilePath ?? "");
  const projectKey = $derived(selectedProject ? `${settingsFilePath}::${projectId}` : "");

  $effect(() => {
    portDraft = String(projectStore.selected?.jsonData.mcp?.port ?? 8765);
    portError = "";
  });

  function clearRevealed() {
    revealedToken = "";
    tokenCopied = false;
    if (copyTokenTimer) {
      clearTimeout(copyTokenTimer);
      copyTokenTimer = undefined;
    }
  }

  $effect(() => {
    const key = projectKey;
    const isActive = active;
    if (!isActive || !key) {
      clearRevealed();
      if (regenerateDialog?.open) regenerateDialog.close();
      if (!key) {
        lastLoadedKey = "";
        credentialState = "unknown";
        credentialError = "";
        credentialSuccess = "";
      }
      return;
    }
    if (key !== lastLoadedKey) {
      clearRevealed();
      credentialError = "";
      credentialSuccess = "";
      if (regenerateDialog?.open) regenerateDialog.close();
    }
    lastLoadedKey = key;
    void loadCredentialStatus();
  });

  onDestroy(() => {
    clearRevealed();
  });

  const parsedPort = $derived(Number(portDraft));
  const isPortValid = $derived(
    /^\d+$/.test(portDraft) && Number.isInteger(parsedPort) && parsedPort >= 1 && parsedPort <= 65535,
  );
  const endpoint = $derived(
    `http://127.0.0.1:${projectStore.selected?.jsonData.mcp?.port ?? 8765}/mcp`,
  );

  function onPortInput(event: Event) {
    portDraft = (event.currentTarget as HTMLInputElement).value;
    portError = isPortValid ? "" : "Enter an integer from 1 to 65535.";
  }

  async function onPortChange() {
    const project = projectStore.selected;
    if (!project) return;
    if (!isPortValid) {
      portError = "Enter an integer from 1 to 65535.";
      return;
    }

    project.jsonData.mcp ??= { enabled: false, port: 8765 };
    project.jsonData.mcp.port = parsedPort;
    await saveProject(project);
  }

  async function onEnabledChange(event: Event) {
    const project = projectStore.selected;
    if (!project) return;

    const current = project.jsonData.mcp ?? { enabled: false, port: 8765 };
    project.jsonData.mcp = {
      ...current,
      enabled: (event.currentTarget as HTMLInputElement).checked,
    };
    await saveProject(project);
  }

  async function saveProject(project: ProjectItem) {
    try {
      const result = await helper_saveProjectSettings(project);
      if (result.status !== "success") throw new Error(result.message || "Failed to save project settings");
      onChanged(project);
    } catch (error) {
      toaster.error({ title: "Failed to save project settings", description: String(error) });
    }
  }

  async function onCopy() {
    try {
      await navigator.clipboard.writeText(endpoint);
      copied = true;
      setTimeout(() => (copied = false), 1500);
    } catch (error) {
      toaster.error({ title: "Failed to copy endpoint", description: String(error) });
    }
  }

  async function loadCredentialStatus() {
    const reqKey = projectKey;
    const reqPath = settingsFilePath;
    const reqProjectId = projectId;
    const seq = ++statusRequestId;

    if (!reqPath || !reqProjectId) {
      credentialLoading = false;
      credentialState = "unknown";
      credentialError = "";
      return;
    }

    credentialLoading = true;
    credentialError = "";
    try {
      const res = await helper_getMcpCredentialStatus({ settingsFilePath: reqPath, projectId: reqProjectId });
      if (seq !== statusRequestId || projectKey !== reqKey) return;
      if (res.status !== "success") {
        credentialState = "unknown";
        credentialError = res.message || "Unable to load MCP credential status.";
        return;
      }
      credentialState = res.state;
    } catch (error) {
      if (seq !== statusRequestId || projectKey !== reqKey) return;
      credentialState = "unknown";
      credentialError = error instanceof Error ? error.message : String(error);
    } finally {
      if (seq === statusRequestId) credentialLoading = false;
    }
  }

  async function onGenerateToken() {
    const reqKey = projectKey;
    const reqPath = settingsFilePath;
    const reqProjectId = projectId;
    if (!reqPath || !reqProjectId || credentialBusy) return;

    credentialBusy = true;
    credentialError = "";
    credentialSuccess = "";
    try {
      const res = await helper_generateMcpCredential({ settingsFilePath: reqPath, projectId: reqProjectId });
      if (projectKey !== reqKey) return;
      if (res.status !== "success") throw new Error(res.message || "Failed to generate MCP credential");
      credentialState = res.state;
      credentialSuccess = res.created
        ? "Managed MCP token generated and stored."
        : "Loaded existing managed MCP token.";
      toaster.success({ title: "MCP credential ready", description: credentialSuccess });
    } catch (error) {
      if (projectKey !== reqKey) return;
      const message = error instanceof Error ? error.message : String(error);
      credentialError = message;
      toaster.error({ title: "Failed to generate MCP token", description: message });
    } finally {
      credentialBusy = false;
    }
  }

  async function onToggleReveal() {
    if (revealedToken) {
      clearRevealed();
      return;
    }
    const reqKey = projectKey;
    const reqPath = settingsFilePath;
    const reqProjectId = projectId;
    if (!reqPath || !reqProjectId || credentialBusy) return;

    credentialBusy = true;
    credentialError = "";
    try {
      const res = await helper_revealMcpCredential({ settingsFilePath: reqPath, projectId: reqProjectId });
      if (projectKey !== reqKey || !active) return;
      if (res.status !== "success" || !res.token) throw new Error(res.status === "error" ? res.message : "Credential token unavailable");
      revealedToken = res.token;
    } catch (error) {
      if (projectKey !== reqKey) return;
      const message = error instanceof Error ? error.message : String(error);
      credentialError = message;
      toaster.error({ title: "Failed to reveal MCP token", description: message });
    } finally {
      credentialBusy = false;
    }
  }

  async function onCopyToken() {
    const reqKey = projectKey;
    const reqPath = settingsFilePath;
    const reqProjectId = projectId;
    if (!reqPath || !reqProjectId || credentialBusy) return;

    credentialBusy = true;
    credentialError = "";
    try {
      let tokenToCopy = revealedToken;
      if (!tokenToCopy) {
        const res = await helper_revealMcpCredential({ settingsFilePath: reqPath, projectId: reqProjectId });
        if (projectKey !== reqKey) return;
        if (res.status !== "success" || !res.token) throw new Error(res.status === "error" ? res.message : "Credential token unavailable");
        tokenToCopy = res.token;
      }
      await navigator.clipboard.writeText(tokenToCopy);
      tokenToCopy = "";
      tokenCopied = true;
      if (copyTokenTimer) clearTimeout(copyTokenTimer);
      copyTokenTimer = setTimeout(() => {
        tokenCopied = false;
        copyTokenTimer = undefined;
      }, 1500);
    } catch (error) {
      if (projectKey !== reqKey) return;
      const message = error instanceof Error ? error.message : String(error);
      credentialError = message;
      toaster.error({ title: "Failed to copy MCP token", description: message });
    } finally {
      credentialBusy = false;
    }
  }

  async function openRegenerateConfirm() {
    if (!regenerateDialog || credentialBusy) return;
    await tick();
    regenerateDialog.showModal();
  }

  async function confirmRegenerate() {
    const reqKey = projectKey;
    const reqPath = settingsFilePath;
    const reqProjectId = projectId;
    if (!reqPath || !reqProjectId || credentialBusy) return;

    credentialBusy = true;
    credentialError = "";
    credentialSuccess = "";
    clearRevealed();
    try {
      const res = await helper_regenerateMcpCredential({ settingsFilePath: reqPath, projectId: reqProjectId });
      if (projectKey !== reqKey) return;
      if (res.status !== "success") throw new Error(res.message || "Failed to regenerate MCP credential");
      credentialState = res.state;
      credentialSuccess = "MCP token regenerated. Existing clients must update their credentials.";
      if (regenerateDialog?.open) regenerateDialog.close();
      toaster.success({ title: "MCP token regenerated", description: "Existing clients must update their credentials." });
    } catch (error) {
      if (projectKey !== reqKey) return;
      const message = error instanceof Error ? error.message : String(error);
      credentialError = message;
      if (regenerateDialog?.open) regenerateDialog.close();
      toaster.error({ title: "Failed to regenerate MCP token", description: message });
    } finally {
      credentialBusy = false;
    }
  }

  let tools = $state<{ name: string; description: string }[]>([]);
  let toolsLoading = $state(true);
  let toolsError = $state("");

  async function loadTools() {
    toolsLoading = true;
    toolsError = "";
    try {
      if (!window.cppApi?.getMcpTools) throw new Error("Tool discovery requires the updated desktop dashboard.");
      const result = await window.cppApi.getMcpTools();
      if (result.status !== "success") throw new Error(result.message);
      tools = result.tools;
    } catch (error) {
      toolsError = String(error);
    } finally {
      toolsLoading = false;
    }
  }

  onMount(() => { void loadTools(); });
</script>

{#if projectStore.selected}
  <div class="h-full p-4 overflow-auto">
    <div class="rounded-md shadow p-4 flex flex-col gap-5">
      <h2 class="text-xl font-bold flex items-center gap-2">
        <icons.Network size={24} />
        MCP Server
      </h2>

      <label class="flex items-center gap-3 cursor-pointer">
        <input
          type="checkbox"
          class="checkbox"
          checked={projectStore.selected.jsonData.mcp?.enabled ?? false}
          onchange={onEnabledChange}
        />
        <span class="font-semibold">Enable MCP server</span>
      </label>

      <label class="label max-w-sm">
        <span class="label-text">Port</span>
        <input
          type="number"
          class="input"
          min="1"
          max="65535"
          step="1"
          value={portDraft}
          aria-invalid={!isPortValid}
          aria-describedby="mcp-port-feedback"
          oninput={onPortInput}
          onchange={onPortChange}
        />
        <span id="mcp-port-feedback" class="text-sm {portError ? 'text-error-500' : 'text-surface-500'}" aria-live="polite">
          {portError || "Valid ports are integers from 1 to 65535."}
        </span>
      </label>

      <div>
        <label class="label" for="mcp-endpoint">
          <span class="label-text">Configured endpoint</span>
        </label>
        <div class="flex gap-2">
          <input id="mcp-endpoint" class="input font-mono" type="text" readonly value={endpoint} />
          <button type="button" class="btn preset-outlined" onclick={onCopy}>
            {copied ? "Copied!" : "Copy"}
          </button>
        </div>
        <p class="mt-1 text-sm text-surface-500">
          This is the configured address; it does not confirm that an MCP server is running or available.
        </p>
      </div>

      <p class="text-sm text-surface-500">
        Settings take effect after restarting the project server.
      </p>

      <section class="rounded-md border border-surface-500/30 p-4 space-y-3">
        <div class="flex flex-wrap items-center justify-between gap-2">
          <h3 class="font-semibold flex items-center gap-2">
            <icons.KeyRound size={18} />
            Authentication
          </h3>
          {#if !projectId}
            <span class="badge preset-tonal-warning text-xs">Project ID required</span>
          {:else if credentialLoading}
            <span class="badge preset-tonal text-xs">Checking…</span>
          {:else if credentialError}
            <span class="badge preset-filled-error-500 text-xs">Error</span>
          {:else if credentialState === "configured"}
            <span class="badge preset-filled-success-500 text-xs">Configured</span>
          {:else if credentialState === "missing"}
            <span class="badge preset-tonal text-xs">Not configured</span>
          {/if}
        </div>

        <p class="text-sm text-surface-500">
          Send this bearer token in the <code>Authorization: Bearer &lt;token&gt;</code> header. Credentials are stored per OS user outside project settings and are never saved in <code>settings.json</code>.
        </p>

        {#if !projectId}
          <div class="rounded-md bg-warning-500/10 border border-warning-500/30 p-3 text-sm" role="status">
            Set a valid <strong>Project ID</strong> in the <strong>Sources</strong> tab before generating or viewing an MCP credential.
          </div>
        {:else if credentialLoading}
          <p class="text-sm text-surface-500" role="status">Loading credential status…</p>
        {:else if credentialError}
          <div class="rounded-md bg-error-500/10 border border-error-500/30 p-3 text-sm text-error-500 space-y-2" role="alert">
            <p>{credentialError}</p>
            <button type="button" class="btn preset-outlined btn-sm" disabled={credentialBusy} onclick={loadCredentialStatus}>
              Retry
            </button>
          </div>
        {:else if credentialState === "missing"}
          <div class="space-y-3">
            <p class="text-sm">
              No stored MCP token exists for this project yet. One will be generated automatically when the MCP server starts, or you can generate it now.
            </p>
            <button
              type="button"
              class="btn preset-filled-primary-500"
              disabled={credentialBusy}
              onclick={onGenerateToken}
            >
              {credentialBusy ? "Generating…" : "Generate token"}
            </button>
          </div>
        {:else if credentialState === "configured"}
          <div class="space-y-3">
            <div>
              <label class="label" for="mcp-token-field">
                <span class="label-text">Managed bearer token</span>
              </label>
              <div class="flex flex-wrap gap-2">
                <input
                  id="mcp-token-field"
                  class="input font-mono flex-1 min-w-48"
                  type="text"
                  readonly
                  value={revealedToken || "••••••••••••••••••••••••••••••••"}
                  aria-label={revealedToken ? "MCP bearer token (revealed)" : "MCP bearer token (masked)"}
                />
                <button
                  type="button"
                  class="btn preset-outlined flex items-center gap-1"
                  disabled={credentialBusy}
                  onclick={onToggleReveal}
                >
                  {#if revealedToken}
                    <icons.EyeOff size={16} />
                    Hide
                  {:else}
                    <icons.Eye size={16} />
                    Reveal
                  {/if}
                </button>
                <button
                  type="button"
                  class="btn preset-outlined flex items-center gap-1"
                  disabled={credentialBusy}
                  onclick={onCopyToken}
                >
                  <icons.Copy size={16} />
                  {tokenCopied ? "Copied!" : "Copy"}
                </button>
              </div>
            </div>

            <div class="flex flex-wrap items-center gap-3">
              <button
                type="button"
                class="btn preset-tonal-warning flex items-center gap-1"
                disabled={credentialBusy}
                onclick={openRegenerateConfirm}
              >
                <icons.RefreshCw size={16} />
                Regenerate token
              </button>
              <span class="text-xs text-surface-500">
                Regenerating invalidates the previous token immediately for subsequent requests on running MCP servers for this project.
              </span>
            </div>
          </div>
        {/if}

        {#if credentialSuccess}
          <p class="text-sm text-success-500" role="status" aria-live="polite">{credentialSuccess}</p>
        {/if}
      </section>

      <p class="text-sm text-surface-500">
        Semantic search may call the embedding provider configured for this project.
      </p>

      <details class="rounded-md border border-surface-500/30 p-4">
        <summary class="cursor-pointer font-semibold">Supported tools</summary>
        {#if toolsLoading}
          <p class="mt-3 text-sm" role="status">Loading tools from the core…</p>
        {:else if toolsError}
          <p class="mt-3 text-sm text-error-500" role="alert">{toolsError}</p>
          <button type="button" class="btn preset-outlined mt-2" onclick={loadTools}>Retry</button>
        {:else if tools.length === 0}
          <p class="mt-3 text-sm">The core reports no supported tools.</p>
        {:else}
          <ul class="mt-3 space-y-2">
            {#each tools as tool (tool.name)}
              <li><code>{tool.name}</code><p class="text-sm text-surface-500">{tool.description}</p></li>
            {/each}
          </ul>
        {/if}
      </details>
    </div>
  </div>
{/if}

<dialog
  bind:this={regenerateDialog}
  aria-labelledby="regenerate-mcp-token-title"
  class="m-auto w-[min(28rem,calc(100vw-2rem))] rounded-xl border border-surface-200-800 bg-surface-50-950 text-surface-950-50 p-6 shadow-xl backdrop:bg-black/50"
  oncancel={(event) => { if (credentialBusy) event.preventDefault(); }}
>
  <div class="flex flex-col gap-4">
    <h2 id="regenerate-mcp-token-title" class="text-lg font-semibold">Regenerate MCP token?</h2>
    <p class="text-sm text-surface-500">
      This replaces the stored MCP credential for this project and invalidates the current token for subsequent requests.
      Existing MCP clients must update their credentials.
    </p>
    <div class="flex justify-end gap-2">
      <button type="button" class="btn preset-tonal" disabled={credentialBusy} onclick={() => regenerateDialog?.close()}>
        Cancel
      </button>
      <button type="button" class="btn preset-filled-warning-500" disabled={credentialBusy} onclick={confirmRegenerate}>
        {credentialBusy ? "Regenerating…" : "Regenerate token"}
      </button>
    </div>
  </div>
</dialog>

<style>
  .label {
    text-align: left;
  }
</style>
