import { get, writable } from "svelte/store";
import { getPersistentKey, setPersistentKey } from "./utils";

type ReasoningSelections = Record<string, Record<string, string>>;
const ReasoningSelectionsKey = "ReasoningSelections";

export const reasoningSelections = writable<ReasoningSelections>({});
export const reasoningReady = writable(false);
let initialization: Promise<void> | undefined;
let pendingSave = Promise.resolve();

export function initializeReasoning(): Promise<void> {
  return initialization ??= (async () => {
    try {
      const saved: unknown = JSON.parse((await getPersistentKey(ReasoningSelectionsKey)) || "{}");
      if (saved && typeof saved === "object" && !Array.isArray(saved)) {
        const selections = Object.fromEntries(Object.entries(saved)
          .filter(([, parameters]) => parameters && typeof parameters === "object" && !Array.isArray(parameters))
          .map(([id, parameters]) => [id, Object.fromEntries(Object.entries(parameters)
            .filter((entry): entry is [string, string] => typeof entry[1] === "string"))]));
        reasoningSelections.set(selections);
      }
    } catch {
      reasoningSelections.set({});
    } finally {
      reasoningReady.set(true);
    }
  })();
}

export function reasoningParameters(provider: ModelItem | undefined): ReasoningParameter[] {
  return provider?.reasoning?.parameters?.filter(parameter => parameter.name && parameter.values.length > 0) || [];
}

export function selectedReasoning(provider: ModelItem | undefined, selections: ReasoningSelections): Record<string, string> {
  return Object.fromEntries(reasoningParameters(provider).map(parameter => {
    const saved = selections[provider!.id]?.[parameter.name];
    const value = parameter.values.includes(saved) ? saved
      : parameter.values.includes(parameter.default) ? parameter.default : parameter.values[0];
    return [parameter.name, value];
  }));
}

export function applyReasoning(provider: ModelItem, values: Record<string, string>): boolean {
  const parameters = reasoningParameters(provider);
  if (!get(reasoningReady) || !parameters.length || parameters.some(parameter => !parameter.values.includes(values[parameter.name]))) return false;
  const applied = Object.fromEntries(parameters.map(parameter => [parameter.name, values[parameter.name]]));
  reasoningSelections.update(selections => ({
    ...selections,
    [provider.id]: applied,
  }));
  const saved = JSON.stringify(get(reasoningSelections));
  // Keep native preference writes in selection order, including rapid model switches.
  pendingSave = pendingSave.then(() => setPersistentKey(ReasoningSelectionsKey, saved));
  return true;
}

export function reasoningRequest(providerId: string, providers: ModelItem[]) {
  const reasoning = selectedReasoning(providers.find(provider => provider.id === providerId), get(reasoningSelections));
  return { targetapi: providerId, reasoning: Object.keys(reasoning).length ? reasoning : undefined };
}
