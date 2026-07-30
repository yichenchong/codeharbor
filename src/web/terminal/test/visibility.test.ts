import { test } from "node:test";
import assert from "node:assert/strict";

import { isRendererVisible } from "../src/visibility.ts";

test("visible only when the pane intersects AND the page is foregrounded", () => {
    assert.equal(isRendererVisible(true, "visible"), true);
});

test("an off-screen pane is hidden even while the page is foregrounded", () => {
    assert.equal(isRendererVisible(false, "visible"), false);
});

test("a backgrounded page is hidden even while the pane intersects", () => {
    assert.equal(isRendererVisible(true, "hidden"), false);
});

test("both conditions false is hidden", () => {
    assert.equal(isRendererVisible(false, "hidden"), false);
});
