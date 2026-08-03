import { test } from "node:test";
import assert from "node:assert/strict";

import {
    applyTerminalPreferences,
    normalizeTerminalPreferences,
    terminalFontPointsToCssPixels,
} from "../src/preferences.ts";

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
