// Direct coverage for the RPC param guards (remote/src/validate.ts).
//
// Every `file.*` and `workspace.*` handler funnels its payload through these
// functions, and until now they were only ever exercised sideways, through a
// handler that happened to reject a bad value. That left the guards' BOUNDARIES
// — the exact line between accepted and rejected — untested, which is the only
// interesting thing about a validator: a guard that is one step too loose lets a
// malformed request reach business logic (and the database), and one that is one
// step too tight refuses a request the client is entitled to make.
//
// Two behaviours matter beyond accept/reject and are asserted throughout:
//   * a rejection is tagged with the JSON-RPC "Invalid params" code, because
//     that tag is the only thing that stops the dispatcher from answering -32603
//     and blaming the server for the client's payload;
//   * the message names the method and the field, because it is what a person
//     debugging a live connection has to work from.

import { test } from "node:test";
import assert from "node:assert/strict";

import { RPC_INVALID_PARAMS } from "../src/rpc-types.ts";
import {
    InvalidParamsError,
    isInvalidParams,
    optionalBoolean,
    optionalIndex,
    optionalIntegerInRange,
    optionalNumber,
    optionalOneOf,
    optionalPlainString,
    optionalString,
    requireDefined,
    requireObject,
    requireOneOf,
    requireString,
    requireStringArray,
} from "../src/validate.ts";

const M = "workspace.updateSession";

// Every rejection must be recognisable to the dispatcher AND readable by a
// person: the code decides whether the client is told "your request is wrong"
// (-32602) or "the server broke" (-32603).
function rejects(fn: () => unknown, ...mustMention: string[]): void {
    assert.throws(fn, (err: unknown) => {
        assert.ok(err instanceof InvalidParamsError, `not an InvalidParamsError: ${String(err)}`);
        assert.ok(isInvalidParams(err), "the dispatcher must recognise the tag");
        assert.equal(err.code, RPC_INVALID_PARAMS);
        for (const fragment of mustMention) {
            assert.ok(
                err.message.includes(fragment),
                `message ${JSON.stringify(err.message)} does not mention ${JSON.stringify(fragment)}`,
            );
        }
        return true;
    });
}

test("isInvalidParams recognises the tag and nothing else", () => {
    assert.equal(isInvalidParams(new InvalidParamsError("x")), true);
    // A plain Error from a handler must NOT be mistaken for a bad request: the
    // dispatcher would answer -32602 for a genuine server fault and send the
    // user hunting a payload problem that does not exist.
    assert.equal(isInvalidParams(new Error("boom")), false);
    assert.equal(isInvalidParams({ code: -32603 }), false);
    assert.equal(isInvalidParams(null), false);
    assert.equal(isInvalidParams(undefined), false);
    assert.equal(isInvalidParams("invalid params"), false);
    // Duck-typed on purpose, so an error raised by another module carrying the
    // same code still routes correctly.
    assert.equal(isInvalidParams({ code: RPC_INVALID_PARAMS }), true);
});

test("requireObject accepts an object and refuses an array or a primitive", () => {
    const params = { a: 1 };
    // Returned by identity: the guards below read fields straight off it.
    assert.equal(requireObject(params, M), params);

    // An ARRAY is the case the dispatcher cannot catch: JSON-RPC calls it a
    // structured value, so it reaches the handler, and every handler here reads
    // named fields off it. Left through, `params[0]` shaped payloads would read
    // as an object with no fields at all.
    for (const bad of [[], [{ id: "x" }], null, undefined, 5, "x", true]) {
        rejects(() => requireObject(bad, M), M, "params object");
    }
});

test("requireString demands a string and reports the field by name", () => {
    assert.equal(requireString({ name: "S" }, "name", M), "S");
    // The empty string is a STRING and is deliberately accepted here: whether a
    // blank name is meaningful belongs to the handler, not to a type guard.
    assert.equal(requireString({ name: "" }, "name", M), "");
    for (const bad of [undefined, null, 5, [], {}, true]) {
        rejects(() => requireString({ name: bad }, "name", M), M, "'name'");
    }
});

test("optionalString admits absent and explicit null, and keeps them apart", () => {
    // The distinction is the whole point: a handler writes NULL to a nullable
    // column for the second and leaves it alone for the first.
    assert.equal(optionalString({}, "task", M), undefined);
    assert.equal(optionalString({ task: null }, "task", M), null);
    assert.equal(optionalString({ task: "t" }, "task", M), "t");
    for (const bad of [5, [], {}, false]) {
        rejects(() => optionalString({ task: bad }, "task", M), M, "'task'");
    }
});

test("optionalPlainString refuses the null optionalString allows", () => {
    assert.equal(optionalPlainString({}, "name", M), undefined);
    assert.equal(optionalPlainString({ name: "N" }, "name", M), "N");
    // This is the difference between the two guards, and it is a real bug class:
    // the column is NOT NULL, so a handler's `params.name ?? current` silently
    // discarded the change and answered success.
    rejects(() => optionalPlainString({ name: null }, "name", M), M, "'name'");
    rejects(() => optionalPlainString({ name: 7 }, "name", M), M, "'name'");
});

test("requireStringArray rejects a non-array and any non-string element", () => {
    assert.deepEqual(requireStringArray({ ids: [] }, "ids", M), []);
    assert.deepEqual(requireStringArray({ ids: ["a", "b"] }, "ids", M), ["a", "b"]);
    // A single element of the wrong type is enough: reorder writes every entry
    // to an ordering column, so one number among the ids corrupts the scope.
    rejects(() => requireStringArray({ ids: ["a", 1] }, "ids", M), M, "'ids'");
    rejects(() => requireStringArray({ ids: ["a", null] }, "ids", M), M, "'ids'");
    rejects(() => requireStringArray({ ids: ["a", ["b"]] }, "ids", M), M, "'ids'");
    for (const bad of [undefined, null, "abc", 5, {}]) {
        rejects(() => requireStringArray({ ids: bad }, "ids", M), M, "'ids'");
    }
});

test("optionalNumber accepts any finite number and refuses NaN and infinities", () => {
    assert.equal(optionalNumber({}, "n", M), undefined);
    assert.equal(optionalNumber({ n: 0 }, "n", M), 0);
    assert.equal(optionalNumber({ n: -1.5 }, "n", M), -1.5);
    // NaN and Infinity are `typeof "number"`, survive JSON only as strings, and
    // poison every arithmetic use downstream — a length, an offset, a position.
    for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, "1", null, []]) {
        rejects(() => optionalNumber({ n: bad }, "n", M), M, "'n'");
    }
});

test("optionalBoolean refuses the truthy values a caller might send instead", () => {
    assert.equal(optionalBoolean({}, "pinned", M), undefined);
    assert.equal(optionalBoolean({ pinned: false }, "pinned", M), false);
    assert.equal(optionalBoolean({ pinned: true }, "pinned", M), true);
    // 0/1 and "true" are how other languages spell a boolean; accepting them
    // here would make the wire contract depend on the client's language.
    for (const bad of [0, 1, "true", "false", null]) {
        rejects(() => optionalBoolean({ pinned: bad }, "pinned", M), M, "'pinned'");
    }
});

test("optionalIndex accepts only whole, non-negative, exactly representable numbers", () => {
    assert.equal(optionalIndex({}, "offset", M), undefined);
    assert.equal(optionalIndex({ offset: 0 }, "offset", M), 0);
    assert.equal(optionalIndex({ offset: Number.MAX_SAFE_INTEGER }, "offset", M), Number.MAX_SAFE_INTEGER);
    // The boundary in both directions: one past MAX_SAFE_INTEGER is a number
    // that no longer means what it says, and -1 is a Buffer.alloc away from a
    // throw or a position column that every other write keeps contiguous.
    for (const bad of [
        Number.MAX_SAFE_INTEGER + 1,
        -1,
        -0.5,
        1.5,
        Number.NaN,
        Number.POSITIVE_INFINITY,
        "3",
        null,
        true,
    ]) {
        rejects(() => optionalIndex({ offset: bad }, "offset", M), M, "'offset'", "non-negative integer");
    }
    // -0 is a whole non-negative number and indexes slot 0. It is passed
    // through verbatim rather than normalized, which is fine because it
    // compares equal to 0 everywhere it is used; `assert.equal` is strict about
    // the sign, `===` is what the guard's callers actually do.
    assert.ok(optionalIndex({ offset: -0 }, "offset", M) === 0);
});

test("optionalIntegerInRange is inclusive at both ends and refuses fractions", () => {
    const mode = (value: unknown) =>
        optionalIntegerInRange({ mode: value }, "mode", "file.writeFile", 0, 0o7777);
    assert.equal(optionalIntegerInRange({}, "mode", "file.writeFile", 0, 0o7777), undefined);
    assert.equal(mode(0), 0);
    assert.equal(mode(0o600), 0o600);
    assert.equal(mode(0o7777), 0o7777);
    // Just outside, on both ends. A mode that slipped through as a non-number
    // became `mode & 0o7777` === 0, i.e. a chmod to 000 on the user's own file.
    for (const bad of [-1, 0o7777 + 1, 0.5, Number.NaN, "0600", null, true]) {
        rejects(() => mode(bad), "file.writeFile", "'mode'", "between 0 and 4095");
    }
});

test("requireOneOf pins a wire token to its closed set", () => {
    const encodings = ["utf-8", "base64"] as const;
    assert.equal(requireOneOf({ encoding: "base64" }, "encoding", M, encodings), "base64");
    // A near-miss must NOT fall back to the default token: silently reading
    // "utf8" as "utf-8" is how a client's typo becomes a corrupted file.
    for (const bad of ["utf8", "UTF-8", "", undefined, null, 0]) {
        rejects(() => requireOneOf({ encoding: bad }, "encoding", M, encodings), M, "'encoding'", "utf-8, base64");
    }
});

test("optionalOneOf allows absence but not an unlisted token", () => {
    const regions = ["viewer", "terminal"] as const;
    assert.equal(optionalOneOf({}, "region", M, regions), undefined);
    assert.equal(optionalOneOf({ region: "terminal" }, "region", M, regions), "terminal");
    rejects(() => optionalOneOf({ region: "sidebar" }, "region", M, regions), M, "'region'");
    // Explicit null is NOT absence: the field is single-valued and a null would
    // reach the CHECK constraint on session_layouts.region as a write.
    rejects(() => optionalOneOf({ region: null }, "region", M, regions), M, "'region'");
});

test("requireDefined rejects only undefined, and passes any JSON value through", () => {
    // Free-form JSON: a split tree. Everything except `undefined` is storable,
    // including null and false, so nothing else may be refused.
    for (const value of [null, false, 0, "", [], { a: 1 }]) {
        assert.deepEqual(requireDefined({ tree: value }, "tree", M), value);
    }
    rejects(() => requireDefined({}, "tree", M), M, "'tree'");
    rejects(() => requireDefined({ tree: undefined }, "tree", M), M, "'tree'");
});
