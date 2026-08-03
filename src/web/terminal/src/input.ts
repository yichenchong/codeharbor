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

function isCsiFinal(byte: number): boolean {
    return byte >= 0x40 && byte <= 0x7e;
}

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
            if (isCsiFinal(byte)) {
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

    // OSC, DCS, SOS, PM and APC are string controls. They end at BEL or ST
    // (ESC backslash), not at the first printable byte.
    if (introducer === 0x5d || introducer === 0x50 || introducer === 0x58
        || introducer === 0x5e || introducer === 0x5f) {
        for (let index = start + 2; index < text.length; ++index) {
            const byte = text.charCodeAt(index);
            if (byte === 0x07) {
                return index + 1;
            }
            if (byte === 0x1b && text.charCodeAt(index + 1) === 0x5c) {
                return index + 2;
            }
        }
        return text.length;
    }

    // Two-byte ESC Fe sequences (including the common ESC c / ESC 7 / ESC 8
    // controls). The trailing byte is ASCII by definition, so this also keeps
    // a Unicode code point following ESC from being separated accidentally.
    return Math.min(text.length, start + 2);
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
    if (!Number.isFinite(maxBytes) || maxBytes < 1) {
        throw new RangeError("maxBytes must be a positive finite number");
    }

    const encoder = new TextEncoder();
    const chunks: string[] = [];
    let chunk = "";
    let chunkBytes = 0;
    let index = 0;

    while (index < text.length) {
        const end = ansiTokenEnd(text, index);
        const token = text.slice(index, end);
        const tokenBytes = encoder.encode(token).byteLength;

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
        if (!Number.isFinite(maxBytes) || maxBytes < 1) {
            throw new RangeError("maxBytes must be a positive finite number");
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
        this.backlog.push(...chunkTerminalInput(data, this.maxBytes));
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
        try {
            this.sink.sendInput(chunk);
        } finally {
            this.inFlight = false;
        }
        if (this.backlog.length > 0 && !this.closed) {
            queueMicrotask(() => this.pump());
        }
    }
}
