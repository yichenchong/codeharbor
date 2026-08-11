// xterm.js renderer bridged to the C++ TerminalController over Qt WebChannel
// (SPEC 5.1). The renderer is a thin view: the controller owns buffering,
// state, and reconnect. Output is streamed in from C++; input keystrokes and
// resize events are forwarded back.
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import { WebglAddon } from "@xterm/addon-webgl";
// Side-effect import: esbuild pulls xterm's stylesheet out of the JS graph into
// dist/terminal.css, which the packaged page links (see build.mjs).
import "@xterm/xterm/css/xterm.css";
// Pure, DOM-free predicate split out so it can be unit-tested (see visibility.ts).
// Relative specifiers carry their .ts extension throughout this package: the
// modules are also loaded directly by node --test, whose ESM resolver does no
// extension guessing. esbuild and tsc (allowImportingTsExtensions) accept it.
import { isRendererVisible } from "./visibility.ts";
// Output flow control, likewise DOM-free and unit-tested (see writer.ts).
import { CoalescingWriter } from "./writer.ts";
// Input pacing keeps large pastes bounded and preserves ANSI sequence boundaries.
import { TerminalInputWriter } from "./input.ts";
// Renderer choice and the WebGL context-loss fallback (see renderer.ts).
import { installRenderer } from "./renderer.ts";
// The pane's own right-click menu, and the mouse policy behind it (see menu.ts).
import { TerminalContextMenu } from "./menu.ts";
import {
    applyTerminalPreferences,
    terminalFontPointsToCssPixels,
    type TerminalPreferenceValues,
} from "./preferences.ts";
import {
    createDevicePixelRatioController,
    type DevicePixelRatioController,
} from "./dpr.ts";
import {
    applyThemeToDocument,
    defaultThemeRoles,
    type ThemeRoles,
    xtermTheme,
} from "./theme.ts";

export interface TerminalBridge {
    /** Forward user keystrokes to the remote PTY (SPEC 5.1). */
    sendInput(data: string): void;
    /** Notify the controller of a renderer resize (SPEC 5.1). */
    resize(cols: number, rows: number): void;
    /** Report view visibility so the controller can suspend/resume the
     *  renderer while output keeps buffering (SPEC 5.4). */
    notifyViewVisible(visible: boolean): void;
    /** Report that the emulator has consumed `bytes` worth of what write()
     *  delivered — the flow-control half of the contract (SPEC 5.4/5.5).
     *
     *  The C++ controller emits at most a bounded amount of unacknowledged
     *  output (ch::TerminalController::kMaxUnacknowledgedBytes) and retains the
     *  rest in the same rolling buffer it uses for a hidden pane, so a runaway
     *  remote process cannot queue an unbounded amount of data in the
     *  WebChannel transport and in Chromium. `bytes` is the count the C++ side
     *  attached to the batch, echoed back unchanged: this page holds decoded
     *  text and cannot recover the PTY byte count from it. */
    notifyOutputConsumed(bytes: number): void;
}

export interface TerminalHost {
    /** Called by C++ with a batch of terminal output (SPEC 5.1). `bytes` is how
     *  many PTY bytes the text was decoded from; it is acknowledged back to C++
     *  once the emulator has consumed it. */
    write(data: string, bytes: number): void;
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
    // xterm's canvas/WebGL renderer reads window.devicePixelRatio when it is
    // constructed. Keep the browser value as the default and expose a guarded
    // override for the explicit AppSettings value.
    const pageWindow = element.ownerDocument.defaultView;
    if (!pageWindow) {
        throw new Error("terminal page has no window");
    }
    const pixelRatio: DevicePixelRatioController =
        createDevicePixelRatioController(pageWindow);
    // MOUSE REPORTING: the tmux session this pane attaches to is created with
    // `mouse on` scoped to that session (see
    // ch::TerminalController::tmuxNewSessionCommand), so tmux asks this page to
    // report mouse events and a wheel turn becomes a tmux scroll. Without that
    // request xterm.js falls back to "alternate scroll" and sends cursor-up /
    // cursor-down keys instead, because tmux occupies the alternate screen and
    // an alternate screen has no scrollback of its own — which is exactly the
    // bug where the wheel walked back through shell history.
    //
    // Nothing below may suppress that: no custom wheel handler is registered and
    // no mouse opt-out is passed, so xterm.js forwards the reports itself.
    //
    // The known cost of mouse reporting is that a plain drag selects inside tmux
    // instead of selecting the page's text. xterm.js already answers this with
    // the terminal-emulator convention — holding SHIFT while dragging forces its
    // own selection — so no override is added here. Paste is unaffected: it
    // arrives as a browser paste event on xterm.js's hidden textarea, never as a
    // mouse report.
    let activeTheme: ThemeRoles = { ...defaultThemeRoles };
    const term = new Terminal({
        cursorBlink: true,
        fontFamily: "monospace",
        // AppSettings stores the default in points; use the same conversion as
        // live preference updates so the first frame does not use a different
        // cell size from the configured default.
        fontSize: terminalFontPointsToCssPixels(13),
        // SCROLLBACK: the pane attaches tmux (see
        // ch::TerminalController::tmuxNewSessionCommand), tmux runs on the
        // ALTERNATE screen, and the alternate screen has no scrollback at all —
        // the wheel is reported to tmux, which scrolls its own history. What is
        // left for this buffer is the NORMAL screen: the few lines printed
        // before tmux takes over, and whatever a user sees if tmux exits. A few
        // hundred lines cover both, so this is deliberately small rather than
        // the 5000 it held while xterm.js was believed to own the history:
        // every line costs memory per pane, and there can be many panes.
        scrollback: 500,
        // macOS: Option and drag must make a local selection even while the
        // program in the terminal holds the mouse — the counterpart of Shift
        // and drag everywhere else, and what this page's menu tells a macOS
        // user to do. xterm.js has the behaviour but leaves it OFF by default,
        // so without this line there is no way at all to select text in a pane
        // running tmux on a Mac. Measured on CI's macOS runner: the drag went
        // to the remote side and the page kept no selection.
        macOptionClickForcesSelection: true,
        // The host may replace this immediately after page load through
        // window.applyTheme(); these dark roles are the standalone fallback.
        theme: xtermTheme(activeTheme),
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

    function applyPageTheme(input: unknown): void {
        activeTheme = applyThemeToDocument(element.ownerDocument, input);
        term.options.theme = xtermTheme(activeTheme);
    }
    applyPageTheme(activeTheme);
    term.open(surface);
    // xterm 5 uses the DOM renderer unless an addon replaces it. Preferring
    // WebGL, and surviving the loss of its context, both live in renderer.ts.
    //
    // Recovery from a lost context is a ONE-SHOT RELOAD of this page with WebGL
    // switched off. Swapping the renderer in place is what xterm advises, but
    // disposing an addon whose context has already gone throws from inside
    // xterm and leaves the terminal with no renderer at all — measured, not
    // assumed. The reload is a path the bridge already supports: ch::
    // TerminalBridge::ready() re-announces the connection state and replays the
    // controller's retained output into the fresh page.
    //
    // The flag lives in sessionStorage because it has to outlive the very
    // navigation it triggers while dying with the pane: each WebEngineView has
    // its own, so a later pane still gets its own attempt at the fast renderer.
    const kDomOnlyKey = "codeharbor.terminal.disableWebgl";
    let recoveredFromContextLoss = false;
    try {
        recoveredFromContextLoss =
            pageWindow.sessionStorage?.getItem(kDomOnlyKey) === "1";
    } catch {
        // Storage can be denied outright on a local origin. Without it there is
        // no way to remember the decision across a reload, so reloading would
        // loop; installRenderer falls back in place instead.
    }
    const rendererState = installRenderer(
        term,
        () => new WebglAddon(),
        () => fitToSurface(),
        (message, detail) => {
            if (detail === undefined) {
                console.warn(message);
            } else {
                console.warn(message, detail);
            }
        },
        {
            surface,
            preferDom: recoveredFromContextLoss,
            recover: () => {
                try {
                    if (!pageWindow.sessionStorage
                        || pageWindow.sessionStorage.getItem(kDomOnlyKey) === "1") {
                        return false;
                    }
                    pageWindow.sessionStorage.setItem(kDomOnlyKey, "1");
                } catch {
                    return false;
                }
                console.warn("CodeHarbor terminal is reloading without WebGL");
                // Deferred: this runs inside a browser event handler, and
                // navigating from one is refused by some embedders.
                pageWindow.setTimeout(() => pageWindow.location.reload(), 0);
                return true;
            },
        },
    );

    // The mouse policy — which button belongs to this application and which
    // belongs to the program running in the terminal — is set up further down,
    // next to the copy helper and the focus helper it needs.

    const input = new TerminalInputWriter({
        sendInput: (data) => bridge.sendInput(data),
    });


    // Explicit terminal shortcuts are needed for Ctrl+Shift+C: xterm's
    // keyboard mapper intentionally leaves that combination to the host, and
    // Chromium otherwise treats it as an empty browser command. Cmd+C on macOS
    // follows the same path. Paste is intentionally left to xterm's native
    // Clipboard handler, which brackets the text when the remote application
    // enabled bracketed-paste mode and handles X11 middle-click selection.
    const copySelection = (): boolean => {
        if (!term.hasSelection()) {
            return false;
        }
        const text = term.getSelection();
        if (text.length === 0) {
            return false;
        }
        const textarea = surface.querySelector(".xterm-helper-textarea");
        let copied = false;
        if (textarea instanceof HTMLTextAreaElement) {
            const previousValue = textarea.value;
            const previousStart = textarea.selectionStart;
            const previousEnd = textarea.selectionEnd;
            textarea.value = text;
            textarea.select();
            try {
                copied = element.ownerDocument.execCommand("copy");
            } catch {
                copied = false;
            } finally {
                textarea.value = previousValue;
                if (previousStart !== null && previousEnd !== null) {
                    textarea.setSelectionRange(previousStart, previousEnd);
                }
            }
        }
        if (!copied && pageWindow.navigator.clipboard) {
            void pageWindow.navigator.clipboard.writeText(text).catch(() => {});
            copied = true;
        }
        return copied;
    };
    const isMac = pageWindow.navigator.userAgent.includes("Mac");
    term.attachCustomKeyEventHandler((event) => {
        if (event.type !== "keydown") {
            return true;
        }
        const key = event.key?.toLowerCase() ?? "";
        const copyShortcut = isMac
            ? event.metaKey && !event.ctrlKey && !event.altKey && key === "c"
            : event.ctrlKey && event.shiftKey && !event.altKey && key === "c";
        if (copyShortcut) {
            copySelection();
            // preventDefault() as well as returning false. Returning false only
            // stops xterm.js from turning the key into PTY input; it does NOT
            // suppress the browser's own command, so Cmd+C on macOS would still
            // run Chromium's native copy right after this one and overwrite the
            // clipboard from the (empty) DOM selection — xterm's selection lives
            // in its renderer, not in the document.
            event.preventDefault();
            return false;
        }
        // Do not intercept paste: the browser emits a trusted paste event for
        // Ctrl+Shift+V / Cmd+V, and xterm's handler is where bracketed paste
        // markers are added. Intercepting the key and reading navigator.clipboard
        // would lose the X11 primary-selection middle-click path.
        return true;
    });
    term.onData((data) => input.write(data));

    // Re-fit the terminal to the surface, but ONLY while the surface actually
    // has a box on screen.
    //
    // FitAddon clamps its proposal to a floor of 2 columns by 1 row. So when
    // the pane is collapsed to zero height or zero width — a splitter dragged
    // shut, a tab switched away in a layout that keeps the page alive at size
    // 0 — an unguarded fit() does not skip: it resizes the terminal to 2x1 and
    // that size is forwarded to the remote PTY. tmux then re-wraps every pane
    // in the session to two columns, permanently mangling the scrollback and
    // any full-screen program running in it; restoring the pane restores the
    // size but not the destroyed content. Skipping the fit leaves the last good
    // size in place, and the ResizeObserver fires again the moment the pane
    // regains a box.
    function fitToSurface(): void {
        if (surface.clientWidth === 0 || surface.clientHeight === 0) {
            return;
        }
        fit.fit();
    }
    const applyPreferences = (values: TerminalPreferenceValues): TerminalPreferenceValues =>
        applyTerminalPreferences(
            term,
            fit,
            surface.clientWidth > 0 && surface.clientHeight > 0,
            bridge,
            values,
            (ratio) => pixelRatio.set(ratio),
        );


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
    //   - Mounted into a pane that has no box yet, the fit is skipped and the
    //     size sent here is xterm's 80x24 default; the ResizeObserver corrects
    //     it (exactly once more) as soon as the pane is laid out.
    fitToSurface();
    // Report renderer size changes so C++ can resize the remote PTY (SPEC 5.1).
    term.onResize(({ cols, rows }) => bridge.resize(cols, rows));
    bridge.resize(term.cols, term.rows);

    // A page that replaced a dead renderer starts with an EMPTY screen. The
    // bridge replays whatever output the controller still holds, but everything
    // already handed to the renderer that died is gone, and tmux keeps the
    // visible screen on the alternate buffer where there is nothing to scroll
    // back to. tmux does redraw a client whose size changed, so tell the remote
    // shell about one different size and then the real one: two window-change
    // requests and one full repaint, which is the cheapest way to get the
    // user's screen back without a C++ round trip.
    if (recoveredFromContextLoss && term.cols > 2) {
        bridge.resize(term.cols - 1, term.rows);
        pageWindow.setTimeout(() => bridge.resize(term.cols, term.rows), 120);
    }

    // Re-fit on container resize; fit() emits onResize only when dims change.
    // BOTH boxes are observed, because either one alone misses a real resize:
    //   * element — the pane itself was resized by the host.
    //   * surface — the root keeps its size while the surface changes, because
    //     the status strip above it is display:none in the "ready" state (see
    //     index.html). Observing only the root leaves a row of the grid
    //     reserved for a strip that is no longer on screen.
    // A second fit() for one real resize costs nothing: it re-fits to
    // already-correct dims, which emits no onResize.
    const resizeObserver = new ResizeObserver(fitToSurface);
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
    // Start pessimistically until IntersectionObserver has delivered the
    // element's first real intersection. Otherwise bridge.ready() can release
    // the controller's retained output to a page that mounted off-screen or
    // in a collapsed splitter before this observer's asynchronous callback.
    let intersecting = false;
    function reportVisibility(): void {
        bridge.notifyViewVisible(
            isRendererVisible(intersecting, element.ownerDocument.visibilityState),
        );
    }
    const visibilityObserver = new IntersectionObserver((entries) => {
        for (const entry of entries)
            intersecting = entry.isIntersecting;
        reportVisibility();
    });
    visibilityObserver.observe(element);
    element.ownerDocument.addEventListener("visibilitychange", reportVisibility);
    reportVisibility();

    // Terminal output goes through the coalescing writer rather than straight
    // into term.write(): see writer.ts for why an unthrottled write() can throw
    // and wedge the pane on a large burst, and why the acknowledgement below is
    // what keeps its backlog bounded instead of merely moving the growth here.
    const writer = new CoalescingWriter(
        { write: (data, done) => term.write(data, done) },
        (bytes) => bridge.notifyOutputConsumed(bytes),
    );

    // dispose() can be reached twice — a host that tears the pane down
    // explicitly still has the "pagehide" handler registered in
    // connectTerminal() below — and the WebChannel signals that drive write() /
    // clear() / setConnectionState() stay connected until the page itself is
    // gone. Both are covered by refusing to touch the terminal once it is
    // disposed: xterm.js' write buffer has no disposal guard of its own, so a
    // single late batch of output would throw inside the signal dispatch.
    let disposed = false;
    type TerminalPageApi = typeof pageWindow & {
        applyTheme?: (roles: unknown) => void;
        codeharborSetTerminalPreferences?: (fontPoints: number, pixelRatio: number) => void;
        codeharborFocusTerminal?: () => void;
        codeharborTerminalDiagnostics?: () => Record<string, unknown>;
    };
    const pageApi = pageWindow as TerminalPageApi;
    const applyPagePreferences = (fontPoints: number, preferredRatio: number): void => {
        applyPreferences({ fontSize: fontPoints, pixelRatio: preferredRatio });
    };
    const focusTerminal = (): void => {
        term.focus();
        const textarea = surface.querySelector(".xterm-helper-textarea");
        if (textarea instanceof HTMLTextAreaElement)
            textarea.focus();
    };

    // ---- mouse -------------------------------------------------------------
    // WHAT GOES WHERE. The pane's tmux session is started with mouse reporting
    // on, so xterm.js forwards button presses and wheel turns to it as mouse
    // reports (see ch::TerminalController::tmuxNewSessionCommand for why the
    // wheel needs that). Two consequences are deliberate and left alone:
    //   * the wheel scrolls the session's history, and a program that asked for
    //     the mouse (vim, htop) gets the wheel itself;
    //   * a plain drag belongs to that program, and the page's own selection is
    //     reached with xterm.js's documented modifier — Alt on macOS, Shift
    //     everywhere else. That selection stays until it is replaced, and
    //     nothing is copied without being asked for.
    //
    // The RIGHT button is the exception, and it is taken away from the remote
    // side entirely. Forwarded, it reaches tmux, whose default binding draws a
    // menu of its own inside the grid and then closes it on the very next mouse
    // report — every pointer movement produces one, so that menu cannot be used
    // with a mouse. Swallowing the button in the CAPTURE phase, at the surface,
    // is what stops it: xterm.js listens on descendants of this element (and,
    // mid-drag, on the document, which the event can only reach by bubbling
    // through here), so none of its handlers run and no report is sent.
    const swallowRightButton = (event: MouseEvent): void => {
        if (event.button !== 2) {
            return;
        }
        event.preventDefault();
        event.stopPropagation();
    };
    for (const type of ["mousedown", "mouseup", "auxclick"] as const) {
        surface.addEventListener(type, swallowRightButton, true);
    }

    const menu = new TerminalContextMenu(element, {
        hasSelection: () => term.hasSelection(),
        focusTerminal: () => focusTerminal(),
        isMac,
        run: (action) => {
            if (action === "copy") {
                copySelection();
                return;
            }
            if (action === "select-all") {
                term.selectAll();
                return;
            }
            // Paste. term.paste() is the same path the browser's own paste event
            // takes, so the text is bracketed when the remote program asked for
            // bracketed paste, and it is chunked by the input writer like any
            // other input. Reading the clipboard is asynchronous and can be
            // refused outright (no clipboard permission, an empty clipboard on a
            // headless host); a refusal pastes nothing rather than throwing.
            const clipboard = pageWindow.navigator.clipboard;
            if (!clipboard?.readText) {
                console.warn("CodeHarbor terminal cannot read the clipboard");
                return;
            }
            void clipboard.readText().then(
                (text) => {
                    if (text.length > 0 && !disposed) {
                        term.paste(text);
                    }
                },
                (error: unknown) => console.warn(
                    "CodeHarbor terminal could not read the clipboard", error),
            );
        },
    });
    // The browser's own menu is empty on this page and would cover the terminal;
    // this is where it is replaced rather than merely suppressed. Capture phase
    // and stopPropagation, for the same reason the buttons above are swallowed:
    // xterm.js has a contextmenu handler of its own that selects the word under
    // the pointer (its default on macOS), and selecting something the user did
    // not ask for is exactly what this change is removing.
    const openTerminalMenu = (event: MouseEvent): void => {
        event.preventDefault();
        event.stopPropagation();
        menu.openAt(event.clientX, event.clientY);
    };
    surface.addEventListener("contextmenu", openTerminalMenu, true);
    const terminalDiagnostics = (): Record<string, unknown> => {
        const canvas = surface.querySelector("canvas");
        const canvasRect = canvas?.getBoundingClientRect();
        // Both renderers size xterm's screen element to the grid, but only the
        // canvas ones have a canvas: measuring the screen element is the one
        // way to read the apparent cell size that works under the DOM renderer
        // too, which is what a host without WebGL falls back to.
        const screenRect = surface.querySelector(".xterm-screen")?.getBoundingClientRect();
        // What the user is looking at, read out of xterm's OWN buffer rather
        // than out of the DOM. The .xterm-rows element only carries glyphs
        // while the DOM renderer is active; loading the WebGL addon disposes
        // that renderer and takes the element with it, so a DOM scrape reports
        // an empty screen on exactly the hosts where rendering works best.
        const buffer = term.buffer.active;
        const viewport: string[] = [];
        for (let row = 0; row < term.rows; ++row) {
            viewport.push(buffer.getLine(buffer.viewportY + row)?.translateToString(true) ?? "");
        }
        return {
            renderer: rendererState.name,
            screenText: viewport.join("\n"),
            windowDevicePixelRatio: pageWindow.devicePixelRatio,
            actualDevicePixelRatio: pixelRatio.state.actual,
            effectiveDevicePixelRatio: pixelRatio.state.effective,
            // The grid, and the apparent size of one cell in CSS pixels. The
            // cell size is what a user calls "how big the text is": it must
            // follow the font-size setting and must not move when the pixel
            // ratio changes, which is only checkable if both are reported.
            cols: term.cols,
            rows: term.rows,
            // What the mouse is doing: whether the page holds a selection of
            // its own, and whether this application's menu is on screen.
            hasSelection: term.hasSelection(),
            menuOpen: menu.isOpen,
            fontCssPixels: term.options.fontSize ?? null,
            surfaceCssPixels: {
                width: surface.clientWidth,
                height: surface.clientHeight,
            },
            canvasCssPixels: canvasRect
                ? { width: canvasRect.width, height: canvasRect.height }
                : null,
            canvasDevicePixels: canvas
                ? { width: canvas.width, height: canvas.height }
                : null,
            cellCssPixels: screenRect && term.cols > 0 && term.rows > 0
                ? { width: screenRect.width / term.cols,
                    height: screenRect.height / term.rows }
                : null,
        };
    };
    pageApi.applyTheme = applyPageTheme;
    pageApi.codeharborSetTerminalPreferences = applyPagePreferences;
    pageApi.codeharborFocusTerminal = focusTerminal;
    pageApi.codeharborTerminalDiagnostics = terminalDiagnostics;


    return {
        write(data: string, bytes: number): void {
            // Not acknowledged: a disposed renderer consumed nothing, and the
            // controller has already been told this pane is hidden, so these
            // bytes are its problem to retain and replay to the next renderer.
            if (disposed) {
                return;
            }
            writer.write(data, bytes);
        },
        setConnectionState(state: string): void {
            if (disposed) {
                return;
            }
            status.dataset.state = state;
            status.textContent = state;
        },
        clear(): void {
            if (disposed) {
                return;
            }
            term.clear();
        },
        dispose(): void {
            if (disposed) {
                return;
            }
            disposed = true;
            input.close();
            if (pageApi.applyTheme === applyPageTheme) {
                delete pageApi.applyTheme;
            }
            if (pageApi.codeharborSetTerminalPreferences === applyPagePreferences) {
                delete pageApi.codeharborSetTerminalPreferences;
            }
            if (pageApi.codeharborFocusTerminal === focusTerminal) {
                delete pageApi.codeharborFocusTerminal;
            }
            if (pageApi.codeharborTerminalDiagnostics === terminalDiagnostics) {
                delete pageApi.codeharborTerminalDiagnostics;
            }
            // Observers FIRST. pixelRatio.restore() dispatches a synthetic
            // "resize" at the window to make xterm remeasure, and a live
            // ResizeObserver would answer that by re-fitting a terminal that is
            // already being torn down — which forwards a geometry change to the
            // remote PTY on the way out of a pane the user just closed.
            resizeObserver.disconnect();
            visibilityObserver.disconnect();
            element.ownerDocument.removeEventListener("visibilitychange", reportVisibility);
            surface.removeEventListener("contextmenu", openTerminalMenu, true);
            for (const type of ["mousedown", "mouseup", "auxclick"] as const) {
                surface.removeEventListener(type, swallowRightButton, true);
            }
            menu.dispose();
            pixelRatio.restore();
            writer.close();
            // The renderer is going away (pane closed, or the page is about to
            // be replaced by a reload). The controller must hear about it
            // BEFORE the teardown so output is retained for the next renderer.
            bridge.notifyViewVisible(false);
            term.dispose();
            status.remove();
            surface.remove();
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
 * TerminalBridge half (JS -> C++); WebChannel cannot call a JS function,
 * so the TerminalHost half (C++ -> JS) arrives as SIGNALS which this page wires
 * onto the host returned by mountTerminal().
 */
export interface TerminalChannelObject extends TerminalBridge {
    /** ch::TerminalState as a string (SPEC 5.6), cached by qwebchannel.js. */
    connectionState: string;
    /** A coalesced batch of terminal output, UTF-8 decoded by C++, with the PTY
     *  byte count it was decoded from -> host.write. */
    write: Signal<(data: string, bytes: number) => void>;
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

// A host may call the exported entry point more than once while replacing a
// WebEngine page. Keep one channel handshake per root; otherwise each call
// appends another xterm surface and permanently duplicates every signal.
const connectingElements = new WeakSet<HTMLElement>();


/**
 * Page entry point: open the WebChannel injected by the QML host and mount the
 * terminal against the "terminal" object (the C++ ch::TerminalBridge proxy).
 */
export function connectTerminal(element: HTMLElement): void {
    if (connectingElements.has(element)) {
        return;
    }
    connectingElements.add(element);
    if (typeof QWebChannel === "undefined" || typeof qt === "undefined"
        || !qt.webChannelTransport) {
        connectingElements.delete(element);
        showFatal(element, "This page requires the CodeHarbor host: no WebChannel transport.");
        return;
    }
    new QWebChannel(qt.webChannelTransport, (channel: QWebChannelInstance) => {
        const bridge = channel.objects.terminal as TerminalChannelObject | undefined;
        if (!bridge) {
            connectingElements.delete(element);
            showFatal(element, "The terminal bridge is missing from this window.");
            return;
        }
        // mountTerminal() can throw before a terminal exists — a document with
        // no window, or an xterm.js constructor that refuses the environment.
        // Left uncaught it escapes into qwebchannel.js' connection callback and
        // the pane is a blank black rectangle with nothing anywhere saying why.
        let host: TerminalHost;
        try {
            host = mountTerminal(element, bridge);
        } catch (error) {
            connectingElements.delete(element);
            showFatal(element, `The terminal could not start: ${String(error)}`);
            return;
        }
        // Named, so the same references can be handed back to disconnect()
        // during teardown. An anonymous arrow could never be unsubscribed, and
        // qwebchannel.js keeps every connected handler alive on the proxy for
        // as long as the channel exists.
        const onWrite = (data: string, bytes: number): void => host.write(data, bytes);
        const onConnectionStateChanged = (state: string): void => host.setConnectionState(state);
        const onClearRequested = (): void => host.clear();
        bridge.write.connect(onWrite);
        bridge.connectionStateChanged.connect(onConnectionStateChanged);
        bridge.clearRequested.connect(onClearRequested);
        // The state reached before this page finished loading is already in the
        // property cache, so the strip is correct without waiting for a signal.
        if (typeof bridge.connectionState === "string") {
            host.setConnectionState(bridge.connectionState);
        }
        // A navigated-away or destroyed pane must not leave a ResizeObserver and
        // an IntersectionObserver firing into a dead terminal, and must hand the
        // controller back the job of retaining output (host.dispose() reports the
        // renderer hidden on its way out). The signal handlers go first so that
        // output still in flight from C++ stops reaching this page at all,
        // rather than relying on the host's post-dispose guards.
        // The element's OWN window, not the ambient global: a page that mounts
        // the terminal inside a frame must tear it down when THAT document
        // goes away.
        (element.ownerDocument.defaultView ?? window).addEventListener("pagehide", () => {
            bridge.write.disconnect(onWrite);
            bridge.connectionStateChanged.disconnect(onConnectionStateChanged);
            bridge.clearRequested.disconnect(onClearRequested);
            connectingElements.delete(element);
            host.dispose();
        }, { once: true });
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
