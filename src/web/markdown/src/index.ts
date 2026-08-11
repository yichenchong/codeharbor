import "./style.css";

import {
    renderMarkdown,
    rewriteRelativeUrls,
} from "./renderer";
import {
    requestImageUrl,
    type MarkdownBridge,
} from "./bridge";

const MAX_MARKDOWN_BYTES = 8 * 1024 * 1024;


export interface MarkdownMountOptions {
    sourceUrl: string;
    documentPath: string;
}

export interface MarkdownHost {
    dispose(): void;
}

export type ThemeRoleName =
    | "surface"
    | "surfaceDeep"
    | "surfaceSunken"
    | "surfaceRaised"
    | "surfaceHover"
    | "surfaceSelected"
    | "border"
    | "borderSubtle"
    | "text"
    | "textDim"
    | "textFaint"
    | "textOnAccent"
    | "accent"
    | "success"
    | "warning"
    | "danger"
    | "busy";

export type MarkdownThemeRoles = Partial<Record<ThemeRoleName, string>>;

const defaultThemeRoles: Record<ThemeRoleName, string> = {
    surface: "#1e1e2e",
    surfaceDeep: "#181825",
    surfaceSunken: "#11111b",
    surfaceRaised: "#313244",
    surfaceHover: "#232338",
    surfaceSelected: "#313244",
    border: "#45475a",
    borderSubtle: "#313244",
    text: "#cdd6f4",
    textDim: "#949ab3",
    textFaint: "#45475a",
    textOnAccent: "#11111b",
    accent: "#89b4fa",
    success: "#a6e3a1",
    warning: "#f9e2af",
    danger: "#f38ba8",
    busy: "#cba6f7",
};

const themeRoleNames = Object.keys(defaultThemeRoles) as ThemeRoleName[];
let mountedRoot: HTMLElement | null = null;
let mountedBridge: MarkdownBridge | null = null;
let mountedPath = "";
let currentSource = "";
let renderSerial = 0;
let mountedToken = 0;

function isThemeColor(value: unknown): value is string {
    return typeof value === "string" && /^#[0-9a-fA-F]{6}$/.test(value);
}

function normaliseThemeRoles(input: unknown): Record<ThemeRoleName, string> {
    const candidate = input && typeof input === "object"
        ? input as Record<string, unknown>
        : {};
    const next = { ...defaultThemeRoles };
    for (const role of themeRoleNames) {
        if (isThemeColor(candidate[role])) {
            next[role] = candidate[role];
        }
    }
    return next;
}

function cssVariableName(role: ThemeRoleName): string {
    return `--ch-${role.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)}`;
}

function applyThemeToDocument(roles: Record<ThemeRoleName, string>): void {
    for (const role of themeRoleNames) {
        document.documentElement.style.setProperty(
            cssVariableName(role),
            roles[role],
        );
    }
}

function statusElement(): HTMLElement | null {
    return document.getElementById("ch-markdown-status");
}

function showError(message: string): void {
    const status = statusElement();
    if (!status) {
        return;
    }
    status.textContent = message;
    status.dataset.visible = "true";
}

function clearError(): void {
    const status = statusElement();
    if (!status) {
        return;
    }
    status.textContent = "";
    status.dataset.visible = "false";
}


async function renderCurrent(): Promise<void> {
    const token = mountedToken;
    const root = mountedRoot;
    const bridge = mountedBridge;
    if (!root || !bridge || currentSource.length === 0) {
        return;
    }

    // The serial prevents a theme change or retarget from allowing an older
    // asynchronous image minting pass to put stale content back on screen.
    const serial = ++renderSerial;
    // Nothing is cleared from the status strip here. mountMarkdown() already
    // clears it before it fetches a new document, and this function also runs
    // for a live theme change on the document already on screen — where the
    // strip may be carrying a message the host put there (a failed image, say)
    // that a repaint has no business erasing.
    const sanitized = renderMarkdown(currentSource);
    const html = rewriteRelativeUrls(sanitized, mountedPath);
    // Both calls above are synchronous, so the mount cannot have changed under
    // us between the guard at the top and here.
    //
    // A theme change re-renders the document that is already on screen, and
    // replacing the markup resets the scroll box to the top. Put the reader
    // back where they were; on a fresh mount the element was just emptied, so
    // this restores 0 onto 0.
    const scrollTop = root.scrollTop;
    const scrollLeft = root.scrollLeft;
    root.innerHTML = html;
    root.scrollTop = scrollTop;
    root.scrollLeft = scrollLeft;

    const images = Array.from(root.querySelectorAll("img[data-ch-image-path]"));
    await Promise.all(images.map(async (image) => {
        const relativePath = image.getAttribute("data-ch-image-path") ?? "";
        const internalUrl = await requestImageUrl(bridge, relativePath);
        if (serial !== renderSerial || token !== mountedToken
            || mountedRoot !== root || mountedBridge !== bridge) {
            return;
        }
        image.removeAttribute("data-ch-image-path");
        if (internalUrl.startsWith("codeharbor-internal://file/")) {
            image.setAttribute("src", internalUrl);
        }
    }));
}

/**
 * Apply the QML Theme.roles map to the page and re-render the current document.
 * Re-rendering deliberately passes through renderMarkdown() again: a live theme
 * update must never create a second unsanitised insertion path.
 */
export function applyTheme(roles: unknown): void {
    applyThemeToDocument(normaliseThemeRoles(roles));
    void renderCurrent();
}

window.applyTheme = applyTheme;
applyTheme(defaultThemeRoles);

declare global {
    interface Window {
        applyTheme?: (roles: unknown) => void;
        showMarkdownError?: (message: string) => void;
    }
}

window.showMarkdownError = showError;

async function fetchMarkdown(sourceUrl: string): Promise<string> {
    if (!/^codeharbor-internal:\/\/file\//i.test(sourceUrl)) {
        throw new Error("the Markdown source address is not internal");
    }
    const response = await fetch(sourceUrl, {
        credentials: "omit",
        redirect: "error",
    });
    if (!response.ok) {
        throw new Error(`the remote file request failed (${response.status})`);
    }
    const contentLength = response.headers.get("content-length");
    if (contentLength !== null) {
        const declaredLength = Number(contentLength);
        if (Number.isFinite(declaredLength)
            && declaredLength > MAX_MARKDOWN_BYTES) {
            throw new Error("the Markdown file is too large to display");
        }
    }

    let bytes: Uint8Array;
    if (!response.body) {
        const array = await response.arrayBuffer();
        if (array.byteLength > MAX_MARKDOWN_BYTES) {
            throw new Error("the Markdown file is too large to display");
        }
        bytes = new Uint8Array(array);
    } else {
        const reader = response.body.getReader();
        const chunks: Uint8Array[] = [];
        let total = 0;
        try {
            while (true) {
                const part = await reader.read();
                if (part.done) {
                    break;
                }
                total += part.value.byteLength;
                if (total > MAX_MARKDOWN_BYTES) {
                    await reader.cancel();
                    throw new Error("the Markdown file is too large to display");
                }
                chunks.push(part.value);
            }
        } finally {
            reader.releaseLock();
        }
        bytes = new Uint8Array(total);
        let offset = 0;
        for (const chunk of chunks) {
            bytes.set(chunk, offset);
            offset += chunk.byteLength;
        }
    }
    try {
        return new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    } catch {
        throw new Error("the Markdown file is not valid UTF-8");
    }
}

export async function mountMarkdown(
    element: HTMLElement,
    bridge: MarkdownBridge,
    options: MarkdownMountOptions,
): Promise<MarkdownHost> {
    const token = ++mountedToken;
    mountedRoot = element;
    mountedBridge = bridge;
    mountedPath = options.documentPath;
    currentSource = "";
    renderSerial += 1;
    clearError();
    element.replaceChildren();

    try {
        const source = await fetchMarkdown(options.sourceUrl);
        // A second mount can begin before this fetch finishes. Only the
        // current mount may publish its source or trigger a render; an older
        // response must not overwrite the newer document.
        if (token === mountedToken && mountedRoot === element
            && mountedBridge === bridge) {
            currentSource = source;
            await renderCurrent();
        }
    } catch (error) {
        if (token === mountedToken && mountedRoot === element
            && mountedBridge === bridge) {
            currentSource = "";
            element.replaceChildren();
            showError(`Unable to render Markdown: ${error instanceof Error ? error.message : "remote read failed"}`);
        }
    }

    let disposed = false;
    return {
        dispose(): void {
            if (disposed) {
                return;
            }
            disposed = true;
            if (token !== mountedToken || mountedRoot !== element
                || mountedBridge !== bridge) {
                return;
            }
            renderSerial += 1;
            ++mountedToken;
            mountedRoot = null;
            mountedBridge = null;
            mountedPath = "";
            currentSource = "";
            element.replaceChildren();
        },
    };
}

interface QWebChannelObjects {
    [name: string]: unknown;
}
interface QWebChannelInstance {
    objects: QWebChannelObjects;
}
type QWebChannelCtor = new (
    transport: unknown,
    callback: (channel: QWebChannelInstance) => void,
) => QWebChannelInstance;

declare const QWebChannel: QWebChannelCtor | undefined;
declare const qt: { webChannelTransport: unknown } | undefined;

export function connectMarkdown(
    element: HTMLElement,
    options: MarkdownMountOptions,
): void {
    if (typeof QWebChannel === "undefined" || typeof qt === "undefined"
        || !qt.webChannelTransport) {
        showError("This page requires the CodeHarbor host: no WebChannel transport.");
        return;
    }
    // qwebchannel.js sends the handshake from the constructor and throws
    // straight back out of it when the transport rejects it. Uncaught, that
    // ends bootstrap() with a blank pane and no message at all — the one state
    // this page must never reach, because nothing else reports it.
    try {
        new QWebChannel(qt.webChannelTransport, (channel: QWebChannelInstance) => {
            const bridge = channel.objects.markdown as MarkdownBridge | undefined;
            if (!bridge || typeof bridge.resolveImage !== "function") {
                showError("The Markdown bridge is missing from this window.");
                return;
            }
            void mountMarkdown(element, bridge, options);
        });
    } catch {
        showError("This page could not reach the CodeHarbor host.");
    }
}

function bootstrap(): void {
    const root = document.getElementById("ch-markdown-root");
    if (!root) {
        return;
    }
    const query = new URLSearchParams(window.location.search);
    const sourceUrl = query.get("source") ?? "";
    const documentPath = query.get("path") ?? "";
    if (sourceUrl.length === 0 || documentPath.length === 0) {
        showError("The Markdown document address is missing.");
        return;
    }
    connectMarkdown(root, { sourceUrl, documentPath });
}

if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bootstrap, { once: true });
} else {
    bootstrap();
}

