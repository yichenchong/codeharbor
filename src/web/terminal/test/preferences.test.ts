import { test } from "node:test";
import assert from "node:assert/strict";

import {
    applyTerminalPreferences,
    followMotionPreference,
    normalizeTerminalPreferences,
    terminalFontPointsToCssPixels,
} from "../src/preferences.ts";

/** A MediaQueryList stand-in whose answer the test can change. */
function fakeMotionQuery(matches: boolean) {
    const listeners = new Set<(event: { matches: boolean }) => void>();
    return {
        matches,
        listenerCount: () => listeners.size,
        change(next: boolean) {
            this.matches = next;
            for (const listener of [...listeners]) {
                listener({ matches: next });
            }
        },
        addEventListener(_type: "change", listener: (event: { matches: boolean }) => void) {
            listeners.add(listener);
        },
        removeEventListener(_type: "change", listener: (event: { matches: boolean }) => void) {
            listeners.delete(listener);
        },
    };
}

function fakeTerminal() {
    return {
        cols: 80,
        rows: 24,
        options: { fontSize: 13 },
    };
}

test("font points and pixel ratio apply live, refit, and resize the remote grid", () => {
    const terminal = fakeTerminal();
    let fits = 0;
    const ratios: number[] = [];
    const sizes: Array<[number, number]> = [];

    const applied = applyTerminalPreferences(
        terminal,
        { fit: () => { fits += 1; } },
        true,
        { resize: (cols, rows) => sizes.push([cols, rows]) },
        { fontSize: 18, pixelRatio: 2.5 },
        (ratio) => ratios.push(ratio),
    );

    assert.deepEqual(applied, { fontSize: 18, pixelRatio: 2.5 });
    assert.equal(terminal.options.fontSize, terminalFontPointsToCssPixels(18));
    assert.deepEqual(ratios, [2.5]);
    assert.equal(fits, 1);
    assert.deepEqual(sizes, [[80, 24]]);
});

test("zero pixel ratio follows the screen and values are clamped", () => {
    assert.deepEqual(normalizeTerminalPreferences(2, 0), {
        fontSize: 6,
        pixelRatio: 0,
    });
    assert.deepEqual(normalizeTerminalPreferences(999, 99), {
        fontSize: 48,
        pixelRatio: 4,
    });
    assert.deepEqual(normalizeTerminalPreferences(Number.NaN, Number.NaN), {
        fontSize: 13,
        pixelRatio: 0,
    });
});

test("the cursor stops blinking for a user who asked for reduced motion", () => {
    const terminal = { options: { cursorBlink: undefined as boolean | undefined } };

    const stop = followMotionPreference(fakeMotionQuery(true), terminal);
    assert.equal(terminal.options.cursorBlink, false);

    stop();
    followMotionPreference(fakeMotionQuery(false), terminal);
    assert.equal(terminal.options.cursorBlink, true);
});

test("a reduced-motion setting changed while the pane is open is followed", () => {
    const terminal = { options: { cursorBlink: undefined as boolean | undefined } };
    const query = fakeMotionQuery(false);

    const stop = followMotionPreference(query, terminal);
    assert.equal(terminal.options.cursorBlink, true);

    query.change(true);
    assert.equal(terminal.options.cursorBlink, false);
    query.change(false);
    assert.equal(terminal.options.cursorBlink, true);

    // Teardown really unsubscribes: a preference change after the pane is gone
    // must not reach a disposed terminal.
    stop();
    assert.equal(query.listenerCount(), 0);
    query.change(true);
    assert.equal(terminal.options.cursorBlink, true);
});

test("a host without matchMedia leaves the cursor at xterm's own default", () => {
    const terminal = { options: { cursorBlink: undefined as boolean | undefined } };

    followMotionPreference(null, terminal);
    assert.equal(terminal.options.cursorBlink, undefined);
});
