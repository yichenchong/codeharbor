// Runtime guards for the workspace.* RPC handler params. This lives here in
// remote/src rather than a top-level shared/ npm workspace because the remote
// ships as a self-contained tarball and the C++ client, which mirrors these
// wire contracts by hand, cannot consume TypeScript.
//
// Each guard throws a plain Error with a precise, uniform message; the RPC
// dispatcher turns a thrown Error into a well-formed JSON-RPC error response.

export function requireObject(params: unknown, method: string): Record<string, unknown> {
    if (typeof params !== "object" || params === null || Array.isArray(params)) {
        throw new Error(`${method}: missing or invalid params object`);
    }
    return params as Record<string, unknown>;
}

export function requireString(obj: Record<string, unknown>, field: string, method: string): string {
    const value = obj[field];
    if (typeof value !== "string") {
        throw new Error(`${method}: missing or invalid field '${field}'`);
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
        throw new Error(`${method}: missing or invalid field '${field}'`);
    }
    return value;
}

export function requireStringArray(obj: Record<string, unknown>, field: string, method: string): string[] {
    const value = obj[field];
    if (!Array.isArray(value) || value.some((v) => typeof v !== "string")) {
        throw new Error(`${method}: missing or invalid field '${field}'`);
    }
    return value as string[];
}

export function optionalNumber(obj: Record<string, unknown>, field: string, method: string): number | undefined {
    const value = obj[field];
    if (value === undefined) return undefined;
    if (typeof value !== "number" || !Number.isFinite(value)) {
        throw new Error(`${method}: missing or invalid field '${field}'`);
    }
    return value;
}

export function optionalBoolean(obj: Record<string, unknown>, field: string, method: string): boolean | undefined {
    const value = obj[field];
    if (value === undefined) return undefined;
    if (typeof value !== "boolean") {
        throw new Error(`${method}: missing or invalid field '${field}'`);
    }
    return value;
}
