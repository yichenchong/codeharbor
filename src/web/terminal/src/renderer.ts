// Renderer selection for the terminal page, split out from the page entry so
// the WebGL fallback rules can be unit-tested without a browser.
//
// xterm renders with the DOM unless an addon replaces it. WebGL is preferred
// because it sizes its backing store in physical pixels and redraws rather than
// stretching a low-resolution surface. It is not, however, something a pane can
// count on keeping.

/** The parts of xterm's Terminal this module touches. */
export interface RendererTerminal {
    readonly rows: number;
    loadAddon(addon: object): void;
    refresh(start: number, end: number): void;
}

/** The parts of xterm's WebglAddon this module touches. */
export interface WebglLike {
    onContextLoss(listener: () => void): unknown;
    dispose(): void;
}

/** The element the terminal is mounted into; the addon's canvas lives under it. */
export interface RendererSurface {
    addEventListener(type: string, listener: () => void, capture: boolean): void;
    removeEventListener(type: string, listener: () => void, capture: boolean): void;
}

export type RendererName = "dom" | "webgl" | "dom-after-context-loss";

export interface RendererState {
    /** Which renderer is painting right now. */
    readonly name: RendererName;
}

export interface RendererOptions {
    /** The element the terminal is mounted into. */
    surface?: RendererSurface;
    /**
     * Skip WebGL entirely. Set when this page is the reload that FOLLOWS a lost
     * context: a context evicted once under pressure would be evicted again,
     * and a loop of taking and losing contexts starves every other pane too.
     */
    preferDom?: boolean;
    /**
     * Last resort when the WebGL renderer dies. Returns true if it took charge
     * of recovery (the page is being reloaded), in which case nothing else is
     * attempted here.
     */
    recover?: () => boolean;
}

/**
 * Load the WebGL renderer if this host can give us one, and survive losing it.
 *
 * A WebGL context is a scarce PROCESS-WIDE resource: Chromium caps how many
 * live contexts one renderer process may hold and, at the cap, takes one away
 * from whoever already has it. Every terminal pane is another view asking for
 * its own, so opening panes is precisely what makes an existing pane's context
 * disappear. A WebGL renderer whose context has gone never paints again: the
 * buffer, the bridge and the remote shell all stay healthy while the pane shows
 * nothing and looks dead — the user sees a blank terminal they cannot type into.
 *
 * Recovery is by RELOAD, not by swapping the renderer in place. xterm's own
 * advice is to dispose the addon and let the DOM renderer take over, but
 * disposing an addon whose context has already gone throws from inside xterm
 * and leaves the terminal with no renderer at all — measured, not assumed. A
 * reload always produces a working page, and costs nothing the user can see:
 * the C++ controller retains this pane's output and replays it into the fresh
 * renderer, and tmux redraws the screen on reattach.
 *
 * `redraw` is called if recovery is declined, so the best-effort in-place
 * fallback at least re-measures and repaints.
 */
export function installRenderer(
    terminal: RendererTerminal,
    createWebgl: () => WebglLike,
    redraw: () => void,
    onWarning: (message: string, detail?: unknown) => void,
    options: RendererOptions = {},
): RendererState {
    const state = { name: "dom" as RendererName };
    if (options.preferDom) {
        return state;
    }

    let webgl: WebglLike;
    try {
        webgl = createWebgl();
    } catch (error) {
        // WebGL2 is unavailable in some software-only WebEngine builds. Keep
        // the DOM renderer usable there rather than making a terminal vanish.
        onWarning("CodeHarbor terminal WebGL renderer unavailable", error);
        return state;
    }

    let lost = false;
    const handleContextLoss = (): void => {
        // Once only: the two triggers below can both fire for one loss, and a
        // disposed addon must not be disposed again.
        if (lost) {
            return;
        }
        lost = true;
        onWarning("CodeHarbor terminal lost its WebGL context");
        state.name = "dom-after-context-loss";
        if (options.recover?.()) {
            return;
        }
        // No recovery available (a standalone page, or the reload already
        // happened). Try xterm's in-place fallback and keep going even if it
        // throws, which it does when the context is already gone.
        try {
            webgl.dispose();
        } catch (error) {
            onWarning("CodeHarbor terminal could not dispose its dead WebGL renderer",
                      error);
        }
        redraw();
        terminal.refresh(0, Math.max(0, terminal.rows - 1));
    };

    // TWO triggers, because neither alone is enough.
    //
    // The addon's own signal is the documented one, but it is deliberately
    // slow: the addon waits three seconds for the browser to restore the
    // context before giving up, and it was measured never arriving at all on a
    // surface the browser is not currently drawing.
    //
    // The browser's own event is dispatched at the addon's canvas the instant
    // the context goes. Listening in the CAPTURE phase at the surface catches
    // it there: `webglcontextlost` does not bubble, so a listener on an
    // ancestor only ever sees it on the way down.
    webgl.onContextLoss(handleContextLoss);
    options.surface?.addEventListener("webglcontextlost", handleContextLoss, true);

    try {
        terminal.loadAddon(webgl);
    } catch (error) {
        // loadAddon() is where the addon actually asks for a context, so this
        // is the ordinary "no WebGL here" path on a software-only host.
        onWarning("CodeHarbor terminal WebGL renderer unavailable", error);
        options.surface?.removeEventListener("webglcontextlost", handleContextLoss, true);
        webgl.dispose();
        return state;
    }
    state.name = "webgl";
    return state;
}
