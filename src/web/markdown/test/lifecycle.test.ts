import { test } from "node:test";
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { buildSync } from "esbuild";
import { JSDOM } from "jsdom";
import type { MarkdownBridge } from "../src/bridge.ts";
import type * as MarkdownPage from "../src/index.ts";

// Execute the real page entry in a browser DOM. Bundling in memory keeps the
// stylesheet import and browser-only dependencies out of Node's module loader.
const bundle = buildSync({
    entryPoints: [fileURLToPath(new URL("../src/index.ts", import.meta.url))],
    bundle: true,
    write: false,
    format: "iife",
    globalName: "markdownModule",
    platform: "browser",
    target: "es2022",
    loader: { ".css": "empty" },
}).outputFiles[0].text;

type MarkdownModule = typeof MarkdownPage;

function deferred<T>() {
    let resolve!: (value: T) => void;
    // The package typechecks against ES2022, before Promise.withResolvers.
    const promise = new Promise<T>((accept) => { resolve = accept; });
    return { promise, resolve };
}

function page(fetchSource: (url: string, init?: RequestInit) => Promise<Response>) {
    const dom = new JSDOM(
        '<div id="preview"></div><div id="ch-markdown-status"></div>',
        { runScripts: "outside-only" },
    );
    Object.assign(dom.window, { TextDecoder, fetch: fetchSource });
    const module = dom.window.eval(`${bundle}\nmarkdownModule;`) as MarkdownModule;
    const root = dom.window.document.getElementById("preview")!;
    return { dom, module, root };
}

const noImages: MarkdownBridge = { resolveImage: () => "" };
const options = (name: string) => ({
    sourceUrl: `codeharbor-internal://file/${name}`,
    documentPath: `/docs/${name}.md`,
});

test("a late source response cannot replace a newer Markdown mount", async () => {
    const oldSource = deferred<Response>();
    const { dom, module, root } = page(async (url) =>
        url.endsWith("/old") ? oldSource.promise : new Response("# New document"));
    try {
        const oldMount = module.mountMarkdown(root, noImages, options("old"));
        const current = await module.mountMarkdown(root, noImages, options("new"));
        oldSource.resolve(new Response("# Old document"));
        const stale = await oldMount;
        assert.equal(root.querySelector("h1")?.textContent, "New document");
        stale.dispose();
        assert.equal(root.querySelector("h1")?.textContent, "New document");
        current.dispose();
        assert.equal(root.childElementCount, 0);
    } finally {
        dom.window.close();
    }
});

test("retargeting aborts a pending source request without publishing its error", async () => {
    let oldSignal: AbortSignal | undefined;
    const { dom, module, root } = page(async (url, init) => {
        if (!url.endsWith("/old")) {
            return new Response("# Current");
        }
        oldSignal = init?.signal ?? undefined;
        assert.ok(oldSignal);
        return new Promise<Response>((_resolve, reject) => {
            oldSignal!.addEventListener("abort", () => reject(new Error("aborted")), { once: true });
        });
    });
    try {
        const previous = module.mountMarkdown(root, noImages, options("old"));
        const current = await module.mountMarkdown(root, noImages, options("new"));
        const stale = await previous;
        assert.equal(oldSignal?.aborted, true);
        assert.equal(root.querySelector("h1")?.textContent, "Current");
        assert.equal(dom.window.document.getElementById("ch-markdown-status")!.textContent, "");
        stale.dispose();
        assert.equal(root.querySelector("h1")?.textContent, "Current");
        current.dispose();
    } finally {
        dom.window.close();
    }
});

test("late image replies cannot modify a retargeted Markdown mount", async () => {
    const { dom, module, root } = page(async (url) => new Response(
        url.endsWith("/old") ? "# Old\n\n![old](old.png)" : "# New\n\n![new](new.png)",
    ));
    const oldRequest = deferred<(url: string) => void>();
    const newRequest = deferred<(url: string) => void>();
    const bridge: MarkdownBridge = {
        resolveImage(path, callback) {
            assert.ok(callback);
            (path === "old.png" ? oldRequest : newRequest).resolve(callback);
        },
    };
    try {
        const oldMount = module.mountMarkdown(root, bridge, options("old"));
        const answerOld = await oldRequest.promise;
        const oldImage = root.querySelector("img")!;
        const newMount = module.mountMarkdown(root, bridge, options("new"));
        const answerNew = await newRequest.promise;
        answerOld("codeharbor-internal://file/stale-token");
        const stale = await oldMount;
        assert.equal(oldImage.hasAttribute("src"), false);
        assert.equal(root.querySelector("h1")?.textContent, "New");
        assert.equal(root.querySelector("img")?.hasAttribute("src"), false);
        answerNew("codeharbor-internal://file/current-token");
        const current = await newMount;
        assert.equal(root.querySelector("img")?.getAttribute("src"),
            "codeharbor-internal://file/current-token");
        stale.dispose();
        assert.equal(root.querySelector("h1")?.textContent, "New");
        current.dispose();
    } finally {
        dom.window.close();
    }
});

test("a theme repaint invalidates old image work and preserves scroll position", async () => {
    const { dom, module, root } = page(async () => new Response("# Theme\n\n![image](a.png)"));
    const firstRequest = deferred<(url: string) => void>();
    const secondRequest = deferred<(url: string) => void>();
    let requests = 0;
    const bridge: MarkdownBridge = {
        resolveImage(_path, callback) {
            assert.ok(callback);
            (++requests === 1 ? firstRequest : secondRequest).resolve(callback);
        },
    };
    try {
        const mount = module.mountMarkdown(root, bridge, options("theme"));
        const answerFirst = await firstRequest.promise;
        const oldImage = root.querySelector("img")!;
        root.scrollTop = 123;
        root.scrollLeft = 17;
        module.applyTheme({ surface: "#123456" });
        const answerSecond = await secondRequest.promise;
        assert.equal(root.scrollTop, 123);
        assert.equal(root.scrollLeft, 17);
        answerFirst("codeharbor-internal://file/stale-theme");
        const host = await mount;
        assert.equal(oldImage.hasAttribute("src"), false);
        assert.equal(root.querySelector("img")?.hasAttribute("src"), false);
        answerSecond("codeharbor-internal://file/current-theme");
        // requestImageUrl's continuation and then renderCurrent's image pass.
        await Promise.resolve();
        await Promise.resolve();
        assert.equal(root.querySelector("img")?.getAttribute("src"),
            "codeharbor-internal://file/current-theme");
        host.dispose();
    } finally {
        dom.window.close();
    }
});

test("a non-internal source is refused before issuing a request", async () => {
    let requests = 0;
    const { dom, module, root } = page(async () => {
        requests += 1;
        return new Response("# Must not render");
    });
    try {
        const host = await module.mountMarkdown(root, noImages, {
            sourceUrl: "https://example.com/readme.md",
            documentPath: "/docs/readme.md",
        });
        assert.equal(requests, 0);
        assert.equal(root.childElementCount, 0);
        assert.match(dom.window.document.getElementById("ch-markdown-status")!.textContent!,
            /source address is not internal/);
        host.dispose();
    } finally {
        dom.window.close();
    }
});

test("malformed UTF-8 produces a visible error instead of replacement text", async () => {
    const { dom, module, root } = page(async () => new Response(
        new Uint8Array([0x23, 0x20, 0xc3, 0x28]),
    ));
    try {
        const host = await module.mountMarkdown(root, noImages, options("invalid"));
        assert.equal(root.childElementCount, 0);
        const status = dom.window.document.getElementById("ch-markdown-status")!;
        assert.equal(status.dataset.visible, "true");
        assert.match(status.textContent!, /not valid UTF-8/);
        host.dispose();
    } finally {
        dom.window.close();
    }
});

test("an oversized streamed source is cancelled without publishing its prefix", async () => {
    let cancelled = false;
    const body = new ReadableStream<Uint8Array>({
        start(controller) {
            controller.enqueue(new Uint8Array(8 * 1024 * 1024));
            controller.enqueue(new Uint8Array([0x61]));
        },
        cancel() {
            cancelled = true;
        },
    });
    const { dom, module, root } = page(async () => new Response(body));
    try {
        const host = await module.mountMarkdown(root, noImages, options("oversized"));
        assert.equal(cancelled, true);
        assert.equal(root.childElementCount, 0);
        const status = dom.window.document.getElementById("ch-markdown-status")!;
        assert.equal(status.dataset.visible, "true");
        assert.match(status.textContent!, /too large to display/);
        host.dispose();
    } finally {
        dom.window.close();
    }
});

test("sources rejected from response headers cancel their unread bodies", async () => {
    for (const responseOptions of [
        { status: 503 },
        { headers: { "content-length": String(8 * 1024 * 1024 + 1) } },
    ]) {
        let cancelled = false;
        const body = new ReadableStream<Uint8Array>({
            start(controller) {
                controller.enqueue(new Uint8Array([0x23, 0x20, 0x61]));
            },
            cancel() {
                cancelled = true;
            },
        });
        const { dom, module, root } = page(async () => new Response(body, responseOptions));
        try {
            const host = await module.mountMarkdown(root, noImages, options("rejected"));
            assert.equal(cancelled, true);
            assert.equal(body.locked, false);
            assert.equal(root.childElementCount, 0);
            const status = dom.window.document.getElementById("ch-markdown-status")!;
            assert.equal(status.dataset.visible, "true");
            assert.match(status.textContent!, responseOptions.status
                ? /remote file request failed \(503\)/ : /too large to display/);
            host.dispose();
        } finally {
            dom.window.close();
        }
    }
});
