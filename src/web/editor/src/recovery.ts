// Debounced crash-recovery snapshotting and the save path that must cancel it
// (SPEC 11.3, and the WB4 fix). Split out from the page entry so the timer
// interaction can be unit-tested with a fake bridge and fake timer, without a
// Monaco editor or a DOM.

/** Delay before a pending edit is snapshotted to the host (SPEC 11.3). */
export const REPORT_DELAY_MS = 500;

/** The subset of the editor bridge this reporter drives. */
export interface ReportBridge {
    /** Push the current buffer so the host can snapshot it for crash recovery. */
    reportContent(content: string): void;
    /** Persist the buffer guarded by the revision it was loaded at. */
    save(content: string, expectedRevision: string): void;
}

/** Timer seam so tests can drive the debounce deterministically. Defaults to
 *  the host environment's setTimeout/clearTimeout (which return a number in the
 *  browser this page runs in). */
export interface ReportTimers {
    set(handler: () => void, ms: number): number;
    clear(handle: number): void;
}

const defaultTimers: ReportTimers = {
    set: (handler, ms) => setTimeout(handler, ms) as unknown as number,
    clear: (handle) => clearTimeout(handle),
};

/**
 * Owns the single pending crash-recovery snapshot timer and the save path.
 * Cancelling the pending snapshot is part of saving, not an optimisation:
 * reportContent() marks the file dirty on the host and rewrites the recovery
 * snapshot, so a timer allowed to fire AFTER the save succeeded would resurrect
 * a stale "unsaved changes" copy of an already-saved file (the host discards the
 * snapshot on a successful save) and would leave the buffer flagged dirty, which
 * suppresses the automatic reload of a clean buffer on an external change
 * (SPEC 8.7). A save that FAILS re-arms it via schedule(): those edits really
 * are still unsaved.
 */
export class RecoveryReporter {
    private timer: number | undefined = undefined;
    private readonly bridge: ReportBridge;
    private readonly getValue: () => string;
    private readonly timers: ReportTimers;

    constructor(bridge: ReportBridge, getValue: () => string, timers: ReportTimers = defaultTimers) {
        this.bridge = bridge;
        this.getValue = getValue;
        this.timers = timers;
    }

    /** Whether a snapshot is currently armed — which is the same thing as "the
     *  host has NOT been told about the edits in this buffer yet", since every
     *  path that delivers the buffer (schedule's timer firing, report, flush,
     *  save) disarms it. The editor page reads it that way when it decides
     *  whether host-driven content may replace the buffer. */
    get pending(): boolean {
        return this.timer !== undefined;
    }

    cancel(): void {
        if (this.timer !== undefined) {
            this.timers.clear(this.timer);
            this.timer = undefined;
        }
    }

    /** Arm the debounced snapshot, but only when the buffer is dirty: snapshotting
     *  a clean buffer would flag the file dirty on the host for no reason, and a
     *  dirty flag suppresses the automatic reload of a clean buffer (SPEC 8.7). */
    schedule(dirty: boolean): void {
        this.cancel();
        if (!dirty) {
            return;
        }
        this.timer = this.timers.set(() => {
            this.timer = undefined;
            this.bridge.reportContent(this.getValue());
        }, REPORT_DELAY_MS);
    }

    /** Send the buffer to the host NOW, whether or not a timer is armed, and
     *  disarm the one that is. Used when the buffer is KNOWN to diverge from the
     *  file the host believes it holds:
     *
     *    * an edit that is later undone during a save leaves the buffer clean
     *      against the OLD baseline, so the edit handler scheduled nothing, yet
     *      after the save lands those bytes are no longer the file's bytes;
     *    * a host-driven reload the page REFUSES because the buffer holds edits
     *      the debounce had not delivered yet — the host must learn about them at
     *      once, both to hold the only copy and to stop reloading over them. */
    report(): void {
        this.cancel();
        this.bridge.reportContent(this.getValue());
    }

    /** Send a still-pending snapshot immediately (teardown / retarget). No-op
     *  when nothing is pending. */
    flush(): void {
        if (this.timer === undefined) {
            return;
        }
        this.report();
    }

    /** Persist the buffer guarded by `expectedRevision`, cancelling any pending
     *  snapshot first so a late timer cannot resurrect a stale recovery copy.
     *
     *  `content` defaults to reading the buffer, and a caller that has ALREADY
     *  taken those bytes passes them in: a whole-buffer read can be megabytes,
     *  and two independent reads are also two chances for the bytes recorded as
     *  "handed to the host" to differ from the ones actually sent. */
    save(expectedRevision: string, content = this.getValue()): void {
        this.cancel();
        this.bridge.save(content, expectedRevision);
    }
}
