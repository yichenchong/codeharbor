// Input pacing for terminal keystrokes and clipboard pastes.
//
// A paste can be many megabytes, while a normal key event is only a handful of
// bytes. Sending the paste as one WebChannel call defeats the terminal's
// backpressure boundary and makes Chromium hold the whole value before the
// remote PTY sees its first byte. The chunker keeps calls small, but it never
// cuts an ANSI control sequence or a UTF-8 code point in half. That last rule
// matters for bracketed paste: the opening and closing markers are escape
// sequences themselves and must arrive intact or the shell stays in paste mode.

export const kTerminalInputChunkBytes = 64 * 1024;

function codePointLength(codePoint: number): number {
    return codePoint > 0xffff ? 2 : 1;
}

/**
 * UTF-8 byte length of `text`, computed arithmetically.
 *
 * TextEncoder would allocate a fresh Uint8Array for every token measured — one
 * per character on ordinary text — which is a lot of garbage for a paste the
 * size of a log file. A lone surrogate counts as three bytes because that is
 * what TextEncoder emits for it (U+FFFD).
 */
function utf8Length(text: string): number {
    let bytes = 0;
    for (let index = 0; index < text.length; ++index) {
        const unit = text.charCodeAt(index);
        if (unit < 0x80) {
            bytes += 1;
        } else if (unit < 0x800) {
            bytes += 2;
        } else if (unit >= 0xd800 && unit <= 0xdbff && index + 1 < text.length
            && text.charCodeAt(index + 1) >= 0xdc00
            && text.charCodeAt(index + 1) <= 0xdfff) {
            bytes += 4; // a surrogate PAIR is one four-byte character
            ++index;
        } else {
            bytes += 3;
        }
    }
    return bytes;
}

// CAN and SUB abandon whatever control sequence is in progress and put the
// parser back on ordinary text. This scanner has to agree with the parsers on
// either side of it — @xterm/xterm routes both bytes to GROUND from every
// escape, CSI and string state, and so does the C++ scanner in
// src/terminal/TerminalController.cpp. Without them an unterminated control
// string in a paste swallows the whole remainder of the paste into one token.
const kCancelByte = 0x18;
const kSubstituteByte = 0x1a;

function ansiTokenEnd(text: string, start: number): number {
    if (text.charCodeAt(start) !== 0x1b) {
        const codePoint = text.codePointAt(start) ?? 0xfffd;
        return start + codePointLength(codePoint);
    }

    const introducer = text.charCodeAt(start + 1);
    if (Number.isNaN(introducer)) {
        return start + 1;
    }

    // CSI (ESC [ ... final), including private modes and intermediate bytes.
    if (introducer === 0x5b) {
        for (let index = start + 2; index < text.length; ++index) {
            const byte = text.charCodeAt(index);
            if (byte >= 0x40 && byte <= 0x7e) {
                return index + 1; // CSI final byte
            }
            if (byte === kCancelByte || byte === kSubstituteByte) {
                return index + 1;
            }
            // A new ESC starts a new sequence. Keeping it outside an unfinished
            // CSI is safer than swallowing the rest of the paste as parameters.
            if (byte === 0x1b) {
                return index;
            }
        }
        return text.length;
    }

    // OSC, DCS, SOS, PM and APC are string controls. They end at ST (ESC
    // backslash), not at the first printable byte. BEL terminates an OSC and
    // NOTHING else: a DCS payload (a sixel image, a DECRQSS reply) may
    // legitimately carry a 0x07 byte, and cutting there would split the
    // sequence this function exists to keep whole. Same rule as the C++ scanner
    // in src/terminal/TerminalController.cpp.
    if (introducer === 0x5d || introducer === 0x50 || introducer === 0x58
        || introducer === 0x5e || introducer === 0x5f) {
        const bellTerminates = introducer === 0x5d;
        for (let index = start + 2; index < text.length; ++index) {
            const byte = text.charCodeAt(index);
            if (bellTerminates && byte === 0x07) {
                return index + 1;
            }
            if (byte === kCancelByte || byte === kSubstituteByte) {
                return index + 1;
            }
            if (byte === 0x1b && text.charCodeAt(index + 1) === 0x5c) {
                return index + 2;
            }
        }
        return text.length;
    }

    // ESC Fe sequences may have intermediate bytes (for example ESC ( 0), so
    // do not assume every non-CSI escape ends after two code units. A final
    // byte is 0x30..0x7e; keep intermediates 0x20..0x2f in the same token.
    for (let index = start + 1; index < text.length; ++index) {
        const byte = text.charCodeAt(index);
        if (byte >= 0x30 && byte <= 0x7e) {
            return index + 1;
        }
        if (byte >= 0x20 && byte <= 0x2f) {
            continue;
        }
        if (byte === 0x1b) {
            return index;
        }
        const codePoint = text.codePointAt(index) ?? 0xfffd;
        return index + codePointLength(codePoint);
    }
    return text.length;
}
/**
 * Split terminal input into bounded chunks without cutting a control sequence
 * or a Unicode scalar value. A single control sequence longer than the bound is
 * returned as one chunk: there is no safe smaller chunk for that sequence.
 */
export function chunkTerminalInput(
    text: string,
    maxBytes = kTerminalInputChunkBytes,
): string[] {
    if (text.length === 0) {
        return [];
    }
    if (!Number.isFinite(maxBytes) || !Number.isInteger(maxBytes) || maxBytes < 1) {
        throw new RangeError("maxBytes must be a positive integer");
    }

    const chunks: string[] = [];
    let chunk = "";
    let chunkBytes = 0;
    let index = 0;

    while (index < text.length) {
        const end = ansiTokenEnd(text, index);
        const token = text.slice(index, end);
        const tokenBytes = utf8Length(token);

        if (chunk.length > 0 && chunkBytes + tokenBytes > maxBytes) {
            chunks.push(chunk);
            chunk = "";
            chunkBytes = 0;
        }

        // Do not split an escape sequence simply to satisfy the nominal bound.
        // A pathological OSC can be larger than the window, but sending it as a
        // unit is what keeps xterm.js' parser state valid.
        if (tokenBytes > maxBytes && chunk.length === 0) {
            chunks.push(token);
        } else {
            chunk += token;
            chunkBytes += tokenBytes;
        }
        index = end;
    }

    if (chunk.length > 0) {
        chunks.push(chunk);
    }
    return chunks;
}

export interface TerminalInputSink {
    sendInput(data: string): void;
}

/**
 * Keep one input chunk in the WebChannel call path at a time. There is no
 * acknowledgement slot on the frozen JS -> C++ bridge, so a microtask between
 * chunks is the honest pacing point: it lets Chromium flush the current call
 * and lets a large paste yield to output/resize events without reordering the
 * bytes. The chunker, rather than this queue, owns sequence integrity.
 */
export class TerminalInputWriter {
    private readonly sink: TerminalInputSink;
    private readonly maxBytes: number;
    private readonly backlog: string[] = [];
    private inFlight = false;
    private closed = false;
    constructor(sink: TerminalInputSink, maxBytes = kTerminalInputChunkBytes) {
        if (!Number.isFinite(maxBytes) || !Number.isInteger(maxBytes) || maxBytes < 1) {
            throw new RangeError("maxBytes must be a positive integer");
        }
        this.sink = sink;
        this.maxBytes = maxBytes;
    }

    get backlogSize(): number {
        return this.backlog.reduce((total, chunk) => total + chunk.length, 0);
    }

    write(data: string): void {
        if (this.closed || data.length === 0) {
            return;
        }
        // Appended one at a time rather than spread into push(): a paste split
        // into more chunks than the engine's argument limit (hundreds of
        // thousands, which a small maxBytes reaches easily) makes the spread
        // form throw RangeError and lose the whole paste.
        for (const chunk of chunkTerminalInput(data, this.maxBytes)) {
            this.backlog.push(chunk);
        }
        this.pump();
    }

    close(): void {
        this.closed = true;
        this.backlog.length = 0;
    }

    private pump(): void {
        if (this.closed || this.inFlight || this.backlog.length === 0) {
            return;
        }
        const chunk = this.backlog.shift();
        if (chunk === undefined) {
            return;
        }
        this.inFlight = true;
        let sent = false;
        try {
            this.sink.sendInput(chunk);
            sent = true;
        } catch (error) {
            // A transient WebChannel failure must not silently lose a key or
            // part of a paste. Keep the failed chunk at the front; a later
            // write() can retry it, while close() still discards it during
            // teardown.
            this.backlog.unshift(chunk);
            throw error;
        } finally {
            this.inFlight = false;
            // Do not spin retrying a failed bridge call in microtasks. The
            // next write is the explicit opportunity to retry.
            if (sent && this.backlog.length > 0 && !this.closed) {
                queueMicrotask(() => this.pump());
            }
        }
    }
}
