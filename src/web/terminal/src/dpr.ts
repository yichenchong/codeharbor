// xterm.js reads the renderer ratio from window.devicePixelRatio. Qt
// WebEngine's page can report a different value from the physical screen when
// the WebEngine surface is composited into a scaled Qt Quick item, which makes
// a canvas backing store too small and lets Chromium blur it. This controller
// keeps the screen value for diagnostics, and temporarily exposes the user's
// explicit ratio to xterm when requested.

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
            const requested = finite > 0
                ? Math.min(4, Math.max(1, finite))
                : 0;
            if (requested === 0 || descriptor?.configurable === false) {
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
