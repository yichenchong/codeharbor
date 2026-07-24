// xterm.js renderer bridged to the C++ TerminalController over Qt WebChannel
// (SPEC 5.1). The renderer is a thin view: the controller owns buffering,
// state, and reconnect. Output is streamed in from C++; input keystrokes and
// resize events are forwarded back.
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";

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
    fit.fit();

    // Forward user keystrokes to the remote PTY (SPEC 5.1).
    term.onData((data) => bridge.sendInput(data));
    // Report renderer size changes so C++ can resize the remote PTY (SPEC 5.1).
    term.onResize(({ cols, rows }) => bridge.resize(cols, rows));

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
    };
}
