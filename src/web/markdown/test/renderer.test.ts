import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import createDOMPurify from "dompurify";
import { JSDOM } from "jsdom";
import {
    remotePathToFileUrl,
    renderMarkdown,
    resolveRemotePath,
    rewriteRelativeUrls,
} from "../src/renderer.ts";
import { requestImageUrl, type MarkdownBridge } from "../src/bridge.ts";

function sanitizerFor(documentWindow: unknown) {
    return createDOMPurify(documentWindow as never);
}

test("renders the supported GitHub-style Markdown structures", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    const html = renderMarkdown(
        "# Heading\n\n**bold** and *italic*\n\n"
        + "- one\n  - nested\n- two\n\n"
        + "1. first\n2. second\n\n"
        + "> quoted\n\n"
        + "[remote](notes.md) ![image](images/logo.png)\n\n"
        + "| A | B |\n| --- | --- |\n| 1 | 2 |\n\n"
        + "---\n\n- [x] done\n- [ ] todo\n\n"
        + "`inline`\n\n```typescript\nconst answer = 42;\n```",
        sanitizerFor(window),
    );

    assert.match(html, /<h1[^>]*>Heading<\/h1>/);
    assert.match(html, /<strong>bold<\/strong>/);
    assert.match(html, /<em>italic<\/em>/);
    assert.match(html, /<ul>/);
    assert.match(html, /<ol>/);
    assert.match(html, /<blockquote>/);
    assert.match(html, /<table>/);
    assert.match(html, /<hr>/);
    assert.match(html, /type="checkbox"/);
    assert.match(html, /data-language="typescript"/);
    assert.match(html, /inline/);
});

test("sanitises executable and embedding markup without losing surrounding text", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    const html = renderMarkdown(
        "before <script>alert(1)</script> after "
        + "<img src=\"ok.png\" onerror=\"alert(2)\"> "
        + "<a href=\"javascript:alert(3)\" target=\"_blank\">link</a> "
        + "<iframe src=\"https://evil.example\"></iframe> "
        + "<object data=\"https://evil.example/a\"></object> "
        + "<embed src=\"https://evil.example/a\"> "
        + "<video src=\"https://evil.example/v.mp4\"><source src=\"https://evil.example/s.mp4\">"
        + "</video><svg><image href=\"https://evil.example/i.png\"></image></svg>",
        sanitizerFor(window),
    );

    assert.match(html, /before/);
    assert.match(html, /after/);
    assert.doesNotMatch(html, /<script/i);
    assert.doesNotMatch(html, /onerror/i);
    assert.doesNotMatch(html, /javascript:/i);
    assert.doesNotMatch(html, /<iframe/i);
    assert.doesNotMatch(html, /<object/i);
    assert.doesNotMatch(html, /<embed/i);
    assert.doesNotMatch(html, /<(?:audio|video|source|svg|image)\b/i);
    assert.doesNotMatch(html, /target=/i);
});

test("responsive image sources and inline styles never survive the rewrite", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    const html = renderMarkdown(
        '<img src="ok.png" srcset="https://evil.example/x.png 1x" sizes="100vw" '
        + 'style="position:fixed;top:0">',
        sanitizerFor(window),
    );
    const rewritten = rewriteRelativeUrls(html, "/docs/readme.md", window.document);
    assert.doesNotMatch(rewritten, /srcset=|sizes=|style=/i);
    assert.match(rewritten, /data-ch-image-path="ok\.png"/);
});

test("relative images and links resolve against the document directory", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    const { document } = window;
    const html = renderMarkdown(
        "![diagram](../assets/diagram.png)\n\n[details](./details.md?raw=1#intro)",
        sanitizerFor(window),
    );
    const rewritten = rewriteRelativeUrls(html, "/docs/guide/README.md", document);

    assert.match(rewritten, /data-ch-image-path="\.\.\/assets\/diagram\.png"/);
    assert.match(rewritten, /href="file:\/\/\/docs\/guide\/details\.md\?raw=1#intro"/);
    assert.equal(
        resolveRemotePath("/docs/guide/README.md", "../assets/diagram.png"),
        "/docs/assets/diagram.png",
    );
    assert.equal(
        remotePathToFileUrl("/docs/assets/diagram #1.png"),
        "file:///docs/assets/diagram%20%231.png",
    );
    assert.equal(
        remotePathToFileUrl("docs/readme.md"),
        "file:///docs/readme.md",
    );
});

test("no image can carry a source the host did not mint", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    // DOMPurify exempts data: URIs on <img src> from its URL policy and gives
    // no way to switch that exemption off, so the sanitizer alone lets this
    // through. The rewrite is what has to remove it, and this pins that.
    const inlinePayload = renderMarkdown(
        '<img src="data:image/svg+xml;base64,PHN2Zy8+">',
        sanitizerFor(window),
    );
    assert.match(inlinePayload, /data:image\/svg/);
    const rewritten = rewriteRelativeUrls(inlinePayload, "/docs/readme.md", window.document);
    assert.doesNotMatch(rewritten, /src=/i);
    assert.doesNotMatch(rewritten, /data:/i);

    // An absolute http(s) source is not a relative resource, so no path is
    // recorded for the bridge and nothing is left for the browser to fetch.
    const remote = rewriteRelativeUrls(
        renderMarkdown('<img src="https://evil.example/beacon.png">', sanitizerFor(window)),
        "/docs/readme.md",
        window.document,
    );
    assert.doesNotMatch(remote, /src=|data-ch-image-path=/i);
});

test("an image map cannot navigate out of the remote file namespace", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    // <area href> navigates exactly like a link but is not an <a>, so the link
    // rewrite never saw it: a relative href stayed relative and resolved
    // against the page's own qrc: address rather than the remote file
    // namespace. Markdown never produces an image map, so the element goes.
    const rewritten = rewriteRelativeUrls(
        renderMarkdown(
            '<map name="m"><area shape="rect" coords="0,0,9,9" href="notes.md">'
            + '<area href="https://evil.example/x"></map>'
            + '<img src="pic.png" usemap="#m">',
            sanitizerFor(window),
        ),
        "/docs/readme.md",
        window.document,
    );
    assert.doesNotMatch(rewritten, /<area|<map/i);
    assert.doesNotMatch(rewritten, /evil\.example/i);
    // The ordinary image beside it still reaches the bridge.
    assert.match(rewritten, /data-ch-image-path="pic\.png"/);
});

test("markup hidden inside a template cannot smuggle an image source", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    // rewriteRelativeUrls() walks ONE template's content fragment, and nodes
    // inside a NESTED <template> are unreachable from that walk, so an <img>
    // there would keep whatever source it was given. The sanitizer has to drop
    // the template outright; this is what pins that it does.
    const html = renderMarkdown(
        'before <template><img src="data:image/svg+xml;base64,PHN2Zy8+">'
        + '<a href="//evil.example/x">l</a></template> after',
        sanitizerFor(window),
    );
    assert.doesNotMatch(html, /<template|data:|evil\.example/i);
    const rewritten = rewriteRelativeUrls(html, "/docs/readme.md", window.document);
    assert.doesNotMatch(rewritten, /src=|data:|evil\.example/i);
    assert.match(rewritten, /before/);
    assert.match(rewritten, /after/);
});

test("an <input type=image> cannot fetch a URL from the privileged page", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    // <input> survives sanitisation because the task-list checkbox renderer
    // emits one, and an https: src passes the URL policy. It is an image
    // request all the same, and it is not an <img>, so the img rewrite misses
    // it. Left alone the privileged page would call out to an external host.
    const rewritten = rewriteRelativeUrls(
        renderMarkdown(
            '<input type="image" src="https://evil.example/beacon.png">'
            + '<input type="image" src="local.png">',
            sanitizerFor(window),
        ),
        "/docs/readme.md",
        window.document,
    );
    assert.doesNotMatch(rewritten, /src=/i);
    assert.doesNotMatch(rewritten, /evil\.example/i);
    // The task-list checkbox the renderer itself emits is untouched.
    const checklist = renderMarkdown("- [x] done", sanitizerFor(window));
    assert.match(checklist, /<input type="checkbox" checked="" disabled="">/);
});

test("attributes that fetch or redirect on the document's behalf are removed", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    const html = renderMarkdown(
        '<table><tr><td background="https://evil.example/b.png">cell</td></tr></table>'
        + '<a href="notes.md" download="notes.md" referrerpolicy="unsafe-url">link</a>'
        + '<img src="logo.png" crossorigin="use-credentials">',
        sanitizerFor(window),
    );
    // `background` is a legacy attribute Chromium still honours: it loads an
    // image from an arbitrary URL without ever being an <img>. `download` turns
    // a click into a profile download instead of the in-pane navigation QML
    // arbitrates. The other two let the document dictate how the privileged
    // page requests its own subresources.
    assert.doesNotMatch(html, /background=|download=|referrerpolicy=|crossorigin=/i);
    // ...and the content around them still renders.
    assert.match(html, /cell/);
    assert.match(html, /link/);
});

test("link rewriting keeps every address inside the remote file namespace", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    const rewritten = rewriteRelativeUrls(
        renderMarkdown(
            "[up](../../../../etc/passwd) [abs](/srv/other.md) "
            + "[remote](https://example.test/x) [anchor](#section)",
            sanitizerFor(window),
        ),
        "/docs/guide/README.md",
        window.document,
    );
    // ".." can never walk above the server's root, and an absolute path is
    // taken as given: SPEC 9 allows a document to link outside the project, and
    // the pane marks that rather than refusing it.
    assert.match(rewritten, /href="file:\/\/\/etc\/passwd"/);
    assert.match(rewritten, /href="file:\/\/\/srv\/other\.md"/);
    // Absolute and in-page addresses are left exactly as the author wrote them.
    assert.match(rewritten, /href="https:\/\/example\.test\/x"/);
    assert.match(rewritten, /href="#section"/);

    // A protocol-relative URL never becomes a network address: the URL policy
    // rejects it, so the sanitizer drops the attribute entirely.
    const protocolRelative = rewriteRelativeUrls(
        renderMarkdown('<a href="//evil.example/x">l</a>', sanitizerFor(window)),
        "/docs/readme.md",
        window.document,
    );
    assert.doesNotMatch(protocolRelative, /evil\.example/);
});

test("relative paths resolve the same wherever the document sits", () => {
    // The document's own address decides the base directory. These are the
    // spellings a real remote path arrives in.
    assert.equal(resolveRemotePath("/README.md", "docs/a.png"), "/docs/a.png");
    assert.equal(resolveRemotePath("/docs/", "a.png"), "/docs/a.png");
    assert.equal(resolveRemotePath("/docs/readme.md", "./a.png"), "/docs/a.png");
    assert.equal(resolveRemotePath("/docs/readme.md", "b/../a.png"), "/docs/a.png");
    assert.equal(resolveRemotePath("/docs/readme.md", "/abs.png"), "/abs.png");
    // A query or fragment belongs to neither path.
    assert.equal(resolveRemotePath("/docs/readme.md?v=2", "a.png"), "/docs/a.png");
    assert.equal(resolveRemotePath("/docs/readme.md", "a.png#top"), "/docs/a.png");
    // No document path at all still yields an absolute server path.
    assert.equal(resolveRemotePath("", "a.png"), "/a.png");
});

test("an empty sanitizer result is returned as-is, never as the dirty HTML", () => {
    // The fallback in renderMarkdown() exists for a sanitizer that THROWS. A
    // sanitizer that legitimately removes everything must not be second-guessed
    // into republishing the unsanitised input.
    assert.equal(renderMarkdown("<script>alert(1)</script>", { sanitize: () => "" }), "");
});

test("a sanitizer that strips raw tags leaves the Markdown document readable", () => {
    const sanitizer = { sanitize: (dirty: string): string => dirty.replaceAll(/<span[^>]*>|<\/span>/gi, "") };
    const html = renderMarkdown("Keep <span>this sentence</span> visible.", sanitizer);
    assert.equal(html, "<p>Keep this sentence visible.</p>\n");
});
test("a failing sanitizer falls back to escaped source instead of throwing", () => {
    const sanitizer = {
        sanitize(): string {
            throw new Error("sanitizer unavailable");
        },
    };
    assert.equal(
        renderMarkdown("<script>alert(1)</script>", sanitizer),
        "<pre>&lt;script&gt;alert(1)&lt;/script&gt;</pre>",
    );
});

test("a sanitizer that cannot even be built falls back to escaped source", () => {
    // With no sanitizer injected the renderer builds a DOMPurify against the
    // page's `window`. There is no window here, so construction fails — which
    // used to throw straight out of renderMarkdown() past the escaped-source
    // fallback, and in the page that surfaced as an unhandled promise
    // rejection and a blank pane with no message.
    assert.equal(
        renderMarkdown("<script>alert(1)</script>"),
        "<pre>&lt;script&gt;alert(1)&lt;/script&gt;</pre>",
    );
});

test("only the first word of a fence info string names the language", () => {
    const dom = new JSDOM("");
    const { window } = dom;
    // marked hands over the whole info string. Keeping all of it produced extra
    // bogus classes and printed "ts twoslash" in the corner label the
    // stylesheet draws from data-language.
    const html = renderMarkdown(
        "```ts twoslash\nconst a = 1;\n```",
        sanitizerFor(window),
    );
    assert.match(html, /data-language="ts"/);
    assert.match(html, /class="language-ts"/);
    assert.doesNotMatch(html, /twoslash/);
});
test("the image bridge also settles direct host return values", async () => {
    const bridge: MarkdownBridge = {
        resolveImage: () => "codeharbor-internal://file/direct-id",
    };
    assert.equal(
        await requestImageUrl(bridge, "diagram.png"),
        "codeharbor-internal://file/direct-id",
    );
});

test("the image bridge uses WebChannel return-value callbacks, not QML callbacks", async () => {
    const calls: string[] = [];
    const qmlImplementation = (relativePath: string, ...unexpected: unknown[]): string => {
        calls.push(relativePath);
        assert.equal(unexpected.length, 0, "QML receives only its declared argument");
        return "codeharbor-internal://file/image-id";
    };
    const bridge: MarkdownBridge & {
        resolveImage(relativePath: string, callback?: (value: string) => void): string;
    } = {
        resolveImage(relativePath: string, callback?: (value: string) => void): string {
            // This wrapper models qwebchannel.js: it removes the trailing
            // callback before invoking the QML method, then feeds the QML
            // return value back to that callback.
            const value = qmlImplementation(relativePath);
            callback?.(value);
            return value;
        },
    };

    assert.equal(
        await requestImageUrl(bridge, "../assets/diagram.png"),
        "codeharbor-internal://file/image-id",
    );
    assert.deepEqual(calls, ["../assets/diagram.png"]);
});

test("the image bridge answers every caller exactly once, even when it fails", async () => {
    // A bridge that is gone, or that refuses the path, must not leave the
    // renderer awaiting a promise that never settles: the whole render awaits
    // these, so one hung image would freeze the pane on a half-drawn document.
    const throwing: MarkdownBridge = {
        resolveImage: () => {
            throw new Error("bridge detached");
        },
    };
    assert.equal(await requestImageUrl(throwing, "diagram.png"), "");

    // QML answers "" for a path it will not resolve.
    assert.equal(await requestImageUrl({ resolveImage: () => "" }, "../../etc/passwd"), "");

    // A bridge that neither calls back nor returns a string — a WebChannel
    // transport torn down mid-render — must not hang the caller forever. The
    // renderer awaits one of these per image, so a single silent answer used to
    // leave that render pending for the life of the page.
    const started = Date.now();
    assert.equal(await requestImageUrl({ resolveImage: () => undefined }, "x.png", 20), "");
    assert.ok(Date.now() - started < 5_000, "the timeout did not bound the wait");

    // The FIRST answer wins and later ones are ignored, so a host that both
    // calls back and returns cannot resolve one image twice.
    const values: unknown[] = [];
    const chatty: MarkdownBridge = {
        resolveImage(relativePath: string, callback?: (value: string) => void): string {
            values.push(relativePath);
            callback?.("codeharbor-internal://file/first");
            callback?.("codeharbor-internal://file/second");
            return "codeharbor-internal://file/third";
        },
    };
    assert.equal(
        await requestImageUrl(chatty, "a.png"),
        "codeharbor-internal://file/first",
    );
    assert.deepEqual(values, ["a.png"]);
});

test("the page shell's content-security-policy stays locked down", () => {
    // The page runs on the privileged profile with local-content-can-access-
    // remote-urls enabled, which means CORS does not stand between it and the
    // network: this policy is the only thing that does. Nothing here is
    // exercised by loading the bundle, so it is asserted as text.
    const shell = readFileSync(
        fileURLToPath(new URL("../index.html", import.meta.url)),
        "utf-8",
    );
    const match = /http-equiv="Content-Security-Policy" content="([^"]+)"/
        .exec(shell);
    assert.ok(match, "the page shell declares no Content-Security-Policy");
    const directives = new Map(
        match[1].split(";")
            .map((part) => part.trim())
            .filter((part) => part.length > 0)
            .map((part) => {
                const [name, ...values] = part.split(/\s+/);
                return [name as string, values];
            }),
    );

    assert.deepEqual(directives.get("default-src"), ["'none'"]);
    // The page's own script is the only script. No inline, no eval, and no
    // Markdown-reachable origin.
    assert.deepEqual(directives.get("script-src"), ["'self'", "qrc:"]);
    // Only the controlled read-only scheme may be fetched or displayed: never
    // http(s), never data:, never the client's own file system.
    for (const directive of ["connect-src", "img-src", "media-src"]) {
        assert.deepEqual(directives.get(directive), ["codeharbor-internal:"],
            `${directive} must name only the internal scheme`);
    }
    // No <base> redirection and no form submission out of the page.
    assert.deepEqual(directives.get("base-uri"), ["'none'"]);
    assert.deepEqual(directives.get("form-action"), ["'none'"]);
    assert.deepEqual(directives.get("object-src"), ["'none'"]);
    // 'unsafe-inline' is permitted for STYLE only, because the host pushes the
    // theme in as inline custom properties.
    assert.ok(directives.get("style-src")?.includes("'unsafe-inline'"));
    assert.doesNotMatch(match[1], /'unsafe-eval'/);
    assert.doesNotMatch(match[1], /script-src[^;]*'unsafe-inline'/);
});