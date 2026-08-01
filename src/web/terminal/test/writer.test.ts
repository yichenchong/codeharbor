import { test } from "node:test";
import assert from "node:assert/strict";

import { CoalescingWriter, type WriteSink } from "../src/writer.ts";

// A fake xterm.js: it records each chunk handed to it and holds the
// acknowledgement callback so the test decides when the "parser" is done, which
// is how the real Terminal.write(data, callback) behaves (the callback fires on
// a later task, never synchronously).
function fakeSink(): { sink: WriteSink; chunks: string[]; ack(): void } {
    const chunks: string[] = [];
    let pendingAck: (() => void) | undefined;
    return {
        chunks,
        sink: {
            write(data, done) {
                assert.equal(pendingAck, undefined, "sink must never be given two chunks at once");
                chunks.push(data);
                pendingAck = done;
            },
        },
        ack() {
            const done = pendingAck;
            pendingAck = undefined;
            assert.ok(done, "nothing was in flight");
            done();
        },
    };
}

test("the first write goes straight through", () => {
    const { sink, chunks } = fakeSink();
    new CoalescingWriter(sink, () => {}).write("hello", 5);
    assert.deepEqual(chunks, ["hello"]);
});

test("writes arriving while a chunk is in flight are held, then coalesced into one", () => {
    const { sink, chunks, ack } = fakeSink();
    const writer = new CoalescingWriter(sink, () => {});

    writer.write("a", 1);
    // The sink has not acknowledged "a" yet, so nothing more may be handed over
    // — this is the whole point: xterm.js' own queue never grows past one chunk
    // and so can never hit the 50 MB ceiling where write() throws.
    writer.write("b", 1);
    writer.write("c", 1);
    assert.deepEqual(chunks, ["a"]);
    assert.equal(writer.backlogSize, 2);

    ack();
    assert.deepEqual(chunks, ["a", "bc"], "the held writes must be delivered as a single chunk");
    assert.equal(writer.backlogSize, 0);
});

// The C++ controller only releases more output once it hears the renderer
// consumed what it already sent, and it counts in PTY BYTES, not characters —
// so the weights of the batches a chunk coalesced must be summed and reported
// together, exactly once, when that chunk is consumed. Under-reporting starves
// the pane; over-reporting defeats the backpressure.
test("consumption is acknowledged with the summed byte weight of the coalesced batches", () => {
    const { sink, ack } = fakeSink();
    const acked: number[] = [];
    const writer = new CoalescingWriter(sink, (bytes) => acked.push(bytes));

    // "é" is two PTY bytes but one character: the weight is what C++ sent, not
    // anything measured from the decoded text.
    writer.write("é", 2);
    assert.deepEqual(acked, [], "nothing is consumed until the sink says so");

    ack();
    assert.deepEqual(acked, [2]);

    writer.write("x", 1);
    writer.write("yz", 2);
    writer.write("w", 1);
    ack(); // "x" alone was in flight
    ack(); // "yzw" was coalesced behind it
    assert.deepEqual(acked, [2, 1, 3]);
});

// The backlog is bounded by the PROTOCOL, not by anything this class does: the
// controller will not send more than its unacknowledged window, so the backlog
// can only hold that window (plus whatever single batch an acknowledgement
// releases). This models the C++ side to prove the loop actually closes.
test("the backlog stays bounded when the peer honours the acknowledgement window", () => {
    const window = 64; // stand-in for kMaxUnacknowledgedBytes
    const { sink, ack } = fakeSink();

    let unacknowledged = 0;
    let produced = 0;
    let delivered = 0;
    const writer = new CoalescingWriter(sink, (bytes) => {
        unacknowledged -= bytes;
        delivered += bytes;
    });

    // A runaway remote process: it never stops printing. The peer emits only
    // while it is under the window and otherwise retains, so what reaches this
    // writer is throttled.
    let highWater = 0;
    for (let round = 0; round < 500; ++round) {
        while (unacknowledged < window) {
            const batch = "0123456789"; // ten bytes, ten characters
            unacknowledged += batch.length;
            produced += batch.length;
            writer.write(batch, batch.length);
        }
        highWater = Math.max(highWater, writer.backlogSize);
        ack();
    }

    assert.ok(
        highWater <= window,
        `backlog reached ${highWater} characters, past the ${window}-byte window`,
    );
    assert.ok(produced > window * 10, "the test must actually push a lot of output through");
    // Nothing was lost on the way. Every byte is in exactly one of three
    // places: consumed by the sink, still in the backlog, or in the one chunk
    // the sink is chewing on.
    assert.equal(produced - delivered, unacknowledged);
    assert.ok(unacknowledged >= writer.backlogSize);
});

// A renderer that stops consuming must not make the backlog grow without
// bound either — and it cannot, because the peer stops sending. Here the sink
// never acknowledges at all, which is the wedged-renderer case.
test("a sink that never acknowledges receives exactly one chunk and no more", () => {
    const window = 32;
    const { sink, chunks } = fakeSink();

    let unacknowledged = 0;
    const writer = new CoalescingWriter(sink, () => {
        assert.fail("the wedged sink never acknowledges");
    });

    for (let i = 0; i < 1000; ++i) {
        if (unacknowledged >= window) {
            continue; // the peer retains instead, in its own bounded buffer
        }
        unacknowledged += 8;
        writer.write("abcdefgh", 8);
    }

    assert.equal(chunks.length, 1, "xterm.js still holds at most one chunk");
    assert.ok(writer.backlogSize <= window);
});

test("acknowledging an empty backlog does not hand the sink an empty chunk", () => {
    const { sink, chunks, ack } = fakeSink();
    const writer = new CoalescingWriter(sink, () => {});

    writer.write("a", 1);
    ack();
    assert.deepEqual(chunks, ["a"]);

    // ...and the writer is idle again, so the next write goes straight through.
    writer.write("b", 1);
    assert.deepEqual(chunks, ["a", "b"]);
});

test("an empty write is dropped rather than becoming an empty chunk", () => {
    const { sink, chunks } = fakeSink();
    const writer = new CoalescingWriter(sink, () => {});

    writer.write("", 0);
    assert.deepEqual(chunks, []);
    assert.equal(writer.backlogSize, 0);
});

test("close discards the backlog, refuses later writes and acknowledges nothing", () => {
    const { sink, chunks, ack } = fakeSink();
    const acked: number[] = [];
    const writer = new CoalescingWriter(sink, (bytes) => acked.push(bytes));

    writer.write("in-flight", 9);
    writer.write("held", 4);
    writer.close();
    assert.equal(writer.backlogSize, 0, "the backlog must be released on close");

    // The acknowledgement for the chunk that was already in flight arrives
    // during teardown; it must NOT push the held data into a disposed terminal,
    // and it must not credit a renderer that no longer exists — the pane
    // reports itself hidden on the same path, which resets the controller.
    ack();
    writer.write("after-close", 11);
    assert.deepEqual(chunks, ["in-flight"]);
    assert.deepEqual(acked, []);
});

test("a sink that acknowledges synchronously still drains in order", () => {
    // Guards the re-entrancy path: pump() is re-entered from inside sink.write.
    const chunks: string[] = [];
    const acked: number[] = [];
    const writer = new CoalescingWriter(
        {
            write(data, done) {
                chunks.push(data);
                done();
            },
        },
        (bytes) => acked.push(bytes),
    );

    writer.write("one", 3);
    writer.write("two", 3);
    assert.deepEqual(chunks, ["one", "two"]);
    assert.deepEqual(acked, [3, 3]);
    assert.equal(writer.backlogSize, 0);
});
