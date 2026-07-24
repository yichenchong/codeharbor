// xterm.js renderer bridged to the C++ TerminalController over Qt WebChannel
// (SPEC 5.1). The renderer is a thin view: the controller owns buffering,
// state, and reconnect. Output is streamed in from C++; input keystrokes and
// resize events are forwarded back.
//
// Bootstrap placeholder: the WebChannel wiring and addon setup land in
// workstream T. This file establishes the module contract only.

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

export function mountTerminal(_element: HTMLElement, _bridge: TerminalBridge): TerminalHost {
    throw new Error("not implemented: workstream T");
}
