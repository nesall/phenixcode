import { createToaster } from '@skeletonlabs/skeleton-svelte';

export const toaster = createToaster();

export function escapeHtml(s: string) {
  return s
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

export function bytesToSize(bytes: number) {
  const sizes = ["Bytes", "KB", "MB", "GB", "TB"];
  if (bytes === 0) return "0 Byte";
  const i = Math.floor(Math.log(bytes) / Math.log(1024));
  return Math.round(bytes / Math.pow(1024, i)) + " " + sizes[i];
}

export function hash(str: string) {
  let hash = 0;
  if (str.length === 0) return hash;
  for (let i = 0; i < str.length; i++) {
    const chr = str.charCodeAt(i);
    hash = (hash << 5) - hash + chr;
    hash |= 0; // Convert to 32bit integer
  }
  return hash;
}

export function nextRandomId(len: number): string {
  // https://github.com/ai/nanoid/blob/main/nanoid.js
  return crypto.getRandomValues(new Uint8Array(len)).reduce(
    (len, e) => len += (e &= 63) < 36 ? e.toString(36) : e < 62 ? (e - 26).toString(36).toUpperCase() : e > 62 ? "-" : "_", "")
}

export function newUUID() {
  return crypto.randomUUID();
}

const _lastLogs: { date: string, data: string }[] = [];

export function clog(...args: any[]) {
  console.log(...args);
  if (0 < args.length) {
    const key = args.map(a => JSON.stringify(a)).join(' ');
    const now = Date.now();
    _lastLogs.push({ date: new Date(now).toLocaleTimeString(), data: key });
    while (100 < _lastLogs.length) {
      _lastLogs.shift();
    }
  }
}

export function getLastLogs() {
  return _lastLogs;
}

export function apiUrl(path: string) {
  const base = window.apiServerUrl || '';
  // clog('apiUrl ', base + path);
  return base + path;
}

export async function testConnection() {
  try {
    const res = await fetch(apiUrl("/api/health"));
    if (res && res.ok) {
      return true;
    }
  } catch (err: any) {
    clog("Error testing server connection:", err);
  }
  return false;
}

export function isPtInRect(rc: DOMRect, x: number, y: number) {
  return rc.x <= x && x < rc.x + rc.width && rc.y <= y && y < rc.y + rc.height;
}

export function isGoodArray(a: any) {
  return a && Array.isArray(a) && a.length;
}


export function stripCommonPrefix(paths: string[]) {
  if (!paths.length) return [];
  const splitPaths = paths.map((p) => p.replaceAll("\\", "/").split("/"));
  const minLen = Math.min(...splitPaths.map((p) => p.length));
  let prefixLen = 0;
  for (let i = 0; i < minLen; i++) {
    const segment = splitPaths[0][i];
    if (splitPaths.every((p) => p[i] === segment)) prefixLen++;
    else break;
  }
  if (prefixLen === 0) return paths;
  if (prefixLen === minLen) prefixLen--; // leave at least one segment
  return splitPaths.map((p) => p.slice(prefixLen).join("/"));
}

export const initResizeObserver = (ref: HTMLElement, handler: any) => {
  const resizeObserver = new ResizeObserver(entries => {
    let sizes = [];
    for (let entry of entries) {
      let size;
      if (entry.contentBoxSize) {
        size = (Array.isArray(entry.contentBoxSize) ? entry.contentBoxSize[0] : entry.contentBoxSize).inlineSize;
      } else {
        size = entry.contentRect.width;
      }
      sizes.push(size);
    }
    handler(sizes);
  });
  resizeObserver.observe(ref);
  return resizeObserver;
};

export function fnv1a64(str: string): string {
  const encoder = new TextEncoder();
  const data = encoder.encode(str); // UTF-8 bytes
  let hash = BigInt("0xcbf29ce484222325");
  const FNV_PRIME = BigInt("0x100000001b3");
  for (const byte of data) {
    hash ^= BigInt(byte);
    hash *= FNV_PRIME;
    hash &= BigInt("0xFFFFFFFFFFFFFFFF"); // keep 64-bit
  }
  return hash.toString(16).padStart(16, "0");
}



export const Consts = {
  CurrentApiKey: "api",
  TemperatureKey: "temperature",
  ThemeKey: "theme",
  // ServerUrlKey: "serverUrl",
  DarkOrLightKey: "darkOrLight",
  ContextFilesKey: "contextFiles",
  ApiOptionsSortedKey: "ApiOptionsSortedKey",
  ApiOptionsGroupedKey: "ApiOptionsGroupedKey",
  EmbedderExecutablePath: "EmbedderExecutablePath",
  EmbedderSettingsFilePaths: "EmbedderSettingsFilePaths"
};

export async function setPersistentKey(key: string, value: string, sendToCpp = true) {
  try {
    clog(`setPersistentKey ${key}`, value, sendToCpp, window.cppApi);
    localStorage.setItem(key, value);
    if (sendToCpp && window.cppApi) {
      await window.cppApi.setPersistentKey(key, value);
      clog(`Saved persistent key ${key} to C++`, value);
    }
  } catch (error) {
    clog(`Unable to set persistent key ${key}`, error)
  }
}

export async function getPersistentKey(key: string, readFromCpp = true): Promise<string | null> {
  try {
    clog(`getPersistentKey ${key}`, readFromCpp, window.cppApi);
    let val = localStorage.getItem(key);
    if (readFromCpp && window.cppApi) {
      val = await window.cppApi.getPersistentKey(key);
      clog(`Loaded persistent key ${key} from C++`, val);
      if (val != null) {
        localStorage.setItem(key, val);
      }
    }
    return val;
  } catch (error) {
    clog(`Unable to get persistent key ${key}`, error)
  }
  return null;
}


export function apiOptionsGroupedSorted(
  ao: { value: string, label: string; _price: number, group: string, desc?: string, hint?: string }[],
  bSorted: boolean,
  bGrouped: boolean,
) {
  if (bGrouped) {
    const grouped: Record<string, typeof ao> = {};
    ao.forEach((api) => {
      const provider = api.group.split(" ")[0];
      if (!grouped[provider]) {
        grouped[provider] = [];
      }
      grouped[provider].push(api);
    });
    let result: typeof ao = [];
    Object.keys(grouped).forEach((provider) => {
      let apis = grouped[provider];
      if (bSorted) {
        apis = apis.slice().sort((a, b) => a._price - b._price);
      }
      result = result.concat(apis);
    });
    return result;
  } else if (bSorted) {
    return ao.slice().sort((a, b) => a._price - b._price);
  }
  return ao;
}