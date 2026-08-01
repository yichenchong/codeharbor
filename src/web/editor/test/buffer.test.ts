import { test } from "node:test";
import assert from "node:assert/strict";

import {
    createTextBufferFactory,
    type TextBuffer,
} from "monaco-editor/esm/vs/editor/common/model/textModel.js";

import { bufferText, type BufferSource } from "../src/buffer.ts";

/**
 * A Monaco text model reduced to the two operations the editor page performs on
 * it — set the loaded file, read the buffer back for a save — over the REAL
 * piece-tree buffer Monaco builds from a string.
 *
 * Both bodies are Monaco's own: `setValue` is what `TextModel.setValue` does
 * (createTextBuffer with a default EOL), and `getValue` is
 * `CodeEditorWidget.getValue` mapping its options onto
 * `TextModel.getValue(eolPreference, preserveBOM)`. The full standalone editor
 * cannot be constructed outside a browser, but nothing in it sits between those
 * two calls and the buffer, so this exercises exactly the machinery that decides
 * whether a byte-order mark and a file's line endings survive a save.
 */
class MonacoModel implements BufferSource {
    private buffer: TextBuffer;

    constructor(text: string) {
        this.buffer = MonacoModel.build(text);
    }

    setValue(text: string): void {
        this.buffer = MonacoModel.build(text);
    }

    getValue(options?: { preserveBOM: boolean; lineEnding: string } | null): string {
        const preserveBOM = options?.preserveBOM === true;
        let eol = 0; // EndOfLinePreference.TextDefined
        if (options?.lineEnding === "\n") {
            eol = 1;
        } else if (options?.lineEnding === "\r\n") {
            eol = 2;
        }
        const whole = this.buffer.getRangeAt(0, this.buffer.getLength());
        const value = this.buffer.getValueInRange(whole, eol);
        return preserveBOM ? this.buffer.getBOM() + value : value;
    }

    private static build(text: string): TextBuffer {
        // 1 = DefaultEndOfLine.LF, the default Monaco creates editors with. It
        // applies only to text that has no line ending to detect.
        return createTextBufferFactory(text).create(1).textBuffer;
    }
}

const BOM = "\uFEFF";

test("a CRLF file comes back out with its CRLF line endings", () => {
    const model = new MonacoModel("alpha\r\nbeta\r\n");
    assert.equal(bufferText(model), "alpha\r\nbeta\r\n");
});

test("an LF file is not promoted to CRLF", () => {
    const model = new MonacoModel("alpha\nbeta\n");
    assert.equal(bufferText(model), "alpha\nbeta\n");
});

test("a byte-order mark survives the round trip", () => {
    const model = new MonacoModel(`${BOM}alpha\nbeta\n`);
    assert.equal(bufferText(model), `${BOM}alpha\nbeta\n`);
});

test("Monaco's default accessor is the one that loses the byte-order mark", () => {
    // This is the defect the rule exists for, pinned so nobody "simplifies"
    // bufferText() back to a bare getValue(): the mark is not part of the text
    // Monaco holds, so the default accessor writes the file back without it.
    const model = new MonacoModel(`${BOM}alpha\nbeta\n`);
    assert.equal(model.getValue(), "alpha\nbeta\n");
    assert.notEqual(model.getValue(), bufferText(model));
});

test("a mark and CRLF endings survive together", () => {
    const model = new MonacoModel(`${BOM}alpha\r\nbeta\r\n`);
    assert.equal(bufferText(model), `${BOM}alpha\r\nbeta\r\n`);
});

test("a file with no trailing newline keeps its shape", () => {
    assert.equal(bufferText(new MonacoModel("alpha\r\nbeta")), "alpha\r\nbeta");
    assert.equal(bufferText(new MonacoModel("no newline at all")), "no newline at all");
    assert.equal(bufferText(new MonacoModel("")), "");
});

test("a mark on an otherwise empty file is still a mark", () => {
    assert.equal(bufferText(new MonacoModel(BOM)), BOM);
});

test("a reload replaces both the mark and the line endings", () => {
    // The page calls setValue() on every host-driven load, so the rule has to
    // track the NEW file rather than the one the model was created with.
    const model = new MonacoModel(`${BOM}alpha\r\n`);
    model.setValue("plain\n");
    assert.equal(bufferText(model), "plain\n");
    model.setValue(`${BOM}back\r\nagain\r\n`);
    assert.equal(bufferText(model), `${BOM}back\r\nagain\r\n`);
});
