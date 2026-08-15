import { test } from "node:test";
import assert from "node:assert/strict";
import { clipboardKeyAction, type KeyChord } from "../src/keys.ts";

function chord(key: string, mods: Partial<Omit<KeyChord, "key">> = {}): KeyChord {
    return {
        key,
        ctrlKey: false,
        shiftKey: false,
        altKey: false,
        metaKey: false,
        ...mods,
    };
}

const pc = { isMac: false, hasSelection: false };
const pcSelected = { isMac: false, hasSelection: true };
const mac = { isMac: true, hasSelection: false };
const macSelected = { isMac: true, hasSelection: true };

test("plain Ctrl+V pastes rather than sending the literal-next byte", () => {
    // The whole point of the change: without this, xterm's keyboard mapper
    // turns Ctrl+V into 0x16 and the shell inserts a control character.
    assert.equal(clipboardKeyAction(chord("v", { ctrlKey: true }), pc), "paste");
    // Whether something is selected is irrelevant to a paste.
    assert.equal(clipboardKeyAction(chord("v", { ctrlKey: true }), pcSelected), "paste");
});

test("Ctrl+Shift+V still pastes", () => {
    assert.equal(
        clipboardKeyAction(chord("v", { ctrlKey: true, shiftKey: true }), pc),
        "paste",
        "the shortcut that worked before this change must keep working",
    );
});

test("Ctrl+V on macOS stays the control byte, because Cmd+V is the paste key", () => {
    assert.equal(clipboardKeyAction(chord("v", { ctrlKey: true }), mac), "pass");
    // Cmd+V is the browser's own paste command and was never ours to intercept.
    assert.equal(clipboardKeyAction(chord("v", { metaKey: true }), mac), "pass");
});

test("a V that is not the paste chord is the program's", () => {
    assert.equal(clipboardKeyAction(chord("v"), pc), "pass", "a bare v is typing");
    assert.equal(
        clipboardKeyAction(chord("v", { ctrlKey: true, altKey: true }), pc),
        "pass",
        "Ctrl+Alt+V is not paste and must not swallow AltGr combinations",
    );
});

test("plain Ctrl+C interrupts with nothing selected and copies with a selection", () => {
    assert.equal(clipboardKeyAction(chord("c", { ctrlKey: true }), pc), "pass");
    assert.equal(
        clipboardKeyAction(chord("c", { ctrlKey: true }), pcSelected),
        "copy-and-clear",
        "copying must drop the selection so the next press interrupts",
    );
});

test("Ctrl+Shift+C copies and keeps the selection", () => {
    assert.equal(
        clipboardKeyAction(chord("c", { ctrlKey: true, shiftKey: true }), pcSelected),
        "copy",
    );
});

test("the explicit copy chord is consumed even with nothing to copy", () => {
    // Regression: a dedicated copy shortcut must never fall through to the
    // remote program just because the selection was empty.
    assert.equal(
        clipboardKeyAction(chord("c", { ctrlKey: true, shiftKey: true }), pc),
        "copy",
    );
    assert.equal(clipboardKeyAction(chord("c", { metaKey: true }), mac), "copy");
});

test("macOS keeps Ctrl+C as the interrupt and Cmd+C as copy", () => {
    assert.equal(
        clipboardKeyAction(chord("c", { ctrlKey: true }), macSelected),
        "pass",
        "a Mac user pressing Ctrl+C means the interrupt every time",
    );
    assert.equal(clipboardKeyAction(chord("c", { metaKey: true }), macSelected), "copy");
});

test("the decision is case-insensitive, as a shifted key reports uppercase", () => {
    assert.equal(clipboardKeyAction(chord("V", { ctrlKey: true }), pc), "paste");
    assert.equal(
        clipboardKeyAction(chord("C", { ctrlKey: true, shiftKey: true }), pcSelected),
        "copy",
    );
});
