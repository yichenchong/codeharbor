import { test } from "node:test";
import assert from "node:assert/strict";

import { installRenderer, type WebglLike } from "../src/renderer.ts";

// One fake terminal, one fake WebGL addon, and counters for everything
// installRenderer() is supposed to do to them.
function scenario(options: {
    createThrows?: boolean;
    loadThrows?: boolean;
    preferDom?: boolean;
    recover?: () => boolean;
} = {}) {
    const record = {
        loadedAddons: 0,
        refreshes: [] as Array<[number, number]>,
        redraws: 0,
        disposals: 0,
        warnings: [] as string[],
    };
    let loseContext: (() => void) | null = null;

    const terminal = {
        rows: 24,
        loadAddon(_addon: object) {
            if (options.loadThrows) {
                throw new Error("no WebGL2 on this host");
            }
            record.loadedAddons += 1;
        },
        refresh(start: number, end: number) {
            record.refreshes.push([start, end]);
        },
    };

    const createWebgl = (): WebglLike => {
        if (options.createThrows) {
            throw new Error("context creation refused");
        }
        return {
            onContextLoss(listener: () => void) {
                loseContext = listener;
                return undefined;
            },
            dispose() {
                record.disposals += 1;
            },
        };
    };
    const state = installRenderer(
        terminal,
        createWebgl,
        () => {
            record.redraws += 1;
        },
        (message) => record.warnings.push(message),
        { preferDom: options.preferDom, recover: options.recover },
    );

    return { state, record, loseContext: () => loseContext?.() };
}

test("WebGL is used when the host can give a context", () => {
    const { state, record } = scenario();
    assert.equal(state.name, "webgl");
    assert.equal(record.loadedAddons, 1);
    assert.deepEqual(record.warnings, []);
});

// The blank-pane defect. Chromium caps live WebGL contexts per renderer process
// and takes one away when another pane asks for one, so a terminal that ignores
// the loss stops painting for good while its buffer, its bridge and its remote
// shell all stay healthy - a pane that looks dead and cannot be typed into.
//
// The host's recovery (a reload of the page with WebGL off) takes precedence:
// disposing an addon whose context has gone throws from inside xterm and leaves
// the terminal with no renderer at all, so nothing else is attempted once the
// host says it has taken charge.
test("losing the context hands over to the host's recovery", () => {
    let recoveries = 0;
    const { state, record, loseContext } = scenario({
        recover: () => {
            recoveries += 1;
            return true;
        },
    });
    loseContext();

    assert.equal(state.name, "dom-after-context-loss");
    assert.equal(recoveries, 1);
    assert.equal(record.disposals, 0, "the addon was torn down behind the host's back");
    assert.equal(record.redraws, 0);
    assert.deepEqual(record.refreshes, []);
    assert.match(record.warnings[0], /WebGL context/);
});

// A page with nowhere to recover to - no storage to remember the decision, or
// the reload already happened - still has to try the in-place fallback, and it
// has to survive a dispose() that throws, which is what a dead context does.
test("with no recovery available the fallback happens in place", () => {
    const { state, record, loseContext } = scenario({ recover: () => false });
    loseContext();

    assert.equal(state.name, "dom-after-context-loss");
    assert.equal(record.disposals, 1);
    assert.equal(record.redraws, 1, "the pane was not re-fitted after the fallback");
    assert.deepEqual(record.refreshes, [[0, 23]],
        "the DOM renderer was never asked to repaint the visible rows");
});

// WebGL is deliberately never retried: a context evicted once under pressure is
// evicted again, and the retry loop would starve every other pane. A second
// loss event must therefore not dispose an already-disposed addon.
test("a repeated context-loss event changes nothing further", () => {
    const { record, loseContext } = scenario({ recover: () => false });
    loseContext();
    loseContext();
    assert.equal(record.disposals, 1);
    assert.equal(record.redraws, 1);
    assert.equal(record.refreshes.length, 1);
});

// The page that comes back after a context loss must not ask for WebGL again.
test("a page told to prefer the DOM never asks for a context", () => {
    let created = 0;
    const state = installRenderer(
        { rows: 24, loadAddon: () => { created = -1; }, refresh: () => {} },
        () => {
            created += 1;
            throw new Error("should never be called");
        },
        () => {},
        () => {},
        { preferDom: true },
    );
    assert.equal(state.name, "dom");
    assert.equal(created, 0);
});

test("a host that refuses a context keeps the DOM renderer", () => {
    const { state, record } = scenario({ createThrows: true });
    assert.equal(state.name, "dom");
    assert.equal(record.loadedAddons, 0);
    assert.match(record.warnings[0], /unavailable/);
});

test("an addon that fails while loading is disposed, not left holding a context", () => {
    const { state, record } = scenario({ loadThrows: true });
    assert.equal(state.name, "dom");
    assert.equal(record.disposals, 1);
    assert.match(record.warnings[0], /unavailable/);
});
