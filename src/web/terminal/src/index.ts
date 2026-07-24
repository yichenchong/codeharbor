// xterm.js renderer bridged to the C++ TerminalController over Qt WebChannel
// (SPEC 5.1). The renderer is a thin view: the controller owns buffering,
// state, and reconnect. Output is streamed in from C++; input keystrokes and
// resize events are forwarded back.
//
// Bootstrap placeholder: the WebChannel wiring and addon setup land in
// workstream T. This file establishes the module contract only.

export interface TerminalBridge {
    /** Send user keystrokes to the remote PTY. */
    sendInput(data: string): void;
    /** Notify the controller of a renderer resize. */
    resize(cols: number, rows: number): void;
}

export interface TerminalHost {
    /** Called by C++ with a batch of terminal output bytes. */
    write(data: string): void;
}

export function mountTerminal(_element: HTMLElement, _bridge: TerminalBridge): TerminalHost {
    throw new Error("not implemented: workstream T");
}
