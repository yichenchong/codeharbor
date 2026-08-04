import { marked, Renderer } from "marked";
import type { Tokens } from "marked";
import createDOMPurify from "dompurify";
import type { Config, DOMPurify } from "dompurify";

/** The only bridge capability the page needs for remote subresources. */
export interface MarkdownSanitizer {
    sanitize(dirty: string, config?: Config): string;
}

const renderer = new Renderer();
renderer.code = ({ text, lang }: Tokens.Code): string => {
    // marked gives us the raw code token. Escape it here because this custom
    // renderer bypasses marked's default code renderer, and then DOMPurify
    // receives the escaped HTML as an additional defence.
    const escaped = escapeHtml(text);
    const language = lang?.trim() ?? "";
    const safeLanguage = language.length > 0 ? escapeHtml(language) : "";
    const languageClass = safeLanguage.length > 0
        ? ` class="language-${safeLanguage}"`
        : "";
    const languageLabel = safeLanguage.length > 0
        ? ` data-language="${safeLanguage}"`
        : "";
    return `<pre${languageLabel}><code${languageClass}>${escaped}</code></pre>`;
};
renderer.checkbox = ({ checked }: Tokens.Checkbox): string => {
    const state = checked ? " checked" : "";
    return `<input type="checkbox"${state} disabled>`;
};

// DOMPurify's built-in URL checks reject javascript:, vbscript: and data: by
// default. This narrower expression also rejects protocol-relative URLs, which
// could otherwise reach an external host from a link in the privileged page.
const allowedUri = /^(?:(?:https?|mailto|file|codeharbor-internal):|#|\/(?!\/)|\.{0,2}\/(?!\/)|[^:\/?#]+(?:[\/?#]|$))/i;

const sanitizerConfig: Config = {
    ALLOW_DATA_ATTR: false,
    ADD_ATTR: ["data-language"],
    ALLOWED_URI_REGEXP: allowedUri,
    FORBID_ATTR: ["sizes", "srcset", "style"],
    FORBID_TAGS: [
        "audio",
        "base",
        "embed",
        "form",
        "iframe",
        "link",
        "math",
        "meta",
        "object",
        "picture",
        "script",
        "source",
        "style",
        "svg",
        "template",
        "track",
        "video",
    ],
    KEEP_CONTENT: true,
};

function escapeHtml(value: string): string {
    return value
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#39;");
}

function createBrowserSanitizer(): MarkdownSanitizer {
    if (typeof window === "undefined") {
        throw new Error("Markdown sanitisation requires a browser document");
    }
    return createDOMPurify(window) as unknown as DOMPurify;
}

/**
 * Parse and sanitise one Markdown source string. Sanitisation is deliberately
 * the final operation before the result is returned, and the sanitizer can be
 * injected by the Node tests without constructing a browser page.
 */
export function renderMarkdown(
    source: string,
    sanitizer?: MarkdownSanitizer,
): string {
    let dirty: string;
    try {
        dirty = String(marked.parse(source, {
            gfm: true,
            renderer,
        }));
    } catch {
        // A malformed extension token must not turn a readable document into a
        // blank pane. The fallback is plain escaped text, still passed through
        // the sanitizer before it reaches the caller.
        dirty = `<pre>${escapeHtml(source)}</pre>`;
    }

    const active = sanitizer ?? createBrowserSanitizer();
    try {
        return active.sanitize(dirty, sanitizerConfig);
    } catch {
        // Never return dirty HTML when a host sanitizer fails. Showing escaped
        // source is safer and more useful than replacing the whole pane with a
        // blank error page. It is already escaped, so this final fallback does
        // not need to call the broken sanitizer a second time.
        return `<pre>${escapeHtml(source)}</pre>`;
    }
}

function isRelativeResource(value: string): boolean {
    return value.length > 0
        && !/^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(value)
        && !value.startsWith("#")
        && !value.startsWith("?");
}

function pathDirectory(documentPath: string): string {
    const path = documentPath.split(/[?#]/, 1)[0];
    if (path.endsWith("/")) {
        return path.slice(0, -1) || "/";
    }
    const separator = path.lastIndexOf("/");
    return separator < 0 ? "/" : path.slice(0, separator) || "/";
}

/** Resolve a Markdown-relative path using remote POSIX path semantics. */
export function resolveRemotePath(documentPath: string, relativePath: string): string {
    const candidate = relativePath.split(/[?#]/, 1)[0];
    const combined = candidate.startsWith("/")
        ? candidate
        : `${pathDirectory(documentPath)}/${candidate}`;
    const output: string[] = [];
    for (const part of combined.split("/")) {
        if (part.length === 0 || part === ".") {
            continue;
        }
        if (part === "..") {
            if (output.length > 0) {
                output.pop();
            }
            continue;
        }
        output.push(part);
    }
    return `/${output.join("/")}`;
}
/** Convert a server POSIX path to CodeHarbor's remote file:// spelling. */
export function remotePathToFileUrl(path: string): string {
    const absolutePath = path.startsWith("/") ? path : `/${path}`;
    return `file://${absolutePath.split("/")
        .map((part) => encodeURIComponent(part))
        .join("/")}`;
}

/**
 * Rewrite only after sanitisation. Relative links become remote file addresses
 * that QML routes through the normal viewer navigation path. Images are held as
 * generated data attributes until the narrow WebChannel bridge mints an opaque
 * internal URL; every non-relative image is removed so it cannot load a network
 * or client-file resource.
 */
export function rewriteRelativeUrls(
    sanitizedHtml: string,
    documentPath: string,
    document: Pick<Document, "createElement"> = globalThis.document,
): string {
    const template = document.createElement("template");
    template.innerHTML = sanitizedHtml;

    for (const image of template.content.querySelectorAll("img")) {
        const source = image.getAttribute("src") ?? "";
        if (source.length > 0 && isRelativeResource(source)) {
            image.setAttribute("data-ch-image-path", source);
        }
        // No source or responsive source is left until the bridge gives us
        // one opaque codeharbor-internal:// URL.
        image.removeAttribute("src");
        image.removeAttribute("srcset");
        image.removeAttribute("sizes");
    }

    for (const link of template.content.querySelectorAll("a[href]")) {
        const href = link.getAttribute("href") ?? "";
        if (isRelativeResource(href)) {
            const separator = href.search(/[?#]/);
            const path = separator < 0 ? href : href.slice(0, separator);
            const suffix = separator < 0 ? "" : href.slice(separator);
            link.setAttribute("href", remotePathToFileUrl(
                resolveRemotePath(documentPath, path),
            ) + suffix);
        }
        // target was forbidden by the sanitizer. Keep all navigation in the
        // current pane so QML can apply the external-profile boundary.
        link.removeAttribute("target");
    }

    return template.innerHTML;
}

export { sanitizerConfig };
