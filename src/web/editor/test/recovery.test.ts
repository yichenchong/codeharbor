import { test } from "node:test";
import assert from "node:assert/strict";

import { RecoveryReporter, type ReportBridge, type ReportTimers } from "../src/recovery.ts";

// A fake timer: setTimeout hands back an id and remembers the handler; fire()
// runs every armed handler, standing in for the 500 ms elapsing. This drives
// the debounce deterministically instead of sleeping.
class FakeClock {
    private handlers = new Map<number, () => void>();
    private nextId = 1;

    readonly timers: ReportTimers = {
        set: (handler) => {
            const id = this.nextId++;
            this.handlers.set(id, handler);
            return id;
        },
        clear: (handle) => {
            this.handlers.delete(handle);
        },
    };

    get armed(): number {
        return this.handlers.size;
    }

    fire(): void {
        const pending = [...this.handlers.values()];
        this.handlers.clear();
        for (const handler of pending) {
            handler();
        }
    }
}

// A fake editor bridge recording the two calls RecoveryReporter makes.
function fakeBridge(): {
    bridge: ReportBridge;
    reports: string[];
    saves: Array<{ content: string; revision: string }>;
} {
    const reports: string[] = [];
    const saves: Array<{ content: string; revision: string }> = [];
    return {
        reports,
        saves,
        bridge: {
            reportContent: (content) => reports.push(content),
            save: (content, revision) => saves.push({ content, revision }),
        },
    };
}

test("a save cancels the pending snapshot so no stale report fires afterwards", () => {
    const clock = new FakeClock();
    const { bridge, reports, saves } = fakeBridge();
    let buffer = "typed";
    const reporter = new RecoveryReporter(bridge, () => buffer, clock.timers);

    reporter.schedule(true); // user edit armed a snapshot
    assert.equal(clock.armed, 1);
    assert.equal(reporter.pending, true);

    reporter.save("rev-1"); // Ctrl+S within the debounce window
    assert.equal(reporter.pending, false, "save must cancel the pending snapshot");
    assert.deepEqual(saves, [{ content: "typed", revision: "rev-1" }]);

    buffer = "typed-more";
    clock.fire(); // the 500 ms would have elapsed here
    assert.deepEqual(reports, [], "the cancelled snapshot must not fire after a save");
});

test("a failed save re-arms the snapshot; the next tick reports the buffer", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    let buffer = "unsaved edits";
    const reporter = new RecoveryReporter(bridge, () => buffer, clock.timers);

    reporter.schedule(true);
    reporter.save("rev-1"); // save goes out, cancelling the snapshot

    // The host reports a conflict/error: those edits are still unsaved, so the
    // conflict/error handler re-arms the snapshot.
    reporter.schedule(true);
    assert.equal(reporter.pending, true, "a failed save must re-arm the snapshot");

    clock.fire();
    assert.deepEqual(reports, ["unsaved edits"], "the re-armed snapshot must fire");
});

test("a successful save leaves nothing armed, so no snapshot is resurrected", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    const reporter = new RecoveryReporter(bridge, () => "saved", clock.timers);

    reporter.schedule(true);
    reporter.save("rev-1");
    // Success path does NOT re-arm.
    clock.fire();
    assert.deepEqual(reports, [], "a successful save must not leave a pending snapshot");
});

test("schedule refuses to arm when the buffer is not dirty", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    const reporter = new RecoveryReporter(bridge, () => "clean", clock.timers);

    reporter.schedule(false);
    assert.equal(reporter.pending, false);
    clock.fire();
    assert.deepEqual(reports, [], "a clean buffer must never be snapshotted");
});

test("flush sends a pending snapshot immediately and disarms the timer", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    const reporter = new RecoveryReporter(bridge, () => "last-half-second", clock.timers);

    reporter.schedule(true);
    reporter.flush();
    assert.deepEqual(reports, ["last-half-second"]);
    assert.equal(reporter.pending, false);

    clock.fire();
    assert.deepEqual(reports, ["last-half-second"], "flush must not leave the timer armed");
});

test("flush is a no-op when nothing is pending", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    const reporter = new RecoveryReporter(bridge, () => "x", clock.timers);

    reporter.flush();
    assert.deepEqual(reports, []);
});

test("re-scheduling replaces the armed timer instead of stacking a second one", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    const reporter = new RecoveryReporter(bridge, () => "final", clock.timers);

    // Every keystroke calls schedule(); the debounce only works if each call
    // cancels the previous timer rather than adding one.
    reporter.schedule(true);
    reporter.schedule(true);
    reporter.schedule(true);
    assert.equal(clock.armed, 1, "keystrokes must not stack timers");

    clock.fire();
    assert.deepEqual(reports, ["final"], "exactly one snapshot per debounce window");
});

test("the snapshot carries the buffer as of the moment the timer fires", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    let buffer = "at schedule time";
    const reporter = new RecoveryReporter(bridge, () => buffer, clock.timers);

    reporter.schedule(true);
    buffer = "at fire time";
    clock.fire();
    assert.deepEqual(reports, ["at fire time"]);
});

test("cancel disarms without reporting, and leaves the reporter reusable", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    const reporter = new RecoveryReporter(bridge, () => "x", clock.timers);

    reporter.schedule(true);
    reporter.cancel();
    assert.equal(reporter.pending, false);
    assert.equal(clock.armed, 0, "cancel must release the timer, not just forget it");
    clock.fire();
    assert.deepEqual(reports, []);

    // cancel() is also a no-op when nothing is armed (contentLoaded calls it
    // unconditionally).
    reporter.cancel();
    reporter.schedule(true);
    clock.fire();
    assert.deepEqual(reports, ["x"]);
});

test("schedule(false) disarms an already-armed snapshot", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    const reporter = new RecoveryReporter(bridge, () => "x", clock.timers);

    reporter.schedule(true);
    // The conflict/error handlers call schedule(dirty) with whatever the flag
    // currently is; a clean buffer must end up with nothing armed.
    reporter.schedule(false);
    assert.equal(reporter.pending, false);
    clock.fire();
    assert.deepEqual(reports, []);
});

test("save sends the buffer as of the save, guarded by the given revision", () => {
    const clock = new FakeClock();
    const { bridge, saves } = fakeBridge();
    let buffer = "one";
    const reporter = new RecoveryReporter(bridge, () => buffer, clock.timers);

    reporter.save("rev-1");
    buffer = "two";
    reporter.save("rev-2");
    assert.deepEqual(saves, [
        { content: "one", revision: "rev-1" },
        { content: "two", revision: "rev-2" },
    ]);
});

test("save sends the bytes it is GIVEN rather than re-reading the buffer", () => {
    const clock = new FakeClock();
    const { bridge, saves } = fakeBridge();
    let reads = 0;
    const reporter = new RecoveryReporter(bridge, () => {
        ++reads;
        return "a second read of the buffer";
    }, clock.timers);

    // The page has already taken the bytes (it records them as "handed to the
    // host" so a successful reply can re-baseline against them), so the save
    // must carry exactly those and read the buffer no further times.
    reporter.schedule(true);
    reporter.save("rev-1", "the bytes the page captured");
    assert.deepEqual(saves, [
        { content: "the bytes the page captured", revision: "rev-1" },
    ]);
    assert.equal(reads, 0, "the buffer was read again for bytes the caller supplied");
    assert.equal(reporter.pending, false, "save must still cancel the snapshot");
});


test("report sends immediately even when no debounce timer is armed", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    const reporter = new RecoveryReporter(bridge, () => "undone-during-save", clock.timers);

    reporter.report();
    assert.deepEqual(reports, ["undone-during-save"]);
    assert.equal(reporter.pending, false);
});

test("report replaces a pending timer instead of sending twice", () => {
    const clock = new FakeClock();
    const { bridge, reports } = fakeBridge();
    const reporter = new RecoveryReporter(bridge, () => "latest", clock.timers);

    reporter.schedule(true);
    reporter.report();
    clock.fire();
    assert.deepEqual(reports, ["latest"]);
    assert.equal(clock.armed, 0);
});