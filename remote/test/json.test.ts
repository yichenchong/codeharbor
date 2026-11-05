import { test } from "node:test";
import assert from "node:assert/strict";
import { JsonDepthLimitError, JsonSizeLimitError, stringifyJson } from "../src/json.ts";

test("bounded JSON serialization counts encoded UTF-8 and escaping at the byte boundary", () => {
    const value = { text: "é\n\u0000😀", items: [true, null, 42] };
    const bytes = Buffer.byteLength(JSON.stringify(value));
    const serialized = stringifyJson(value, bytes);
    assert.equal(typeof serialized, "string");
    assert.deepEqual(JSON.parse(serialized!), value);
    assert.equal(Buffer.byteLength(serialized!), bytes);
    assert.throws(
        () => stringifyJson(value, bytes - 1),
        (error: unknown) => {
            assert.ok(error instanceof JsonSizeLimitError);
            assert.equal(error.maxBytes, bytes - 1);
            assert.ok(error.bytes > error.maxBytes);
            return true;
        },
    );
});

test("shared JSON serialization preserves undefined roots and distinguishes encoding failures from size limits", () => {
    for (const value of [undefined, () => "not JSON", Symbol("not JSON")]) {
        assert.equal(stringifyJson(value, 1024), undefined);
    }
    assert.equal(stringifyJson({ toJSON: () => undefined }, 1024), undefined);
    const circular: Record<string, unknown> = {};
    circular.self = circular;
    const numeric = new Number(1);
    Object.defineProperty(numeric, Symbol.toPrimitive, { value: () => 1n });
    for (const value of [circular, 1n, Object(1n), { nested: 1n }, numeric]) {
        assert.throws(() => stringifyJson(value, 1024), TypeError);
    }
    const failure = new Error("conversion failed");
    const value = {
        get content(): unknown {
            throw failure;
        },
    };
    assert.throws(() => stringifyJson(value, 1024), (error: unknown) => error === failure);
});

test("shared JSON serialization handles deep values under its byte cap without a depth limit", () => {
    const depth = 20_000;
    const source = '{"metadata":'.repeat(depth) + "true" + "}".repeat(depth);
    const limit = 256 * 1024;
    assert.ok(Buffer.byteLength(source) < limit);
    const serialized = stringifyJson(JSON.parse(source), limit);
    assert.equal(typeof serialized, "string");
    let current: unknown = JSON.parse(serialized!);
    for (let level = 0; level < depth; level++) {
        assert.ok(current !== null && typeof current === "object" && "metadata" in current);
        current = current.metadata;
    }
    assert.equal(current, true);
});

test("opt-in JSON depth budgets count containers without changing the unbounded default", () => {
    const maxDepth = 1024;
    const source = "[".repeat(maxDepth) + "null" + "]".repeat(maxDepth);
    const value: unknown = JSON.parse(source);
    const serialized = stringifyJson(value, 256 * 1024, maxDepth);
    assert.equal(typeof serialized, "string");
    let current: unknown = JSON.parse(serialized!);
    for (let level = 0; level < maxDepth; level++) {
        assert.ok(Array.isArray(current));
        assert.equal(current.length, 1);
        current = current[0];
    }
    assert.equal(current, null);
    const wrapped = { metadata: value };
    assert.throws(
        () => stringifyJson(wrapped, 256 * 1024, maxDepth),
        (error: unknown) => {
            assert.ok(error instanceof JsonDepthLimitError);
            assert.equal(error.depth, maxDepth + 1);
            assert.equal(error.maxDepth, maxDepth);
            return true;
        },
    );
    assert.equal(typeof stringifyJson(wrapped, 256 * 1024), "string");
    assert.equal(JSON.parse(stringifyJson({ toJSON: () => 7 }, 1024, 0)!), 7);
    assert.throws(() => stringifyJson({}, 1024, 0), JsonDepthLimitError);
    assert.throws(
        () => stringifyJson({ content: "x".repeat(1024) }, 16, maxDepth),
        JsonSizeLimitError,
    );
    const circular: Record<string, unknown> = {};
    circular.self = circular;
    assert.throws(() => stringifyJson(circular, 1024, 1), TypeError);
});
