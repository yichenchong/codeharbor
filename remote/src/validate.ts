// Runtime guards for the RPC handler params (the `workspace.*` group and the
// frozen `file.*` catalog). This lives here in remote/src rather than a
// top-level shared/ npm workspace because the remote ships as a self-contained
// tarball and the C++ client, which mirrors these wire contracts by hand,
// cannot consume TypeScript.
//
// Each guard throws an InvalidParamsError with a precise, uniform message; the
// RPC dispatcher turns it into a JSON-RPC "Invalid params" (-32602) error
// response. That code, rather than the generic internal error, is what tells a
// client the fault is in the REQUEST it sent and not in the server.

import { RPC_INVALID_PARAMS } from "./rpc-types.ts";

/**
 * A malformed request payload. Tagged with the JSON-RPC "Invalid params" code
 * so the dispatcher can answer with -32602 without knowing which handler threw
 * (the same decoupling RevisionMismatchError uses in files.ts).
 */
export class InvalidParamsError extends Error {
    readonly code = RPC_INVALID_PARAMS;
    constructor(message: string) {
        super(message);
        this.name = "InvalidParamsError";
    }
}

export function isInvalidParams(err: unknown): err is InvalidParamsError {
    return (
        typeof err === "object" &&
        err !== null &&
        "code" in err &&
        err.code === RPC_INVALID_PARAMS
    );
}

export function requireObject(params: unknown, method: string): Record<string, unknown> {
    if (typeof params !== "object" || params === null || Array.isArray(params)) {
        throw new InvalidParamsError(`${method}: missing or invalid params object`);
    }
    return params as Record<string, unknown>;
}

export function requireString(obj: Record<string, unknown>, field: string, method: string): string {
    const value = obj[field];
    if (typeof value !== "string") {
        throw new InvalidParamsError(`${method}: missing or invalid field '${field}'`);
    }
    return value;
}

// Optional field that may be absent (undefined) or explicitly null — several
// param shapes carry nullable strings (e.g. taskDescription). When present and
// not null it must be a string.
export function optionalString(
    obj: Record<string, unknown>,
    field: string,
    method: string,
): string | null | undefined {
    const value = obj[field];
    if (value === undefined || value === null) return value;
    if (typeof value !== "string") {
        throw new InvalidParamsError(`${method}: missing or invalid field '${field}'`);
    }
    return value;
}

/**
 * Optional field that must be a STRING when present. Unlike optionalString it
 * REJECTS an explicit null, because it guards the params that map to columns
 * which are not nullable (a group's name, a session's repository root, a
 * viewer pane's url). There, `null` passed optionalString and was then thrown
 * away by the handler's `?? current` fallback: the caller asked for a change,
 * got a success response, and the row was left exactly as it was.
 */
export function optionalPlainString(
    obj: Record<string, unknown>,
    field: string,
    method: string,
): string | undefined {
    const value = obj[field];
    if (value === undefined) return undefined;
    if (typeof value !== "string") {
        throw new InvalidParamsError(`${method}: missing or invalid field '${field}'`);
    }
    return value;
}

export function requireStringArray(obj: Record<string, unknown>, field: string, method: string): string[] {
    const value = obj[field];
    if (!Array.isArray(value)) {
        throw new InvalidParamsError(`${method}: missing or invalid field '${field}'`);
    }
    for (const item of value) {
        if (typeof item !== "string") {
            throw new InvalidParamsError(`${method}: missing or invalid field '${field}'`);
        }
    }
    return value as string[];
}

export function optionalNumber(obj: Record<string, unknown>, field: string, method: string): number | undefined {
    const value = obj[field];
    if (value === undefined) return undefined;
    if (typeof value !== "number" || !Number.isFinite(value)) {
        throw new InvalidParamsError(`${method}: missing or invalid field '${field}'`);
    }
    return value;
}

export function optionalBoolean(obj: Record<string, unknown>, field: string, method: string): boolean | undefined {
    const value = obj[field];
    if (value === undefined) return undefined;
    if (typeof value !== "boolean") {
        throw new InvalidParamsError(`${method}: missing or invalid field '${field}'`);
    }
    return value;
}

/**
 * A byte offset or length: a non-negative integer no larger than
 * Number.MAX_SAFE_INTEGER. Anything else — a float, a negative, NaN, a numeric
 * string — is rejected HERE rather than being silently clamped or truncated
 * deeper in a handler, where it surfaces as a confusing internal error (a
 * non-numeric offset used to reach Buffer.alloc(NaN)).
 */
export function optionalIndex(
    obj: Record<string, unknown>,
    field: string,
    method: string,
): number | undefined {
    const value = obj[field];
    if (value === undefined) return undefined;
    if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) {
        throw new InvalidParamsError(
            `${method}: field '${field}' must be a non-negative integer`,
        );
    }
    return value;
}

/**
 * An integer within [min, max] inclusive. Used for POSIX file modes, where an
 * unchecked value is actively dangerous: `mode & 0o7777` turns any non-numeric
 * value into 0, i.e. a chmod to 000 that locks the user out of their own file.
 */
export function optionalIntegerInRange(
    obj: Record<string, unknown>,
    field: string,
    method: string,
    min: number,
    max: number,
): number | undefined {
    const value = obj[field];
    if (value === undefined) return undefined;
    if (typeof value !== "number" || !Number.isInteger(value) || value < min || value > max) {
        throw new InvalidParamsError(
            `${method}: field '${field}' must be an integer between ${min} and ${max}`,
        );
    }
    return value;
}

/**
 * A required string field constrained to a closed set of wire tokens (a file
 * encoding, a layout region). Rejecting an unlisted token is what keeps an
 * unknown value from being silently reinterpreted as the default one.
 */
export function requireOneOf<T extends string>(
    obj: Record<string, unknown>,
    field: string,
    method: string,
    allowed: readonly T[],
): T {
    const value = obj[field];
    if (typeof value !== "string" || !(allowed as readonly string[]).includes(value)) {
        throw new InvalidParamsError(
            `${method}: field '${field}' must be one of ${allowed.join(", ")}`,
        );
    }
    return value as T;
}

/** Optional form of requireOneOf: absent is fine, present must be in the set. */
export function optionalOneOf<T extends string>(
    obj: Record<string, unknown>,
    field: string,
    method: string,
    allowed: readonly T[],
): T | undefined {
    if (obj[field] === undefined) return undefined;
    return requireOneOf(obj, field, method, allowed);
}

/**
 * A field that must be PRESENT but whose value is free-form JSON (a split
 * tree). `undefined` is the only rejected value: it is what an omitted field
 * decodes to, and it cannot be stored — JSON.stringify(undefined) returns
 * undefined, which the SQLite driver refuses with an opaque type error.
 */
export function requireDefined(
    obj: Record<string, unknown>,
    field: string,
    method: string,
): unknown {
    if (obj[field] === undefined) {
        throw new InvalidParamsError(`${method}: missing field '${field}'`);
    }
    return obj[field];
}
