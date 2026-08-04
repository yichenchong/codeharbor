import { test } from "node:test";
import assert from "node:assert/strict";
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
