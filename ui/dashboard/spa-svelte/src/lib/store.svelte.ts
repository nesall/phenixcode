import type { EmbeddingApi, GenerationApi } from "../app";
import type { InstanceItem, ProjectItem } from '../app';

export const generationProviders = $state<GenerationApi[]>([]);
export const embeddingProviders = $state<EmbeddingApi[]>([]);

export function setGenerationProviders(list: GenerationApi[]) { generationProviders.length = 0; generationProviders.push(...list); }
export function setEmbeddingProviders(list: EmbeddingApi[]) { embeddingProviders.length = 0; embeddingProviders.push(...list); }


export const projectStore = $state({
  selected: null as ProjectItem | null,
  list: [] as ProjectItem[],
});

export const instances = $state<InstanceItem[]>([]);

export function setInstances(list: InstanceItem[]) { instances.length = 0; instances.push(...list); }