import { marked, Renderer } from "marked";
import type { Tokens } from "marked";
import createDOMPurify from "dompurify";
import type { Config } from "dompurify";

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
    // marked hands over the WHOLE info string after the fence ("ts twoslash",
    // "js title=a.js"), not just the language. Only the first token names a
    // language: keeping the rest would emit bogus extra `language-*` classes
    // and print the whole info string in the corner label the stylesheet draws
    // from data-language.
    const language = lang?.trim().split(/\s+/, 1)[0] ?? "";
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

// The URL policy for every attribute DOMPurify treats as a URI. It REPLACES
// DOMPurify's default expression, and is narrower than it: besides the
// javascript:/vbscript:/data: spellings the default already refuses, this one
// also rejects protocol-relative URLs ("//host/x"), which could otherwise reach
// an external host from a link in the privileged page.
//
// It is not the whole story for images. DOMPurify exempts `src` on <img> (and
// its DATA_URI_TAGS siblings) from this expression whenever the value starts
// with "data:", and that exemption is not configurable away. Nothing here can
// stop a data: image source, so rewriteRelativeUrls() is what removes it: that
// function drops `src` from EVERY image unconditionally and puts back only an
// opaque codeharbor-internal:// URL minted by the host.
const allowedUri = /^(?:(?:https?|mailto|file|codeharbor-internal):|#|\/(?!\/)|\.{0,2}\/(?!\/)|[^:\/?#]+(?:[\/?#]|$))/i;

const sanitizerConfig: Config = {
    ALLOW_DATA_ATTR: false,
    ADD_ATTR: ["data-language"],
    ALLOWED_URI_REGEXP: allowedUri,
    // `background` is a legacy attribute Chromium still honours on <body>,
    // <table> and <td>: it fetches an image from an arbitrary URL, so it is an
    // <img> the img rewrite below would never see. `download` turns a link
    // click into a profile download instead of the in-pane navigation QML
    // arbitrates. `crossorigin` and `referrerpolicy` let a document dictate how
    // the privileged page requests its own subresources.
    FORBID_ATTR: [
        "background",
        "crossorigin",
        "download",
        "referrerpolicy",
        "sizes",
        "srcset",
        "style",
    ],
    // `map`/`area` are an image map: <area href> NAVIGATES exactly like a link
    // and is not an <a>, so the link rewrite below never sees it and a relative
    // href would resolve against the page's own qrc: address instead of the
    // remote file namespace. Markdown never produces an image map, so the
    // whole element is dropped rather than patched up afterwards.
    //
    // `template` is load-bearing for a different reason: rewriteRelativeUrls()
    // walks ONE template's content fragment, and nodes inside a NESTED
    // <template> are not reachable from that walk. An <img src="data:...">
    // hidden in one would keep its source. Nothing in Markdown needs a
    // template, so it never reaches the rewrite at all.
    FORBID_TAGS: [
        "area",
        "audio",
        "base",
        "embed",
        "form",
        "iframe",
        "link",
        "map",
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

// One DOMPurify instance per page, created on first use. It is stateless
// between sanitize() calls (the configuration travels with each call), and
// building a fresh one per render meant a new instance for every document AND
// every live theme change, each re-walking DOMPurify's own setup.
let browserSanitizer: MarkdownSanitizer | null = null;

function createBrowserSanitizer(): MarkdownSanitizer {
    if (browserSanitizer) {
        return browserSanitizer;
    }
    if (typeof window === "undefined") {
        throw new Error("Markdown sanitisation requires a browser document");
    }
    browserSanitizer = createDOMPurify(window);
    return browserSanitizer;
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

    try {
        // Acquiring the sanitizer is inside the guard on purpose: a host where
        // DOMPurify cannot even be constructed is exactly the case the escaped
        // fallback exists for, and letting that throw out of here left the
        // caller with an unhandled rejection and a silently blank pane.
        const active = sanitizer ?? createBrowserSanitizer();
        return active.sanitize(dirty, sanitizerConfig);
    } catch {
        // Never return dirty HTML when a host sanitizer fails. Showing escaped
        // source is safer and more useful than replacing the whole pane with a
        // blank error page. It is already escaped, so this final fallback does
        // not need to call the broken sanitizer a second time.
        return `<pre>${escapeHtml(source)}</pre>`;
    }
}

// MIRRORED IN QML. src/qml/ViewerMarkdownView.qml reimplements the next three
// functions (isRelativeResource, pathDirectory, resolveRemotePath) because the
// WebChannel image bridge passes the RAW document-relative string and the host
// has to resolve it against its own copy of the document path — it cannot call
// into this bundle, which runs in a separate Chromium world. The two must stay
// in step: any change here needs the same change there, and both sides pin the
// shared table (relative, "..", ".." above the root, absolute, protocol-
// relative "//host/x", backslashes, "#"/"?" prefixes) — here in
// test/renderer.test.ts and there in
// src/qml/tests/tst_paneidentity.cpp's
// theMarkdownImagePathResolverAgreesWithTheRendererBundle.
//
// The QML side has exactly TWO deliberate divergences, documented at both ends.
// Both go the same way — it refuses where this side answers, because its answer
// is fed to a remote file READ rather than used as a link target:
//   * where resolveRemotePath() answers "/" (a reference that climbs to the
//     filesystem root), it answers "".
//   * where this side treats an empty documentPath as "/", it answers "". The
//     page cannot reach that state: its document path is the ?path= query the
//     QML side wrote. That view can, before a url arrives.
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
 *
 * This is also the only place a `data:` image source dies. DOMPurify exempts
 * data: URIs on <img src> from its URL policy and offers no way to switch that
 * exemption off, so the unconditional `src` removal below is what stops a
 * document from smuggling its own inline payload into the privileged page.
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

    // <input type="image"> issues an image request exactly like <img> does, and
    // it is NOT an <img>, so the loop above never sees it. The sanitizer keeps
    // <input> because the task-list checkbox renderer emits one, and it keeps an
    // https: src because the URL policy allows https — which left a Markdown
    // document able to fetch an arbitrary external URL from the privileged page.
    // A Markdown document has no legitimate <input src>.
    for (const input of template.content.querySelectorAll("input[src]")) {
        input.removeAttribute("src");
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
