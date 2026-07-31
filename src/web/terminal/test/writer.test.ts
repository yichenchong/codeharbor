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
    new CoalescingWriter(sink).write("hello");
    assert.deepEqual(chunks, ["hello"]);
});

test("writes arriving while a chunk is in flight are held, then coalesced into one", () => {
    const { sink, chunks, ack } = fakeSink();
    const writer = new CoalescingWriter(sink);

    writer.write("a");
    // The sink has not acknowledged "a" yet, so nothing more may be handed over
    // — this is the whole point: xterm.js' own queue never grows past one chunk
    // and so can never hit the 50 MB ceiling where write() throws.
    writer.write("b");
    writer.write("c");
    assert.deepEqual(chunks, ["a"]);
    assert.equal(writer.backlogSize, 2);

    ack();
    assert.deepEqual(chunks, ["a", "bc"], "the held writes must be delivered as a single chunk");
    assert.equal(writer.backlogSize, 0);
});

test("acknowledging an empty backlog does not hand the sink an empty chunk", () => {
    const { sink, chunks, ack } = fakeSink();
    const writer = new CoalescingWriter(sink);

    writer.write("a");
    ack();
    assert.deepEqual(chunks, ["a"]);

    // ...and the writer is idle again, so the next write goes straight through.
    writer.write("b");
    assert.deepEqual(chunks, ["a", "b"]);
});

test("an empty write is dropped rather than becoming an empty chunk", () => {
    const { sink, chunks } = fakeSink();
    const writer = new CoalescingWriter(sink);

    writer.write("");
    assert.deepEqual(chunks, []);
    assert.equal(writer.backlogSize, 0);
});

test("close discards the backlog and refuses every later write", () => {
    const { sink, chunks, ack } = fakeSink();
    const writer = new CoalescingWriter(sink);

    writer.write("in-flight");
    writer.write("held");
    writer.close();
    assert.equal(writer.backlogSize, 0, "the backlog must be released on close");

    // The acknowledgement for the chunk that was already in flight arrives
    // during teardown; it must NOT push the held data into a disposed terminal.
    ack();
    writer.write("after-close");
    assert.deepEqual(chunks, ["in-flight"]);
});

test("a sink that acknowledges synchronously still drains in order", () => {
    // Guards the re-entrancy path: pump() is re-entered from inside sink.write.
    const chunks: string[] = [];
    const writer = new CoalescingWriter({
        write(data, done) {
            chunks.push(data);
            done();
        },
    });

    writer.write("one");
    writer.write("two");
    assert.deepEqual(chunks, ["one", "two"]);
    assert.equal(writer.backlogSize, 0);
});
