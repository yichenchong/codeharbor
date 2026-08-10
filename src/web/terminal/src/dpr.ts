// xterm.js reads the renderer ratio from window.devicePixelRatio. Qt
// WebEngine's page can report a different value from the physical screen when
// the WebEngine surface is composited into a scaled Qt Quick item, which makes
// a canvas backing store too small and lets Chromium blur it. This controller
// keeps the screen value for diagnostics, and temporarily exposes the user's
// explicit ratio to xterm when requested.

// The SAME bounds normalizeTerminalPreferences() clamps the stored setting to.
// Imported rather than restated: two copies of "1 to 4" drift, and a
// preference the settings layer accepted but this one clamped differently
// would render at a ratio the diagnostics then report as the user's choice.
// The specifier carries its .ts extension, and must: these modules are loaded
// DIRECTLY by node --test (see test/dpr.test.ts), and node's ESM resolver does
// no extension guessing, so an extension-less specifier resolves to nothing.
// esbuild and tsc (allowImportingTsExtensions) both accept this form, so it is
// the one that works in every consumer.
import {
    kMaxTerminalPixelRatio,
    kMinTerminalPixelRatio,
} from "./preferences.ts";

export interface DevicePixelRatioWindow {
    devicePixelRatio: number;
    dispatchEvent?: (event: Event) => boolean;
    Event?: typeof Event;
}

export interface DevicePixelRatioState {
    actual: number;
    effective: number;
    overridden: boolean;
}

export interface DevicePixelRatioController {
    readonly state: DevicePixelRatioState;
    set(preference: number): DevicePixelRatioState;
    restore(): void;
}

export function createDevicePixelRatioController(
    view: DevicePixelRatioWindow,
): DevicePixelRatioController {
    const descriptor = Object.getOwnPropertyDescriptor(view, "devicePixelRatio");
    const descriptorValue = descriptor?.get
        ? Number(descriptor.get.call(view))
        : Number(descriptor?.value);
    const initialActual = Number.isFinite(descriptorValue)
        ? descriptorValue
        : Number(view.devicePixelRatio);
    let state: DevicePixelRatioState = {
        actual: Number.isFinite(initialActual) && initialActual > 0 ? initialActual : 1,
        effective: Number.isFinite(initialActual) && initialActual > 0 ? initialActual : 1,
        overridden: false,
    };
    let overridden = false;

    function notifyXterm(): void {
        if (typeof view.dispatchEvent !== "function") {
            return;
        }
        const EventConstructor = view.Event ?? Event;
        view.dispatchEvent(new EventConstructor("resize"));
    }

    const controller: DevicePixelRatioController = {
        get state(): DevicePixelRatioState {
            return state;
        },
        set(preference: number): DevicePixelRatioState {
            const finite = Number.isFinite(preference) ? preference : 0;
            const clamped = finite > 0
                ? Math.min(kMaxTerminalPixelRatio,
                           Math.max(kMinTerminalPixelRatio, finite))
                : 0;
            // NEVER above the display's real ratio. Claiming more than the
            // screen has does not add detail - there are no extra physical
            // pixels to show it in - and it makes xterm allocate a picture
            // buffer larger than its box on screen. A compositor that maps that
            // oversized buffer straight onto physical pixels, instead of
            // shrinking it back into the box, draws the whole terminal
            // magnified by exactly the ratio, which is the reported defect
            // where raising the resolution also enlarged the text. The setting
            // therefore only ever lowers the render resolution; the real ratio
            // is already the sharpest correct value.
            const requested = clamped > 0 ? Math.min(clamped, state.actual) : 0;
            if (requested === 0 || requested >= state.actual
                || descriptor?.configurable === false) {
                if (overridden) {
                    try {
                        if (descriptor) {
                            Object.defineProperty(view, "devicePixelRatio", descriptor);
                        } else {
                            delete (view as unknown as Record<string, unknown>).devicePixelRatio;
                        }
                    } catch {
                        // A host may expose a non-configurable property even
                        // after the initial descriptor check. In that case the
                        // page stays on the browser's real ratio.
                    }
                    overridden = false;
                }
                const actual = Number(view.devicePixelRatio);
                const safeActual = Number.isFinite(actual) && actual > 0 ? actual : state.actual;
                state = { actual: safeActual, effective: safeActual, overridden: false };
                notifyXterm();
                return state;
            }

            try {
                Object.defineProperty(view, "devicePixelRatio", {
                    configurable: true,
                    enumerable: descriptor?.enumerable ?? true,
                    get: () => requested,
                });
                overridden = true;
                state = { actual: state.actual, effective: requested, overridden: true };
            } catch {
                // The browser's ratio remains authoritative if the host refuses
                // a property override; callers still get an honest diagnostic.
                const actual = Number(view.devicePixelRatio);
                const safeActual = Number.isFinite(actual) && actual > 0 ? actual : state.actual;
                state = { actual: safeActual, effective: safeActual, overridden: false };
            }
            notifyXterm();
            return state;
        },
        restore(): void {
            controller.set(0);
        },
    };
    return controller;
}
