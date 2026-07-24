// R-server file service (SPEC 8.3/8.4/8.5/8.7, 7.5). Implements the C1 file
// methods (RPC_METHODS) over node:fs/promises: stat, readFile, writeFile,
// resolvePath, watch, unwatch, listDirectory. Revision tokens are opaque strings
// minted here server-side; clients never parse them. Writes are revision-guarded
// (SPEC 8.4) and atomic (SPEC 8.5). Watching uses fs.watch with a polling
// fallback and an EventEmitter sink so codeharbord can relay WatchEvents as
// JSON-RPC notifications. getMimeType stays an internal helper (the client
// resolves viewer types by extension in ViewerHandlerRegistry).

import { promises as fsp, watch as fsWatch } from "node:fs";
import type { FSWatcher, Stats } from "node:fs";
import type { FileHandle } from "node:fs/promises";
import { EventEmitter } from "node:events";
import { randomBytes } from "node:crypto";
import path from "node:path";

import { RPC_METHODS, RPC_REVISION_MISMATCH } from "./rpc-types.ts";
import type {
    StatParams,
    StatResult,
    ReadFileParams,
    ReadFileResult,
    WriteFileParams,
    WriteFileResult,
    ResolvePathParams,
    ResolvePathResult,
    WatchParams,
    WatchResult,
    UnwatchParams,
    UnwatchResult,
    WatchEvent,
    ListDirectoryParams,
    ListDirectoryResult,
} from "./rpc-types.ts";

// Opaque revision token (SPEC 8.4). Derived from mtime + size; clients treat it
// as bytes. Reused by every read/write/watch path, so it earns its name.
export function revisionFrom(stats: Pick<Stats, "mtimeMs" | "size">): string {
    return `${stats.mtimeMs}-${stats.size}`;
}

// Tagged error carrying the JSON-RPC revision-mismatch code (SPEC 8.6). The
// dispatcher recognizes it via the `code` field, keeping codeharbord decoupled.
export class RevisionMismatchError extends Error {
    readonly code = RPC_REVISION_MISMATCH;
    readonly data?: unknown;
    constructor(message: string, data?: unknown) {
        super(message);
        this.name = "RevisionMismatchError";
        this.data = data;
    }
}

export function isRevisionMismatch(err: unknown): err is RevisionMismatchError {
    return (
        typeof err === "object" &&
        err !== null &&
        "code" in err &&
        err.code === RPC_REVISION_MISMATCH
    );
}

// Structural over both fs.Stats and fs.Dirent, which share these predicates.
function nodeKind(node: Pick<Stats, "isFile" | "isDirectory" | "isSymbolicLink">): StatResult["kind"] {
    if (node.isFile()) return "file";
    if (node.isDirectory()) return "directory";
    if (node.isSymbolicLink()) return "symlink";
    return "other";
}

async function statOrUndefined(target: string): Promise<Stats | undefined> {
    try {
        return await fsp.stat(target);
    } catch (err) {
        if (err && typeof err === "object" && "code" in err && err.code === "ENOENT") {
            return undefined;
        }
        throw err;
    }
}

// Detect binary content: any NUL byte, or bytes that are not valid UTF-8.
function isBinary(buf: Buffer): boolean {
    if (buf.includes(0)) return true;
    try {
        new TextDecoder("utf-8", { fatal: true }).decode(buf);
        return false;
    } catch {
        return true;
    }
}

export async function stat(params: StatParams): Promise<StatResult> {
    // lstat so symlinks report kind "symlink" rather than their target.
    const stats = await fsp.lstat(params.path);
    return {
        path: params.path,
        kind: nodeKind(stats),
        size: stats.size,
        mtimeMs: stats.mtimeMs,
        mode: stats.mode,
        revision: revisionFrom(stats),
    };
}

export async function readFile(params: ReadFileParams): Promise<ReadFileResult> {
    // Read the bytes and mint the revision from ONE open descriptor so both
    // describe the same inode. With atomic saves (SPEC 8.5) the descriptor is
    // pinned across a concurrent rename, so fstat + read cannot straddle two
    // versions of the file the way two independent fsp.stat / fsp.readFile calls
    // could (TOCTOU: revision minted from one version, bytes from another).
    const handle = await fsp.open(params.path, "r");
    try {
        const stats = await handle.stat();
        if (stats.isDirectory()) {
            throw Object.assign(
                new Error(`EISDIR: illegal operation on a directory, read '${params.path}'`),
                { code: "EISDIR" },
            );
        }
        const buf = await handle.readFile();
        const revision = revisionFrom(stats);

        // offset/length are BYTE ranges. Normalize to non-negative integers: a
        // negative offset would otherwise index from the end of the buffer
        // (Buffer.subarray semantics) and silently return the wrong tail.
        const offset = Math.max(0, Math.trunc(params.offset ?? 0));
        let slice: Buffer;
        let truncated = false;
        if (offset >= buf.length) {
            slice = Buffer.alloc(0);
        } else if (params.length !== undefined) {
            const length = Math.max(0, Math.trunc(params.length));
            const end = offset + length;
            slice = buf.subarray(offset, Math.min(end, buf.length));
            truncated = end < buf.length;
        } else {
            slice = buf.subarray(offset);
        }

        // A byte range that cuts a multibyte codepoint is not valid UTF-8, so
        // isBinary() flips encoding to base64 — the exact bytes round-trip
        // losslessly rather than being mangled by a lossy UTF-8 decode.
        const binary = isBinary(slice);
        return {
            path: params.path,
            encoding: binary ? "base64" : "utf-8",
            content: binary ? slice.toString("base64") : slice.toString("utf-8"),
            revision,
            truncated,
        };
    } finally {
        await handle.close();
    }
}

// Enforce the revision guard (SPEC 8.4 / 8.6). "" means create-only. Factored
// out so the identical rule runs both at the start of a write and again just
// before the rename, shrinking the window where a file changes mid-write.
function assertRevisionMatches(filePath: string, expectedRevision: string, current: Stats | undefined): void {
    if (expectedRevision === "") {
        if (current) {
            throw new RevisionMismatchError(
                `File already exists: ${filePath}`,
                { path: filePath, revision: revisionFrom(current) },
            );
        }
    } else if (!current) {
        // A non-empty expectedRevision means the client loaded an existing file;
        // if it is now gone (deleted externally) that is a conflict, not a
        // silent recreate (SPEC 8.6: never silently overwrite a changed file).
        throw new RevisionMismatchError(
            `File no longer exists: ${filePath}`,
            { path: filePath, expected: expectedRevision, actual: null },
        );
    } else {
        const rev = revisionFrom(current);
        if (rev !== expectedRevision) {
            throw new RevisionMismatchError(
                `Revision mismatch for ${filePath}`,
                { path: filePath, expected: expectedRevision, actual: rev },
            );
        }
    }
}

export async function writeFile(params: WriteFileParams): Promise<WriteFileResult> {
    const encoding = params.encoding ?? "utf-8";
    const existing = await statOrUndefined(params.path);

    assertRevisionMatches(params.path, params.expectedRevision, existing);

    const buf = Buffer.from(params.content, encoding === "base64" ? "base64" : "utf-8");

    // Atomic save (SPEC 8.5): temp file in the same directory, flush, preserve
    // mode when overwriting, then rename over the target. When the target is an
    // existing symlink, resolve it to its real path so the rename replaces the
    // linked-to file rather than severing the link — this also keeps the write
    // consistent with the revision guard above, which stat()'d through the link.
    const target = existing ? await fsp.realpath(params.path) : params.path;
    const dir = path.dirname(target);
    const tmp = path.join(dir, `.${path.basename(target)}.${randomBytes(6).toString("hex")}.tmp`);
    const mode = existing ? existing.mode : 0o644;
    let handle: FileHandle | undefined = await fsp.open(tmp, "wx", mode);
    try {
        await handle.writeFile(buf);
        await handle.sync();
        await handle.close();
        handle = undefined;
        // open()'s mode is masked by umask; chmod pins the exact mode on overwrite.
        if (existing) await fsp.chmod(tmp, existing.mode);
        // Re-verify as late as possible — right before the atomic replace — so a
        // file changed during the write + flush above is caught instead of being
        // silently overwritten (SPEC 8.6). This shrinks but cannot fully close
        // the window; POSIX offers no atomic compare-and-rename.
        assertRevisionMatches(params.path, params.expectedRevision, await statOrUndefined(params.path));
        await fsp.rename(tmp, target);
    } catch (err) {
        if (handle) await handle.close().catch(() => {});
        await fsp.rm(tmp, { force: true });
        throw err;
    }

    // fsync the containing directory so the rename itself is durable across a
    // crash. Best-effort: some platforms (e.g. Windows) cannot fsync a directory.
    try {
        const dirHandle = await fsp.open(dir, "r");
        try {
            await dirHandle.sync();
        } finally {
            await dirHandle.close();
        }
    } catch {
        // Directory fsync unsupported here; the file data was already flushed.
    }

    const written = await fsp.stat(params.path);
    return { path: params.path, revision: revisionFrom(written) };
}

// Resolve relative paths against `base` (the repository root, defaulting to the
// process cwd). insideRepositoryRoot follows SPEC 9: outside paths are allowed
// but flagged so the UI can indicate the file is outside the project.
export function resolvePath(params: ResolvePathParams): ResolvePathResult {
    const base = path.resolve(params.base ?? process.cwd());
    const resolved = path.isAbsolute(params.path)
        ? path.resolve(params.path)
        : path.resolve(base, params.path);
    const rel = path.relative(base, resolved);
    const inside = rel === "" || (!rel.startsWith("..") && !path.isAbsolute(rel));
    return { path: resolved, insideRepositoryRoot: inside };
}

type WatchCallback = (event: WatchEvent) => void;

interface Subscription {
    id: string;
    path: string;
    watcher?: FSWatcher;
    poll?: NodeJS.Timeout;
    lastRevision?: string;
    // Serializes reconcile() calls so overlapping fs.watch and poll signals
    // dedupe against a single lastRevision instead of racing across awaits.
    pending?: Promise<void>;
}

// File-watch service (SPEC 8.7). fs.watch is the primary signal; a polling
// interval is the fallback for filesystems where fs.watch is unreliable. Events
// are delivered through an EventEmitter sink (onWatchEvent) so codeharbord can
// forward them as JSON-RPC notifications without coupling to stdio.
export class FileWatchService {
    private readonly emitter = new EventEmitter();
    private readonly subscriptions = new Map<string, Subscription>();
    private counter = 0;

    // Polling-fallback cadence, in milliseconds. Adjustable for tests.
    pollIntervalMs = 1000;

    onWatchEvent(callback: WatchCallback): () => void {
        this.emitter.on("event", callback);
        return () => this.emitter.off("event", callback);
    }

    async watch(params: WatchParams): Promise<WatchResult> {
        const id = `sub-${(this.counter += 1)}-${randomBytes(4).toString("hex")}`;
        const existing = await statOrUndefined(params.path);
        const sub: Subscription = {
            id,
            path: params.path,
            lastRevision: existing ? revisionFrom(existing) : undefined,
        };

        try {
            sub.watcher = fsWatch(params.path, () => {
                void this.reconcile(sub);
            });
            sub.watcher.on("error", () => {
                sub.watcher?.close();
                sub.watcher = undefined;
            });
        } catch {
            // fs.watch unavailable for this path; polling covers it.
        }

        sub.poll = setInterval(() => {
            void this.reconcile(sub);
        }, this.pollIntervalMs);
        sub.poll.unref?.();

        this.subscriptions.set(id, sub);
        return { subscriptionId: id };
    }

    unwatch(params: UnwatchParams): UnwatchResult {
        const sub = this.subscriptions.get(params.subscriptionId);
        if (sub) {
            sub.watcher?.close();
            clearInterval(sub.poll);
            this.subscriptions.delete(params.subscriptionId);
        }
        return { ok: true };
    }

    closeAll(): void {
        for (const id of [...this.subscriptions.keys()]) {
            this.unwatch({ subscriptionId: id });
        }
    }

    // Serialize reconcile calls per subscription and diff the on-disk revision
    // against the last one seen. Chaining onto sub.pending closes the await race
    // where an fs.watch signal and a poll tick both read the pre-change revision
    // and emit twice for one change. Event kind is derived purely from the state
    // transition (created/modified/deleted) so it is identical regardless of
    // which signal — fs.watch or poll — observed the change first.
    private reconcile(sub: Subscription): Promise<void> {
        const next = (sub.pending ?? Promise.resolve())
            .catch(() => {})
            .then(() => this.diffAndEmit(sub));
        sub.pending = next;
        return next;
    }

    private async diffAndEmit(sub: Subscription): Promise<void> {
        const stats = await statOrUndefined(sub.path);
        if (!stats) {
            if (sub.lastRevision !== undefined) {
                sub.lastRevision = undefined;
                this.emitter.emit("event", {
                    subscriptionId: sub.id,
                    path: sub.path,
                    event: "deleted",
                } satisfies WatchEvent);
            }
            return;
        }
        const revision = revisionFrom(stats);
        if (revision === sub.lastRevision) return;
        const created = sub.lastRevision === undefined;
        sub.lastRevision = revision;
        this.emitter.emit("event", {
            subscriptionId: sub.id,
            path: sub.path,
            event: created ? "created" : "modified",
            revision,
        } satisfies WatchEvent);
    }
}

export const fileWatchService = new FileWatchService();

// listDirectory (SPEC 7.5) is a registered RPC method (added after the initial
// six for the viewer workstream). getMimeType below stays an internal helper.

export async function listDirectory(
    params: ListDirectoryParams,
): Promise<ListDirectoryResult> {
    const dirents = await fsp.readdir(params.path, { withFileTypes: true });
    return {
        path: params.path,
        entries: dirents.map((entry) => ({ name: entry.name, kind: nodeKind(entry) })),
    };
}

const MIME_TYPES: Record<string, string> = {
    ".ts": "text/typescript",
    ".tsx": "text/typescript",
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".json": "application/json",
    ".md": "text/markdown",
    ".html": "text/html",
    ".css": "text/css",
    ".txt": "text/plain",
    ".yaml": "text/yaml",
    ".yml": "text/yaml",
    ".toml": "text/toml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".svg": "image/svg+xml",
    ".webp": "image/webp",
    ".pdf": "application/pdf",
};

export function getMimeType(filePath: string): string {
    return MIME_TYPES[path.extname(filePath).toLowerCase()] ?? "application/octet-stream";
}

// RPC handler table keyed by the frozen wire names (RPC_METHODS). codeharbord
// merges this into its method map. Handlers take opaque params and cast to the
// C1 request shapes; dispatch awaits any returned promise.
export const fileMethods: Record<string, (params: unknown) => unknown | Promise<unknown>> = {
    [RPC_METHODS.stat]: (params) => stat(params as StatParams),
    [RPC_METHODS.readFile]: (params) => readFile(params as ReadFileParams),
    [RPC_METHODS.writeFile]: (params) => writeFile(params as WriteFileParams),
    [RPC_METHODS.resolvePath]: (params) => resolvePath(params as ResolvePathParams),
    [RPC_METHODS.watch]: (params) => fileWatchService.watch(params as WatchParams),
    [RPC_METHODS.unwatch]: (params) => fileWatchService.unwatch(params as UnwatchParams),
    [RPC_METHODS.listDirectory]: (params) => listDirectory(params as ListDirectoryParams),
};
