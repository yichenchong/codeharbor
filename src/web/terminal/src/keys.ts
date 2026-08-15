// The terminal's clipboard key policy, as a pure decision so it can be tested
// without a DOM. `mountTerminal` supplies the two facts it cannot derive from
// the event - which platform this is, and whether the terminal currently holds
// a selection - and acts on the answer.

/** The modifier/key facts a clipboard decision needs from a KeyboardEvent. */
export interface KeyChord {
    /** `KeyboardEvent.key`, in any case; compared lowercased. */
    key: string;
    ctrlKey: boolean;
    shiftKey: boolean;
    altKey: boolean;
    metaKey: boolean;
}

export interface KeyContext {
    isMac: boolean;
    hasSelection: boolean;
}

export type KeyAction =
    /** Not ours: xterm turns it into PTY input. */
    | "pass"
    /** Copy the selection and keep it. */
    | "copy"
    /**
     * Copy the selection and drop it, so the next press of the same key means
     * the interrupt again.
     */
    | "copy-and-clear"
    /**
     * Ours only in the sense that xterm must NOT consume it: the caller returns
     * false WITHOUT preventDefault(), leaving the browser's native paste to
     * fire so xterm's own Clipboard handler brackets the text.
     */
    | "paste";

/**
 * Decide what a keydown means for the clipboard.
 *
 * COPY. `Ctrl+Shift+C` (`Cmd+C` on macOS) always copies when there is anything
 * to copy. Plain `Ctrl+C` copies only when a selection exists - otherwise it is
 * the interrupt, which must keep working - and copying then clears the
 * selection so the very next press interrupts. A forgotten selection therefore
 * costs one keypress, never more. macOS is excluded from that rule: there
 * `Ctrl+C` is the interrupt and `Cmd+C` is copy, two separate keys with nothing
 * to disambiguate.
 *
 * PASTE. Plain `Ctrl+V` pastes, matching `Ctrl+C` above and every other desktop
 * application; `Ctrl+Shift+V` keeps working for existing muscle memory. The
 * cost, stated plainly: `Ctrl+V` no longer reaches the remote program as the
 * `0x16` byte, which is readline's `quoted-insert` and vi's literal-next. That
 * is the deliberate trade for a consistent pair. On macOS `Cmd+V` is already
 * the paste key and plain `Ctrl+V` stays the control byte.
 */
export function clipboardKeyAction(event: KeyChord, context: KeyContext): KeyAction {
    const key = event.key?.toLowerCase() ?? "";

    if (key === "v") {
        // Cmd+V on macOS already reaches the browser as a paste command; it is
        // the CONTROL key this reroutes, and only off macOS.
        if (!context.isMac && event.ctrlKey && !event.altKey && !event.metaKey) {
            return "paste";
        }
        return "pass";
    }

    if (key !== "c") {
        return "pass";
    }

    const explicitCopy = context.isMac
        ? event.metaKey && !event.ctrlKey && !event.altKey
        : event.ctrlKey && event.shiftKey && !event.altKey;
    if (explicitCopy) {
        // Consumed even with nothing selected, which is what the handler did
        // before this decision was factored out of it. `copy` on an empty
        // selection is a no-op by construction, and the alternative is worse:
        // letting the chord through hands Ctrl+Shift+C to xterm and Chromium as
        // an ordinary key, so a dedicated copy shortcut would sometimes reach
        // the remote program. A key that means "copy" must never mean anything
        // else just because there was nothing to copy.
        return "copy";
    }

    const copyInsteadOfInterrupt = !context.isMac
        && event.ctrlKey && !event.shiftKey && !event.altKey && !event.metaKey
        && context.hasSelection;
    return copyInsteadOfInterrupt ? "copy-and-clear" : "pass";
}
