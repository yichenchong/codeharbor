import { test } from "node:test";
import assert from "node:assert/strict";

import { createDevicePixelRatioController } from "../src/dpr.ts";

// The setting may only LOWER the render resolution. Asking for more than the
// display actually has buys no detail and makes xterm allocate a picture buffer
// bigger than its box on screen, which is what let a compositor draw the whole
// terminal magnified by the ratio.
test("a ratio above the display's own is refused and the screen value is kept", () => {
    let resizeEvents = 0;
    const view = {
        devicePixelRatio: 1.25,
        dispatchEvent: () => {
            resizeEvents += 1;
            return true;
        },
        Event,
    };
    const controller = createDevicePixelRatioController(view);

    assert.deepEqual(controller.state, {
        actual: 1.25,
        effective: 1.25,
        overridden: false,
    });
    assert.equal(controller.set(3).effective, 1.25);
    assert.equal(view.devicePixelRatio, 1.25);
    assert.equal(controller.state.overridden, false);
    assert.ok(resizeEvents >= 1);
});

test("a ratio below the display's own is applied and then restored", () => {
    let resizeEvents = 0;
    const view = {
        devicePixelRatio: 2,
        dispatchEvent: () => {
            resizeEvents += 1;
            return true;
        },
        Event,
    };
    const controller = createDevicePixelRatioController(view);

    assert.equal(controller.set(1).effective, 1);
    assert.equal(view.devicePixelRatio, 1);
    assert.equal(controller.state.actual, 2);
    assert.equal(controller.state.overridden, true);

    controller.restore();
    assert.equal(view.devicePixelRatio, 2);
    assert.equal(controller.state.overridden, false);
    assert.ok(resizeEvents >= 2);
});

test("non-finite and out-of-range ratios follow the documented bounds", () => {
    const view = { devicePixelRatio: 2, dispatchEvent: () => true, Event };
    const controller = createDevicePixelRatioController(view);

    // 10 clamps to the maximum 4, which is above this display's 2, so the
    // display's own value stands.
    assert.equal(controller.set(10).effective, 2);
    assert.equal(controller.set(Number.NaN).effective, 2);
    assert.equal(controller.set(-1).effective, 2);
    // ...and a value between the minimum and the display's own still applies.
    assert.equal(controller.set(1.5).effective, 1.5);
});

// The REAL browser shape, and the one every other case here misses: a window's
// devicePixelRatio is an accessor on Window.prototype, not an own property. The
// controller therefore has no descriptor to put back when it restores, and must
// delete its own override so the inherited accessor shows through again. Getting
// this wrong pins the page to a stale ratio for the rest of its life.
test("a ratio inherited from a prototype is overridden and then restored", () => {
    let screenRatio = 1.5;
    const proto = {};
    Object.defineProperty(proto, "devicePixelRatio", {
        configurable: true,
        get: () => screenRatio,
    });
    const view = Object.create(proto) as {
        devicePixelRatio: number;
        dispatchEvent: () => boolean;
        Event: typeof Event;
    };
    view.dispatchEvent = () => true;
    view.Event = Event;

    const controller = createDevicePixelRatioController(view);
    assert.equal(controller.state.actual, 1.5);
    assert.equal(controller.state.overridden, false);

    // Below this display's 1.5, so it really is applied.
    assert.equal(controller.set(1).effective, 1);
    assert.equal(view.devicePixelRatio, 1);
    assert.equal(controller.state.overridden, true);

    // The screen changes while the override is in place — the user dragged the
    // window onto a display with a different scaling factor.
    screenRatio = 3;
    controller.restore();
    // The inherited accessor is visible again, so the page follows the screen
    // instead of staying pinned to the ratio it was overridden to.
    assert.equal(view.devicePixelRatio, 3);
    assert.equal(controller.state.overridden, false);
    assert.equal(controller.state.effective, 3);
    assert.equal(controller.state.actual, 3);
});
