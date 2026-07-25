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
    const resizeObserver = new ResizeObserver(() => fit.fit());
    resizeObserver.observe(element);

    // Report visibility so the controller can suspend/resume the renderer while
    // output keeps buffering server-side (SPEC 5.4).
    const visibilityObserver = new IntersectionObserver((entries) => {
        for (const entry of entries)
            bridge.notifyViewVisible(entry.isIntersecting);
    });
    visibilityObserver.observe(element);

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

declare const QWebChannel: QWebChannelCtor;
declare const qt: { webChannelTransport: unknown };

/**
 * Page entry point: open the WebChannel injected by the QML host and mount the
 * terminal against the "terminal" object (the C++ ch::TerminalBridge proxy).
 */
export function connectTerminal(element: HTMLElement): void {
    new QWebChannel(qt.webChannelTransport, (channel: QWebChannelInstance) => {
        const bridge = channel.objects.terminal as TerminalChannelObject | undefined;
        if (!bridge) {
            return; // host registered no bridge: leave the page inert
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
        // an IntersectionObserver firing into a dead terminal.
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
