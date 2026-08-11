import { test } from "node:test";
import assert from "node:assert/strict";
import { clampMenuOrigin, selectionHintText, terminalMenuItems } from "../src/menu.ts";

test("copy is offered only when there is something to copy", () => {
    const withSelection = terminalMenuItems(true);
    const withoutSelection = terminalMenuItems(false);
    assert.deepEqual(
        withSelection.map((item) => item.action),
        ["copy", "paste", "select-all"],
        "the menu offers the same commands in the same order either way",
    );
    assert.deepEqual(
        withoutSelection.map((item) => item.action),
        ["copy", "paste", "select-all"],
    );
    assert.equal(withSelection[0].enabled, true);
    assert.equal(withoutSelection[0].enabled, false);
    // Paste cannot be decided without reading the clipboard, so it is always
    // offered rather than guessed at.
    assert.equal(withSelection[1].enabled, true);
    assert.equal(withoutSelection[1].enabled, true);
    assert.equal(withoutSelection[2].enabled, true);
});

test("the hint names the modifier the user's platform actually uses", () => {
    assert.match(selectionHintText(false), /Shift/);
    assert.doesNotMatch(selectionHintText(false), /Option/);
    assert.match(selectionHintText(true), /Option/);
    assert.doesNotMatch(selectionHintText(true), /Shift/);
});

test("a menu opened well inside the page keeps the pointer's position", () => {
    const origin = clampMenuOrigin(
        { x: 40, y: 30 },
        { width: 170, height: 90 },
        { width: 800, height: 600 },
    );
    assert.deepEqual(origin, { x: 40, y: 30 });
});

test("a menu opened near an edge is pulled back inside the page", () => {
    // Right-clicking eight pixels from the bottom-right corner: the menu must
    // end flush with the corner instead of hanging off it, where the part that
    // matters cannot be clicked.
    const origin = clampMenuOrigin(
        { x: 792, y: 592 },
        { width: 170, height: 90 },
        { width: 800, height: 600 },
    );
    assert.deepEqual(origin, { x: 630, y: 510 });
});

test("a menu larger than the pane starts at the top-left corner", () => {
    // A very short pane. Pinning to zero keeps the first item reachable; the
    // alternative, a negative offset, scrolls the top of the menu off screen.
    const origin = clampMenuOrigin(
        { x: 20, y: 20 },
        { width: 170, height: 90 },
        { width: 120, height: 40 },
    );
    assert.deepEqual(origin, { x: 0, y: 0 });
});
