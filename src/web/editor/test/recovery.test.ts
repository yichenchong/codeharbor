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
