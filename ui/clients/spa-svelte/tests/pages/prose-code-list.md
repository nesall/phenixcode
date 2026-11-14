You're right — the current binding is wrong. `generation.supports_tools` is a single scalar for the whole project, but tool support is a **per-model capability**. A project with `glm-5.3-flash` and `mistral-small` enabled can have one tool-capable and one not.

**Correct home: `providers.json`, per `GenerationApi` entry**

This is where model metadata already lives (`context_length`, `pricing_tpm`, `max_tokens_name`). Add:

```ts
// app.d.ts, GenerationApi
supports_tools?: "auto" | "on" | "off";
```

Each provider entry gets its own flag, defaulting to `"auto"` when absent.

**What to change in Generation.svelte**

Remove the single "Supports Tools" select from the General section. Instead, render a small select **per provider row** in the "Generation Providers" list — the same place the checkbox lives today. Something like:

```
[x] Mistral (mistral-small)        Tools: [Auto ▾]
[x] Z.AI (glm-5.3-flash)           Tools: [Auto ▾]
```

On change, write to the provider object and call `helper_saveProvidersSettings` (or your existing providers save path) instead of `helper_saveProjectSettings`.

**settings.json gets nothing**

Remove `supports_tools` from `GenerationSettings` in `app.d.ts` — or keep it as a deprecated project-level override if you want backward compat, but the source of truth should be providers.json.

**Runtime resolution order** (in `httpserver_generation.cpp`):
1. `provider.supports_tools == "on"` → always attach tools
2. `== "off"` → never
3. `== "auto"` or absent → probe/observe as discussed earlier

This keeps the model capability where it belongs — next to `api_url` and `context_length` — and keeps settings.json purely about *how the project uses* providers, not *what the providers can do*.

Sources:  
*ui/dashboard/spa-svelte/src/lib/widgets/project-panels/config/Generation.svelte*  
*ui/dashboard/spa-svelte/src/lib/utils.ts*  
*include/cutils.h*  
*src/cutils.cpp*  
*ui/clients/spa-svelte/src/lib/utils.ts*  
*ui/dashboard/spa-svelte/src/app.d.ts*  

<span class="text-surface-500 text-xs">Model: mimo-v2.6-pro</span>  