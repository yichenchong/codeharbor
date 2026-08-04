import { test } from "node:test";
import assert from "node:assert/strict";

import { chunkTerminalInput, TerminalInputWriter } from "../src/input.ts";

function utf8ByteLength(text: string): number {
    return new TextEncoder().encode(text).byteLength;
}

function assertEscapeSequencesStayInOneChunk(
    chunks: string[],
    sequences: string[] = ["\x1b[200~", "\x1b[31m", "\x1b]0;title\x07", "\x1b[201~"],
): void {
    for (const sequence of sequences) {
        const containing = chunks.filter((chunk) => chunk.includes(sequence));
        assert.equal(containing.length, 1, `${JSON.stringify(sequence)} was split`);
    }
}

test("large multi-byte paste is chunked without splitting UTF-8 or ANSI sequences", () => {
    const payload = "é界".repeat(32)
        + "\x1b[200~"
        + "paste-contents-世界"
        + "\x1b]0;title\x07"
        + "\x1b[31mred\x1b[0m"
        + "\x1b[201~";
    const chunks = chunkTerminalInput(payload, 64);

    assert.ok(chunks.length > 1, "the fixture must cross the chunk boundary");
    assert.equal(chunks.join(""), payload);
    for (const chunk of chunks) {
        assert.ok(utf8ByteLength(chunk) <= 64 || chunk.startsWith("\x1b"));
        new TextDecoder("utf-8", { fatal: true }).decode(new TextEncoder().encode(chunk));
    }
    assertEscapeSequencesStayInOneChunk(chunks);
});

test("an ESC followed by a Unicode scalar keeps the scalar intact", () => {
    const payload = "\x1b😀";
    const chunks = chunkTerminalInput(payload, 2);
    assert.deepEqual(chunks, [payload]);
    assert.equal(new TextDecoder("utf-8", { fatal: true }).decode(
        new TextEncoder().encode(chunks[0]),
    ), payload);
});

test("ESC sequences with intermediate bytes stay in one chunk", () => {
    const payload = "\x1b(0";
    assert.deepEqual(chunkTerminalInput(payload, 2), [payload]);
});

test("fractional chunk bounds are rejected", () => {
    assert.throws(() => chunkTerminalInput("data", 1.5), RangeError);
    assert.throws(() => new TerminalInputWriter({ sendInput() {} }, 1.5), RangeError);
});

test("a paste larger than one flow-control window drains in order", async () => {
    const sent: string[] = [];
    const writer = new TerminalInputWriter({
        sendInput(data) {
            sent.push(data);
        },
    }, 128);
    const payload = ("界-".repeat(1024) + "\x1b[?2004h")
        + "middle"
        + "\x1b[?2004l";

    writer.write(payload);
    assert.ok(sent.length > 0, "the first chunk should be sent immediately");
    while (writer.backlogSize > 0) {
        await new Promise<void>((resolve) => setImmediate(resolve));
    }

    assertEscapeSequencesStayInOneChunk(sent, ["\x1b[?2004h", "\x1b[?2004l"]);
    assert.ok(sent.length > 8, "the fixture must cross several input windows");
});

test("invalid chunk bounds are rejected instead of disabling pacing", () => {
    assert.throws(() => chunkTerminalInput("data", 0), RangeError);
    assert.throws(() => new TerminalInputWriter({ sendInput() {} }, Number.NaN), RangeError);
});
