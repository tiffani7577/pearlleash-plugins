/**
 * JUCE 8 WebView ↔ React parameter bridge.
 * Supports PlugForge native setParam, JUCE backend events, and __woManus* hooks.
 */

export type ParamDetail = { id: string; value: number };

declare global {
  interface Window {
    __woManusSetParam?: (id: string, value: number) => void;
    __woManusParamChanged?: (id: string, value: number) => void;
    __plugforgeSetParam?: (detail: ParamDetail) => void;
    __JUCE__?: { backend?: { emitEvent?: (name: string, args: unknown[]) => void } };
    juce?: { getNativeFunction?: (name: string) => ((...args: unknown[]) => Promise<unknown>) | undefined };
    webkit?: {
      messageHandlers?: {
        paramChanged?: { postMessage: (msg: unknown) => void };
      };
    };
  }
}

type ParamListener = (detail: ParamDetail) => void;

const listeners = new Set<ParamListener>();

function notify(detail: ParamDetail) {
  for (const l of listeners) l(detail);
}

/** Install C++ → JS entry points once. */
export function installWoManusBridge() {
  window.__woManusSetParam = (id, value) => {
    notify({ id, value });
  };

  window.__woManusParamChanged = (id, value) => {
    try {
      window.webkit?.messageHandlers?.paramChanged?.postMessage({ id, value });
    } catch {
      /* preview / non-WKWebView */
    }
    void setParam(id, value);
  };

  window.__plugforgeSetParam = (detail) => {
    if (detail?.id != null) notify(detail);
  };

  const onPlugforge = (ev: Event) => {
    const detail = (ev as CustomEvent<ParamDetail>).detail;
    if (detail?.id != null) notify(detail);
  };
  window.addEventListener("plugforge-param", onPlugforge);

  return () => {
    window.removeEventListener("plugforge-param", onPlugforge);
  };
}

export function subscribeParams(onParam: ParamListener) {
  listeners.add(onParam);
  const uninstall = installWoManusBridge();
  return () => {
    listeners.delete(onParam);
    uninstall();
  };
}

export async function setParam(id: string, value: number) {
  try {
    const juce = window.__JUCE__?.backend;
    if (juce?.emitEvent) {
      juce.emitEvent("setParam", [id, value]);
      return;
    }
    const native = window.juce?.getNativeFunction?.("setParam");
    if (native) {
      await native(id, value);
      return;
    }
    window.webkit?.messageHandlers?.paramChanged?.postMessage({ id, value });
  } catch (e) {
    console.warn("[bridge] setParam failed", e);
  }
}

export type HostFrame =
  | { type: "spectrum"; bins?: number[]; magnitudes?: number[] }
  | { type: "meter"; level: number }
  | { type: "gr"; current: number; peak: number }
  | { type: "cloudUpdate"; message?: string };

export function onHostFrame(cb: (msg: HostFrame) => void) {
  const handler = (ev: MessageEvent) => {
    const data = ev.data;
    if (!data || typeof data !== "object") return;
    if (
      data.type === "spectrum" ||
      data.type === "meter" ||
      data.type === "gr" ||
      data.type === "cloudUpdate"
    ) {
      cb(data as HostFrame);
    }
  };
  window.addEventListener("message", handler);
  return () => window.removeEventListener("message", handler);
}

/** @deprecated alias */
export const onSpectrumFrame = onHostFrame;
