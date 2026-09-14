import type { EmbeddingApi, GenerationApi } from "../app";

export const generationProviders = $state<GenerationApi[]>([]);
export const embeddingProviders = $state<EmbeddingApi[]>([]);

export function setGenerationProviders(list: GenerationApi[]) { generationProviders.length = 0; generationProviders.push(...list); }
export function setEmbeddingProviders(list: EmbeddingApi[]) { embeddingProviders.length = 0; embeddingProviders.push(...list); }