import { test } from "node:test";
import assert from "node:assert/strict";

import { createDevicePixelRatioController } from "../src/dpr.ts";

test("explicit device pixel ratio is visible to xterm and restores screen follow mode", () => {
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
    assert.equal(controller.set(3).effective, 3);
    assert.equal(view.devicePixelRatio, 3);
    assert.equal(controller.state.actual, 1.25);

    controller.restore();
    assert.equal(view.devicePixelRatio, 1.25);
    assert.equal(controller.state.overridden, false);
    assert.ok(resizeEvents >= 2);
});

test("non-finite and out-of-range ratios follow the documented bounds", () => {
    const view = { devicePixelRatio: 2, dispatchEvent: () => true, Event };
    const controller = createDevicePixelRatioController(view);

    assert.equal(controller.set(10).effective, 4);
    assert.equal(controller.set(Number.NaN).effective, 2);
    assert.equal(controller.set(-1).effective, 2);
});
