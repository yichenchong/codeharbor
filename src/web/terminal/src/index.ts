// xterm.js renderer bridged to the C++ TerminalController over Qt WebChannel
// (SPEC 5.1). The renderer is a thin view: the controller owns buffering,
// state, and reconnect. Output is streamed in from C++; input keystrokes and
// resize events are forwarded back.
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
// Side-effect import: esbuild pulls xterm's stylesheet out of the JS graph into
// dist/terminal.css, which the packaged page links (see build.mjs).
import "@xterm/xterm/css/xterm.css";

export interface TerminalBridge {
    /** Forward user keystrokes to the remote PTY (SPEC 5.1). */
    sendInput(data: string): void;
    /** Notify the controller of a renderer resize (SPEC 5.1). */
    resize(cols: number, rows: number): void;
    /** Report view visibility so the controller can suspend/resume the
     *  renderer while output keeps buffering (SPEC 5.4). */
    notifyViewVisible(visible: boolean): void;
}

export interface TerminalHost {
    /** Called by C++ with a batch of terminal output bytes (SPEC 5.1). */
    write(data: string): void;
    /** Called by C++ with the current connection lifecycle state; the value is
     *  a ch::TerminalState string from SessionState.h (SPEC 5.6). */
    setConnectionState(state: string): void;
    /** Called by C++ to clear the visible screen buffer (SPEC 5.1). */
    clear(): void;
    /** Tear down the renderer: dispose xterm.js (and its addons) and disconnect
     *  the resize/visibility observers so nothing leaks when the pane is closed
     *  (SPEC 5.4). */
    dispose(): void;
}

export function mountTerminal(element: HTMLElement, bridge: TerminalBridge): TerminalHost {
    const term = new Terminal({
        allowProposedApi: true,
        cursorBlink: true,
        fontFamily: "monospace",
        fontSize: 13,
        scrollback: 5000,
        theme: { background: "#11111b", foreground: "#cdd6f4" },
        // SECURITY: terminal output is fully attacker-controlled — every byte
        // here came off the remote PTY. xterm.js parses OSC 8 hyperlinks out of
        // that stream, and with NO linkHandler configured its built-in default
        // takes over on click: confirm() and then `window.open()` +
        // `location.href = <the URI from the stream>`. That URI is unvalidated
        // (javascript:, file:, https://attacker/...), and this page is the
        // PRIVILEGED one — it carries the WebChannel bridge to C++. So a remote
        // process printing one escape sequence gets a navigation primitive
        // behind a single dialog.
        //
        // This page has no business opening anything: it is a terminal, not a
        // browser. An explicit handler that does nothing removes the default
        // entirely, so an OSC 8 link renders as ordinary underlined text and
        // clicking it is inert. (TerminalPaneView.qml independently refuses the
        // navigation and disables window.open — this is the layer that stops
        // the dialog from ever being shown.)
        linkHandler: { activate: () => {} },
    });
    const fit = new FitAddon();
    term.loadAddon(fit);

    // Status strip reflecting the ch::TerminalState reported by C++ (SPEC 5.6).
    const status = element.ownerDocument.createElement("div");
    status.className = "ch-terminal-status";
    status.dataset.state = "unloaded";
    status.textContent = "unloaded";
    element.appendChild(status);

    // The renderer surface xterm.js attaches to.
    const surface = element.ownerDocument.createElement("div");
    surface.className = "ch-terminal-surface";
    element.appendChild(surface);

    term.open(surface);

    // Forward user keystrokes to the remote PTY (SPEC 5.1). fit() never emits
    // onData, so this listener's ordering is independent of the size handshake.
    term.onData((data) => bridge.sendInput(data));

    // Initial-size ordering invariant: fit() FIRST (with no onResize listener
    // attached), THEN attach onResize, THEN send the fitted size exactly once.
    //   - Fitting before term.open()'s listener is wired avoids relying on fit()
    //     to emit the initial resize: FitAddon only emits onResize when the
    //     fitted dims differ from xterm's 80x24 default, so an onResize-before-fit
    //     approach silently drops the handshake whenever the surface happens to
    //     fit exactly 80x24, leaving the remote PTY stuck at its default size.
    //   - The single explicit bridge.resize() below forwards the real size to
    //     the remote PTY unconditionally, so the initial size is sent EXACTLY
    //     ONCE. The ResizeObserver's first fit() then re-fits to already-correct
    //     dims (a no-op that emits no onResize), so there is no double-send.
    fit.fit();
    // Report renderer size changes so C++ can resize the remote PTY (SPEC 5.1).
    term.onResize(({ cols, rows }) => bridge.resize(cols, rows));
    bridge.resize(term.cols, term.rows);

    // Re-fit on container resize; fit() emits onResize only when dims change.
    // BOTH boxes are observed, because either one alone misses a real resize:
    //   * element — the pane itself was resized by the host.
    //   * surface — the root keeps its size while the surface changes, because
    //     the status strip above it is display:none in the "ready" state (see
    //     index.html). Observing only the root leaves a row of the grid
    //     reserved for a strip that is no longer on screen.
    // A second fit() for one real resize costs nothing: it re-fits to
    // already-correct dims, which emits no onResize.
    const resizeObserver = new ResizeObserver(() => fit.fit());
    resizeObserver.observe(element);
    resizeObserver.observe(surface);

    // Report visibility so the controller can suspend/resume the renderer while
    // output keeps buffering server-side (SPEC 5.4).
    //
    // OWNERSHIP: this page is the AUTHORITATIVE reporter of renderer
    // visibility, because it is the only layer that observes every condition
    // deciding whether a byte written now would actually be seen. The reported
    // value is the CONJUNCTION of both, because either one alone reports a
    // hidden pane as visible:
    //   * IntersectionObserver — the element collapsed or scrolled out of view.
    //     Nothing outside the document can observe this at all.
    //   * document.visibilityState — Qt WebEngine marks the whole page hidden
    //     when its WebEngineView stops being rendered (the pane item is not
    //     visible, the pane is not the active tab, the window is minimised).
    //     The element still intersects the viewport then, so the observer alone
    //     never notices.
    //
    // The same C++ slot has two other, COARSER callers, and both are strict
    // subsets of what is computed here:
    //   * src/qml/TerminalPaneView.qml `onVisibleChanged` — an invisible QML
    //     item is precisely what stops the WebEngineView rendering, which is
    //     what turns document.visibilityState to "hidden"; it says nothing
    //     about an in-document collapse.
    //   * ch::TerminalBridge::ready() — reports visible when this page mounts,
    //     to release the buffer retained while the page was loading.
    // They exist because they can speak before this page has a renderer.
    //
    // Consequence: another caller can move ch::TerminalController's flag behind
    // this page's back, so the page must NOT remember what it last reported and
    // skip a repeat of it — that memory would be stale and would swallow the
    // correcting report. Every event re-asserts the full conjunction instead.
    // ch::TerminalController::setViewVisible() already ignores a value equal to
    // the one it holds, so a redundant report costs one WebChannel message and
    // changes nothing.
    let intersecting = true;
    function reportVisibility(): void {
        bridge.notifyViewVisible(
            intersecting && element.ownerDocument.visibilityState === "visible",
        );
    }
    const visibilityObserver = new IntersectionObserver((entries) => {
        for (const entry of entries)
            intersecting = entry.isIntersecting;
        reportVisibility();
    });
    visibilityObserver.observe(element);
    element.ownerDocument.addEventListener("visibilitychange", reportVisibility);

    return {
        write(data: string): void {
            term.write(data);
        },
        setConnectionState(state: string): void {
            status.dataset.state = state;
            status.textContent = state;
        },
        clear(): void {
            term.clear();
        },
        dispose(): void {
            resizeObserver.disconnect();
            visibilityObserver.disconnect();
            element.ownerDocument.removeEventListener("visibilitychange", reportVisibility);
            // The renderer is going away (pane closed, or the page is about to
            // be replaced by a reload). The controller must hear about it
            // BEFORE the teardown: otherwise it still believes a renderer is
            // listening and keeps emitting write() at a page that no longer
            // has a handler attached, and that output is LOST instead of being
            // retained and replayed to the next page (SPEC 5.4). Sent
            // unconditionally for the reason given above: what this page last
            // reported is not proof of what the controller currently holds.
            bridge.notifyViewVisible(false);
            // Disposing the terminal also disposes loaded addons (FitAddon).
            term.dispose();
        },
    };
}

// ---- QWebChannel page-entry bootstrap ----
// The QML host (QWebEngineView) injects `qt.webChannelTransport` and serves
// qwebchannel.js, and registers the C++ ch::TerminalBridge under the object
// name "terminal" (see src/qml/TerminalPaneView.qml). connectTerminal() opens
// the channel and mounts xterm.js against it.

/** Minimal QWebChannel signal shape (obj.signalName.connect(handler)). */
export interface Signal<F extends (...args: never[]) => void> {
    connect(handler: F): void;
    disconnect(handler: F): void;
}

/**
 * The QWebChannel proxy for the C++ ch::TerminalBridge. Its SLOTS are the
 * frozen TerminalBridge half (JS -> C++); WebChannel cannot call a JS function,
 * so the TerminalHost half (C++ -> JS) arrives as SIGNALS which this page wires
 * onto the host returned by mountTerminal().
 */
export interface TerminalChannelObject extends TerminalBridge {
    /** ch::TerminalState as a string (SPEC 5.6), cached by qwebchannel.js. */
    connectionState: string;
    /** A coalesced batch of terminal output, UTF-8 decoded by C++ -> host.write. */
    write: Signal<(data: string) => void>;
    /** Lifecycle transition -> host.setConnectionState. */
    connectionStateChanged: Signal<(state: string) => void>;
    /** The app asked for the visible screen buffer to be dropped -> host.clear. */
    clearRequested: Signal<() => void>;
    /** Optional handshake: the renderer is mounted, so the controller may stop
     *  buffering and replay what the pane missed while the page was loading
     *  (SPEC 5.4). Optional so an older host without the slot still works. */
    ready?(): void;
}

/** Minimal ambient shape of the qwebchannel.js runtime injected by the host. */
interface QWebChannelObjects {
    [name: string]: unknown;
}
interface QWebChannelInstance {
    objects: QWebChannelObjects;
}
type QWebChannelCtor = new (
    transport: unknown,
    callback: (channel: QWebChannelInstance) => void,
) => QWebChannelInstance;

// Both are injected by the host, so a page opened OUTSIDE Qt WebEngine (or one
// whose qwebchannel.js failed to load) has neither. Typed as possibly-undefined
// so the guards in connectTerminal() are the honest shape rather than a cast:
// only `typeof` may touch a global that was never declared at all.
declare const QWebChannel: QWebChannelCtor | undefined;
declare const qt: { webChannelTransport: unknown } | undefined;

/**
 * Replace the pane with a single explanatory line. Used only for the failures
 * that happen BEFORE a terminal exists, which would otherwise leave a blank
 * black rectangle and no clue (the QML pane can only report a page that failed
 * to LOAD, not one that loaded and found no bridge). textContent, never
 * innerHTML: this is a privileged page and never builds markup from a string.
 */
function showFatal(element: HTMLElement, message: string): void {
    const status = element.ownerDocument.createElement("div");
    status.className = "ch-terminal-status";
    status.dataset.state = "error";
    status.textContent = message;
    element.replaceChildren(status);
}

/**
 * Page entry point: open the WebChannel injected by the QML host and mount the
 * terminal against the "terminal" object (the C++ ch::TerminalBridge proxy).
 */
export function connectTerminal(element: HTMLElement): void {
    if (typeof QWebChannel === "undefined" || typeof qt === "undefined"
        || !qt.webChannelTransport) {
        showFatal(element, "This page requires the CodeHarbor host: no WebChannel transport.");
        return;
    }
    new QWebChannel(qt.webChannelTransport, (channel: QWebChannelInstance) => {
        const bridge = channel.objects.terminal as TerminalChannelObject | undefined;
        if (!bridge) {
            showFatal(element, "The terminal bridge is missing from this window.");
            return;
        }
        const host = mountTerminal(element, bridge);
        bridge.write.connect((data: string) => host.write(data));
        bridge.connectionStateChanged.connect((state: string) => host.setConnectionState(state));
        bridge.clearRequested.connect(() => host.clear());
        // The state reached before this page finished loading is already in the
        // property cache, so the strip is correct without waiting for a signal.
        if (typeof bridge.connectionState === "string") {
            host.setConnectionState(bridge.connectionState);
        }
        // A navigated-away or destroyed pane must not leave a ResizeObserver and
        // an IntersectionObserver firing into a dead terminal, and must hand the
        // controller back the job of retaining output (host.dispose() reports the
        // renderer hidden on its way out).
        window.addEventListener("pagehide", () => host.dispose(), { once: true });
        // Handshake LAST, once every host callback is connected: the controller
        // has been buffering output since the channel opened and replays it into
        // a renderer that is now listening (SPEC 5.4).
        bridge.ready?.();
    });
}

// The packaged page (dist/index.html) carries no inline script — its CSP
// forbids one — so the bundle boots itself.
function bootstrap(): void {
    const root = document.getElementById("ch-terminal-root");
    if (!root) {
        return; // embedded by a host that mounts explicitly
    }
    connectTerminal(root);
}

if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bootstrap, { once: true });
} else {
    bootstrap();
}
