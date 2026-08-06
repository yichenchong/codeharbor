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

// BEL terminates an OSC and NOTHING else. A DCS payload — a sixel image, a
// DECRQSS reply — can legitimately carry a 0x07 byte, so treating that as the
// end cuts the sequence in half and the remote application sees a truncated
// request followed by garbage. This is the same rule the C++ scanner in
// src/terminal/TerminalController.cpp follows; the two must agree.
test("BEL ends an OSC but never a DCS, SOS, PM or APC", () => {
    const osc = "\x1b]0;title\x07after";
    assert.deepEqual(chunkTerminalInput(osc, 4), ["\x1b]0;title\x07", "afte", "r"]);

    for (const introducer of ["P", "X", "^", "_"]) {
        // A 0x07 sits INSIDE the payload; only the ST closes the sequence.
        const sequence = `\x1b${introducer}q\x07data\x1b\\`;
        const chunks = chunkTerminalInput(`${sequence}tail`, 4);
        assert.equal(chunks[0], sequence, `${introducer} was cut at the BEL`);
        assert.equal(chunks.join(""), `${sequence}tail`);
    }
});

// CAN and SUB abandon the sequence in progress. Without them an unterminated
// control string — which is what a paste of arbitrary text can easily contain —
// swallows every remaining byte into one unbounded token, and the whole paste
// is handed over as a single WebChannel call, defeating the pacing.
test("CAN or SUB ends an unterminated control string instead of eating the paste", () => {
    for (const abort of ["\x18", "\x1a"]) {
        const payload = `\x1b]0;never-closed${abort}${"x".repeat(64)}`;
        const chunks = chunkTerminalInput(payload, 8);
        assert.equal(chunks.join(""), payload);
        assert.ok(chunks.length > 1, `${JSON.stringify(abort)} did not end the OSC`);
        assert.equal(chunks[0], `\x1b]0;never-closed${abort}`);

        // Same inside an unterminated CSI. The bound is the aborted sequence's
        // own length, so the token boundary and the chunk boundary coincide and
        // the assertion is about the token.
        const csi = `\x1b[1;2${abort}${"y".repeat(64)}`;
        assert.equal(chunkTerminalInput(csi, 6)[0], `\x1b[1;2${abort}`);
    }
});

// The bound is measured in UTF-8 BYTES, and the measurement is now arithmetic
// rather than a TextEncoder allocation per token. An astral character is four
// bytes in one token (two UTF-16 code units), so a bound of four fits exactly
// one of them and never splits the surrogate pair.
test("chunk bounds are counted in UTF-8 bytes, astral characters included", () => {
    const payload = "😀😀😀";
    const chunks = chunkTerminalInput(payload, 4);
    assert.deepEqual(chunks, ["😀", "😀", "😀"]);
    assert.equal(chunks.join(""), payload);

    // One byte short of a single character: the character is still emitted
    // whole, because there is no smaller safe unit.
    assert.deepEqual(chunkTerminalInput("😀", 3), ["😀"]);

    // Mixed widths add up to the bound exactly: "a" 1 + "é" 2 + "界" 3 = 6.
    assert.deepEqual(chunkTerminalInput("aé界aé界", 6), ["aé界", "aé界"]);
});

// A sink that throws must retain the failed chunk rather than silently lose
// input. A later write is the retry opportunity; close() remains the way to
// abandon it when the bridge is genuinely being torn down.
test("a throwing sink retains the failed chunk for a later retry", async () => {
    const sent: string[] = [];
    let failures = 0;
    let available = false;
    const writer = new TerminalInputWriter({
        sendInput(data) {
            if (!available) {
                failures += 1;
                throw new Error("bridge temporarily unavailable");
            }
            sent.push(data);
        },
    }, 2);

    assert.throws(() => writer.write("abcdefgh"), /bridge temporarily unavailable/);
    assert.equal(failures, 1);
    assert.equal(writer.backlogSize, 8);

    available = true;
    writer.write("!");
    while (writer.backlogSize > 0) {
        await new Promise<void>((resolve) => setImmediate(resolve));
    }
    assert.deepEqual(sent, ["ab", "cd", "ef", "gh", "!"]);
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
