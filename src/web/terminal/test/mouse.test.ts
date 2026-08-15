import { test } from "node:test";
import assert from "node:assert/strict";
import { installInvertedMousePolicy } from "../src/mouse.ts";

type MouseListener = (event: MouseEvent) => void;

/**
 * A stand-in for one of the two DOM nodes the policy attaches to, recording the
 * listeners it registered so a test can hand them an event the way the DOM
 * would.
 *
 * The policy only ever calls addEventListener / removeEventListener and reads
 * ownerDocument, so recorders stand in for the real elements. What is under
 * test is which flag the policy rewrites and when, and that survives the
 * substitution: the events are plain objects, and Object.defineProperty shadows
 * a property on those exactly as it does on a real MouseEvent.
 */
interface ListenerRecorder {
    listeners: Record<string, MouseListener[]>;
    addEventListener(type: string, handler: MouseListener): void;
    removeEventListener(type: string, handler: MouseListener): void;
}

/** A mouse event as the policy sees it: a bag of flags it may rewrite. */
type FakeMouseEvent = Record<string, unknown>;

interface InstalledPolicy {
    surface: ListenerRecorder;
    doc: ListenerRecorder;
    remove: () => void;
    /** Run the recorded listeners for `type`, then return the same event. */
    dispatch(
        where: ListenerRecorder,
        type: string,
        event: FakeMouseEvent,
    ): FakeMouseEvent;
}

function newRecorder(): ListenerRecorder {
    return {
        listeners: {},
        addEventListener(type, handler) {
            (this.listeners[type] ??= []).push(handler);
        },
        removeEventListener(type, handler) {
            this.listeners[type] = (this.listeners[type] ?? []).filter(
                (registered) => registered !== handler,
            );
        },
    };
}

function install(options: {
    isMac: boolean;
    reporting: () => boolean;
}): InstalledPolicy {
    const doc = newRecorder();
    const surface = newRecorder();
    const remove = installInvertedMousePolicy({
        surface: Object.assign(surface, {
            ownerDocument: doc,
        }) as unknown as HTMLElement,
        mouseReportingActive: options.reporting,
        isMac: options.isMac,
    });
    return {
        surface,
        doc,
        remove,
        dispatch(where, type, event) {
            for (const handler of where.listeners[type] ?? []) {
                handler(event as unknown as MouseEvent);
            }
            return event;
        },
    };
}

test("a plain press becomes the forced-selection press xterm looks for", () => {
    const policy = install({ isMac: false, reporting: () => true });
    const down = policy.dispatch(policy.surface, "mousedown", {
        button: 0,
        shiftKey: false,
        altKey: false,
    });
    // shouldForceSelection() reads shiftKey off this event and now sees the
    // modifier the user did not hold, which is what keeps the drag on the page.
    assert.equal(down.shiftKey, true);
    assert.equal(down.altKey, false, "no other modifier is invented");
});

test("a Shift press is handed to the program with the Shift spent", () => {
    const policy = install({ isMac: false, reporting: () => true });
    const down = policy.dispatch(policy.surface, "mousedown", {
        button: 0,
        shiftKey: true,
    });
    assert.equal(down.shiftKey, false);
    // The rest of the gesture is rewritten with it, wherever the pointer goes,
    // so the reports tmux receives are one consistent unmodified drag.
    const move = policy.dispatch(policy.doc, "mousemove", { shiftKey: true });
    const up = policy.dispatch(policy.doc, "mouseup", { shiftKey: true });
    assert.equal(move.shiftKey, false);
    assert.equal(up.shiftKey, false);
    // ...and only for that gesture: the move after the release is untouched.
    const after = policy.dispatch(policy.doc, "mousemove", { shiftKey: true });
    assert.equal(after.shiftKey, true);
});

test("macOS inverts Option, the flag xterm reads there", () => {
    const policy = install({ isMac: true, reporting: () => true });
    const down = policy.dispatch(policy.surface, "mousedown", {
        button: 0,
        shiftKey: false,
        altKey: false,
    });
    assert.equal(down.altKey, true);
    assert.equal(down.shiftKey, false, "Shift keeps its own meaning on a Mac");
});

test("nothing is inverted while no program holds the mouse", () => {
    // With reporting off, xterm's plain drag already selects and Shift means
    // "extend the selection": inverting there would turn every click into an
    // extension of the last selection.
    const policy = install({ isMac: false, reporting: () => false });
    const down = policy.dispatch(policy.surface, "mousedown", {
        button: 0,
        shiftKey: false,
    });
    assert.equal(down.shiftKey, false);
    const move = policy.dispatch(policy.doc, "mousemove", { shiftKey: false });
    assert.equal(move.shiftKey, false);
});

test("only the primary button carries the policy", () => {
    const policy = install({ isMac: false, reporting: () => true });
    const right = policy.dispatch(policy.surface, "mousedown", {
        button: 2,
        shiftKey: false,
    });
    assert.equal(right.shiftKey, false);
    const move = policy.dispatch(policy.doc, "mousemove", { shiftKey: false });
    assert.equal(move.shiftKey, false, "a right press starts no inverted gesture");
});

test("the wheel is never listened for, so scrollback keeps reaching tmux", () => {
    const policy = install({ isMac: false, reporting: () => true });
    assert.deepEqual(Object.keys(policy.surface.listeners), ["mousedown"]);
    assert.deepEqual(Object.keys(policy.doc.listeners), ["mousemove", "mouseup"]);
});

test("removing the policy leaves no listener behind", () => {
    const policy = install({ isMac: false, reporting: () => true });
    policy.remove();
    for (const registered of Object.values(policy.surface.listeners)) {
        assert.equal(registered.length, 0);
    }
    for (const registered of Object.values(policy.doc.listeners)) {
        assert.equal(registered.length, 0);
    }
});
