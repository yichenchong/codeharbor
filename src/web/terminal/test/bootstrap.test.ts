import { test } from "node:test";
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { buildSync } from "esbuild";
import { JSDOM } from "jsdom";
import type * as TerminalPage from "../src/index.ts";

const bundle = buildSync({
    entryPoints: [fileURLToPath(new URL("../src/index.ts", import.meta.url))],
    bundle: true,
    write: false,
    format: "iife",
    globalName: "terminalModule",
    platform: "browser",
    target: "es2022",
    loader: { ".css": "empty" },
}).outputFiles[0].text;

test("a failed channel handshake shows an error and permits another connection", () => {
    const dom = new JSDOM('<div id="preview"></div>', { runScripts: "outside-only" });
    try {
        Object.assign(dom.window, {
            qt: { webChannelTransport: {} },
            QWebChannel: class {
                constructor() { throw new Error("transport closed"); }
            },
        });
        const module = dom.window.eval(`${bundle}\nterminalModule;`) as typeof TerminalPage;
        const root = dom.window.document.getElementById("preview")!;
        assert.doesNotThrow(() => module.connectTerminal(root));
        assert.equal(root.querySelector(".ch-terminal-status")?.textContent,
            "This page could not reach the CodeHarbor host.");

        let handshakes = 0;
        Object.assign(dom.window, {
            QWebChannel: class {
                constructor(_transport: unknown, connected: (channel: { objects: object }) => void) {
                    ++handshakes;
                    connected({ objects: {} });
                }
            },
        });
        module.connectTerminal(root);
        assert.equal(handshakes, 1, "the failed handshake released the connecting guard");
        assert.equal(root.querySelector(".ch-terminal-status")?.textContent,
            "The terminal bridge is missing from this window.");
    } finally {
        dom.window.close();
    }
});
