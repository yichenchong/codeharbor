// The pane's mouse policy: which of a drag's two possible meanings — a local
// selection on this page, or a mouse report to the program running in the
// terminal — a PLAIN drag gets, and which one is reached with a modifier.
//
// xterm.js makes that decision in exactly one place. Both gates that can hand a
// button press to the remote side consult
// SelectionService.shouldForceSelection(event)
// (node_modules/@xterm/xterm/src/browser/services/SelectionService.ts:429):
//
//     public shouldForceSelection(event: MouseEvent): boolean {
//       if (Browser.isMac) {
//         return event.altKey && this._optionsService.rawOptions.macOptionClickForcesSelection;
//       }
//       return event.shiftKey;
//     }
//
// Terminal.ts:780 refuses to send the report when it returns true, and
// SelectionService.handleMouseDown():456 refuses to start a selection when it
// returns false while selection is disabled (which is precisely what mouse
// reporting does to it — Terminal.ts:728 calls selectionService.disable() when
// the program asks for the mouse). So the modifier in that one predicate IS the
// policy, and inverting the modifier's SENSE on the mousedown that reaches it
// inverts the policy: a plain drag selects locally and stays highlighted, and
// the modifier hands the gesture to the program.
//
// xterm exposes no hook on that predicate, so the inversion is done where the
// page can reach it: a capture-phase listener that redefines the modifier flag
// on the event object before any xterm listener sees it. This is the same shape
// as the right-button swallow in index.ts, and for the same reason — xterm
// listens on descendants of the surface, so the capture phase there is strictly
// earlier.
//
// The obvious alternative — turning tmux's `mouse on` off (see
// ch::TerminalController::tmuxNewSessionCommand, TerminalController.cpp:814) —
// is wrong. Mouse reporting is what makes a wheel turn scroll tmux's
// scrollback: tmux occupies the ALTERNATE screen, which has no scrollback of
// its own, so without the reports xterm.js falls back to alternate-scroll and
// sends cursor-up/cursor-down keys instead. Taking the mouse away from tmux to
// win back a drag would cost the wheel, which is the whole reason the option is
// set. Nothing here touches wheel events: no wheel listener is registered, and
// the flip is confined to mousedown/mousemove/mouseup of a primary-button
// gesture.

export interface InvertedMousePolicyOptions {
    /** The element `Terminal.open()` was given; a gesture must start inside it. */
    surface: HTMLElement;
    /** True while the program in the terminal has asked for mouse reports —
     *  `Terminal.modes.mouseTrackingMode !== "none"`. */
    mouseReportingActive: () => boolean;
    /** True on macOS. xterm reads Option there and Shift everywhere else, so
     *  that is the flag whose sense has to be inverted. */
    isMac: boolean;
}

/**
 * Invert the sense of the modifier xterm reads in shouldForceSelection() for
 * primary-button gestures that start on `surface`, and return a function that
 * removes the listeners again.
 *
 * The inversion is applied ONLY while mouse reporting is active. With reporting
 * off, xterm's selection is enabled and Shift already has a second, unrelated
 * meaning on mousedown — SelectionService.handleMouseDown():470 extends the
 * existing selection to the click instead of starting a new one — and a page
 * that flipped the flag unconditionally would turn every ordinary click into
 * that extension. Reporting off is also the case where a plain drag already
 * selects, so there is nothing to invert there in the first place.
 *
 * The move and up of an inverted gesture are flipped too, on the document so
 * that they are still caught when the pointer leaves the surface mid-drag. That
 * keeps the modifier bits of a handed-over drag consistent from press to
 * release: the modifier is the chord this page spends to reach the program, and
 * a report stream whose press said "no modifier" and whose drag said "Shift"
 * would be looked up in two different tmux key tables (`MouseDrag1Pane` versus
 * `S-MouseDrag1Pane`, which has no default binding), so the drag would do
 * nothing at all on the remote side.
 */
export function installInvertedMousePolicy(
    options: InvertedMousePolicyOptions,
): () => void {
    const { surface, mouseReportingActive } = options;
    // The flag xterm reads for the force-selection decision on this platform:
    // Option on macOS (with `macOptionClickForcesSelection` on), Shift
    // everywhere else.
    const modifier = options.isMac ? "altKey" : "shiftKey";
    const doc = surface.ownerDocument;
    // Set on a mousedown this policy inverted, so the rest of that one gesture
    // is inverted with it and no other pointer traffic is touched.
    let gestureIsInverted = false;

    const invert = (event: MouseEvent): void => {
        // `shiftKey` / `altKey` are accessors on MouseEvent.prototype; an own
        // data property on the instance shadows them for every later reader,
        // which is the whole trick. It is configurable so a second pass could
        // never wedge the event in an inverted state.
        Object.defineProperty(event, modifier, {
            value: !event[modifier],
            configurable: true,
            enumerable: true,
        });
    };

    const onMouseDown = (event: MouseEvent): void => {
        gestureIsInverted = false;
        // Only the primary button carries the selection-versus-report choice.
        // The right button is taken away from the remote side wholesale in
        // index.ts, and the middle button pastes on the remote side.
        if (event.button !== 0 || !mouseReportingActive()) {
            return;
        }
        gestureIsInverted = true;
        invert(event);
    };
    const onMouseMove = (event: MouseEvent): void => {
        if (gestureIsInverted) {
            invert(event);
        }
    };
    const onMouseUp = (event: MouseEvent): void => {
        if (!gestureIsInverted) {
            return;
        }
        invert(event);
        gestureIsInverted = false;
    };

    surface.addEventListener("mousedown", onMouseDown, true);
    doc.addEventListener("mousemove", onMouseMove, true);
    doc.addEventListener("mouseup", onMouseUp, true);
    return () => {
        surface.removeEventListener("mousedown", onMouseDown, true);
        doc.removeEventListener("mousemove", onMouseMove, true);
        doc.removeEventListener("mouseup", onMouseUp, true);
    };
}
