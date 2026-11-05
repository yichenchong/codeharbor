// Compact JSON serialization without recursive container traversal. The byte
// limit applies to emitted UTF-8 text; nesting is unbounded unless requested.
export class JsonSizeLimitError extends Error {
    readonly bytes: number;
    readonly maxBytes: number;

    constructor(bytes: number, maxBytes: number) {
        super(`JSON serialization exceeds ${maxBytes} bytes (at least ${bytes} bytes)`);
        this.name = "JsonSizeLimitError";
        this.bytes = bytes;
        this.maxBytes = maxBytes;
    }
}

export class JsonDepthLimitError extends Error {
    readonly depth: number;
    readonly maxDepth: number;

    constructor(depth: number, maxDepth: number) {
        super(`JSON serialization exceeds container depth ${maxDepth} (observed ${depth})`);
        this.name = "JsonDepthLimitError";
        this.depth = depth;
        this.maxDepth = maxDepth;
    }
}

// Like JSON.stringify, unsupported root values return undefined; unsupported
// object properties are omitted and unsupported array elements become null.
// Cycles, BigInt values, and failing user conversions propagate as errors.
export function stringifyJson(value: unknown, maxBytes: number, maxDepth?: number): string | undefined {
    const chunks: string[] = [];
    let bytes = 0;
    const append = (text: string): void => {
        bytes += Buffer.byteLength(text);
        if (bytes > maxBytes) throw new JsonSizeLimitError(bytes, maxBytes);
        chunks.push(text);
    };
    const normalize = (current: unknown, key: string): unknown => {
        if (
            current !== null &&
            (typeof current === "object" || typeof current === "function" || typeof current === "bigint")
        ) {
            const toJSON: unknown = Reflect.get(Object(current), "toJSON");
            if (typeof toJSON === "function") current = toJSON.call(current, key);
        }
        if (current instanceof Number) {
            // JSON uses ToNumber, not Number(), so a BigInt conversion fails.
            const numeric = current as unknown as number;
            return +numeric;
        }
        if (current instanceof String) return String(current);
        if (current instanceof Boolean) return Boolean.prototype.valueOf.call(current);
        if (current !== null && typeof current === "object" && BigInt.prototype.isPrototypeOf(current)) {
            return BigInt.prototype.valueOf.call(current);
        }
        return current;
    };
    const root = normalize(value, "");
    if (root === undefined || typeof root === "function" || typeof root === "symbol") return undefined;
    const ancestors = new Set<object>();
    const frames: Array<{
        value: object;
        keys?: string[];
        length: number;
        index: number;
        emitted: boolean;
    }> = [];
    const writeValue = (current: unknown): void => {
        if (current === null || typeof current !== "object") {
            const json = JSON.stringify(current);
            if (json === undefined) throw new TypeError("Value has no JSON representation");
            append(json);
            return;
        }
        if (ancestors.has(current)) throw new TypeError("Converting circular structure to JSON");
        const depth = frames.length + 1;
        if (maxDepth !== undefined && depth > maxDepth) {
            throw new JsonDepthLimitError(depth, maxDepth);
        }
        ancestors.add(current);
        let keys: string[] | undefined;
        let length: number;
        if (Array.isArray(current)) {
            length = current.length;
        } else {
            keys = Object.keys(current);
            length = keys.length;
        }
        append(keys === undefined ? "[" : "{");
        frames.push({ value: current, keys, length, index: 0, emitted: false });
    };
    writeValue(root);
    while (frames.length > 0) {
        const frame = frames[frames.length - 1];
        if (frame.index === frame.length) {
            append(frame.keys === undefined ? "]" : "}");
            ancestors.delete(frame.value);
            frames.pop();
            continue;
        }
        const key = frame.keys === undefined ? String(frame.index++) : frame.keys[frame.index++];
        // Keys come from this container's enumerable own keys or array indexes.
        const container = frame.value as Record<string, unknown>;
        const current = normalize(container[key], key);
        const omitted = current === undefined || typeof current === "function" || typeof current === "symbol";
        if (omitted && frame.keys !== undefined) continue;
        if (frame.emitted) append(",");
        frame.emitted = true;
        if (frame.keys !== undefined) {
            append(JSON.stringify(key));
            append(":");
        }
        writeValue(omitted ? null : current);
    }
    return chunks.join("");
}
