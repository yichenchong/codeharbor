// Flow control for terminal output, split out from the page entry so it can be
// unit-tested without xterm.js or a DOM (the same split as visibility.ts).
//
// WHY THIS EXISTS: xterm.js' Terminal.write() is fire-and-forget and queues
// everything handed to it. Its write buffer is NOT unbounded — once more than
// 50 MB of unparsed data has accumulated it THROWS
// ("write data discarded, use flow control to avoid losing data"). That
// exception would escape into the QWebChannel signal dispatch that delivered
// the output, so a burst the parser cannot keep up with (`cat` of a large
// file, `yes`, a base64 dump) does not merely lag: it aborts delivery and
// leaves the pane wedged.
//
// The remedy xterm.js documents is exactly what this class implements: keep at
// most ONE chunk inside xterm.js at a time and hand over the next one only when
// the callback for the previous chunk says the parser is done with it.
// Everything that arrives meanwhile is accumulated here and handed over as a
// SINGLE coalesced chunk, which also parses considerably faster than the many
// small chunks the bridge delivers. xterm.js' own pending-data counter
// therefore never grows past one chunk and the 50 MB ceiling is unreachable.
//
// WHAT BOUNDS THE BACKLOG: this class does not drop anything and does not need
// to, because the C++ side will not keep sending. Every chunk the sink consumes
// is reported back to ch::TerminalController through `onConsumed`, and the
// controller emits at most ch::TerminalController::kMaxUnacknowledgedBytes of
// unacknowledged output before it starts retaining the rest in its own rolling
// buffer instead. So the backlog here is bounded by that window plus one
// release batch — the protocol bounds it, not a limit invented in this file,
// which is what keeps the "nothing is ever dropped here" property true: the
// backlog holds exactly the bytes xterm.js has not consumed yet.
//
// A renderer that stops acknowledging (wedged, crashed, mid-teardown) simply
// stops receiving: the controller retains and evicts oldest-first, exactly as
// it does for a hidden pane, and picks up again when acknowledgements resume.

/** The sink this writer drives — xterm.js' Terminal.write(data, callback), or a
 *  fake in tests. `done` MUST be invoked once the sink has consumed `data`. */
export interface WriteSink {
    write(data: string, done: () => void): void;
}

/** Called with the byte weight of a chunk the sink has finished consuming, so
 *  the C++ controller can release the next one (bridge.notifyOutputConsumed).
 *  The weight is the PTY byte count C++ attached to the batch, never a length
 *  measured here: the text is decoded, and re-measuring it would drift on
 *  output that is not valid UTF-8. */
export type ConsumedCallback = (bytes: number) => void;

export class CoalescingWriter {
    private readonly sink: WriteSink;
    private readonly onConsumed: ConsumedCallback;
    /** Output that has arrived but has not been handed to the sink yet. */
    private backlog = "";
    /** Summed byte weight of the batches making up `backlog`. */
    private backlogBytes = 0;
    /** True while the sink still owes us a `done` for the last chunk. */
    private inFlight = false;
    /** Set by close(); every later write is discarded (the sink is gone). */
    private closed = false;

    constructor(sink: WriteSink, onConsumed: ConsumedCallback) {
        this.sink = sink;
        this.onConsumed = onConsumed;
    }

    /** Characters waiting for the sink to catch up. Exposed for tests/diagnostics. */
    get backlogSize(): number {
        return this.backlog.length;
    }

    /** Queue terminal output. Returns immediately; the sink is fed as it drains.
     *  `bytes` is the batch's PTY byte weight, reported back through
     *  `onConsumed` once the sink has consumed it. */
    write(data: string, bytes: number): void {
        if (this.closed || data.length === 0) {
            return;
        }
        this.backlog += data;
        this.backlogBytes += bytes;
        this.pump();
    }

    /** The sink is being disposed. Refuse further writes and drop the backlog so
     *  a `done` arriving during teardown cannot write into a dead terminal.
     *
     *  Nothing is acknowledged on the way out, deliberately: the pane reports
     *  itself hidden as part of the same teardown, which resets the controller's
     *  account and moves it back to retaining output for whichever renderer
     *  shows up next. An acknowledgement here would only credit a renderer that
     *  no longer exists. */
    close(): void {
        this.closed = true;
        this.backlog = "";
        this.backlogBytes = 0;
    }

    private pump(): void {
        if (this.inFlight || this.closed || this.backlog.length === 0) {
            return;
        }
        const chunk = this.backlog;
        const bytes = this.backlogBytes;
        this.backlog = "";
        this.backlogBytes = 0;
        this.inFlight = true;
        this.sink.write(chunk, () => {
            this.inFlight = false;
            // Acknowledge BEFORE pumping: the C++ side may answer the
            // acknowledgement by releasing what it retained, and that arrives
            // as another write() into this same writer. Doing it in this order
            // means the released batch joins a backlog that is already drained
            // rather than one this pump is halfway through.
            if (!this.closed) {
                this.onConsumed(bytes);
            }
            this.pump();
        });
    }
}
