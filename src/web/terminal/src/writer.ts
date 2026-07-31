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
// No data is ever dropped: the backlog holds exactly the bytes xterm.js has not
// consumed yet, which is the same data that would otherwise be sitting inside
// xterm.js' own queue.

/** The sink this writer drives — xterm.js' Terminal.write(data, callback), or a
 *  fake in tests. `done` MUST be invoked once the sink has consumed `data`. */
export interface WriteSink {
    write(data: string, done: () => void): void;
}

export class CoalescingWriter {
    private readonly sink: WriteSink;
    /** Output that has arrived but has not been handed to the sink yet. */
    private backlog = "";
    /** True while the sink still owes us a `done` for the last chunk. */
    private inFlight = false;
    /** Set by close(); every later write is discarded (the sink is gone). */
    private closed = false;

    constructor(sink: WriteSink) {
        this.sink = sink;
    }

    /** Bytes waiting for the sink to catch up. Exposed for tests/diagnostics. */
    get backlogSize(): number {
        return this.backlog.length;
    }

    /** Queue terminal output. Returns immediately; the sink is fed as it drains. */
    write(data: string): void {
        if (this.closed || data.length === 0) {
            return;
        }
        this.backlog += data;
        this.pump();
    }

    /** The sink is being disposed. Refuse further writes and drop the backlog so
     *  a `done` arriving during teardown cannot write into a dead terminal. */
    close(): void {
        this.closed = true;
        this.backlog = "";
    }

    private pump(): void {
        if (this.inFlight || this.closed || this.backlog.length === 0) {
            return;
        }
        const chunk = this.backlog;
        this.backlog = "";
        this.inFlight = true;
        this.sink.write(chunk, () => {
            this.inFlight = false;
            this.pump();
        });
    }
}
