// R-server file service (SPEC 8.3/8.4/8.5/8.7, 7.5). Implements the C1 file
// methods (RPC_METHODS) over node:fs/promises: stat, readFile, writeFile,
// resolvePath, watch, unwatch, listDirectory. Revision tokens are opaque strings
// minted here server-side; clients never parse them. Writes are revision-guarded
// (SPEC 8.4) and atomic (SPEC 8.5). Watching uses fs.watch with a polling
// fallback and an EventEmitter sink so codeharbord can relay WatchEvents as
// JSON-RPC notifications. getMimeType stays an internal helper (the client
// resolves viewer types by extension in ViewerHandlerRegistry).

import { promises as fsp, watch as fsWatch, constants as fsConstants } from "node:fs";
import type { Dirent, FSWatcher, Stats } from "node:fs";
import type { FileHandle } from "node:fs/promises";
import { EventEmitter } from "node:events";
import { randomBytes } from "node:crypto";
import path from "node:path";

import { RPC_METHODS, RPC_REVISION_MISMATCH, RPC_RESOURCE_LIMIT } from "./rpc-types.ts";
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
    RpcMethodName,
} from "./rpc-types.ts";
import {
    InvalidParamsError,
    optionalIndex,
    optionalIntegerInRange,
    optionalOneOf,
    optionalString,
    requireObject,
    requireString,
} from "./validate.ts";

// Opaque revision token (SPEC 8.4/8.6). Clients treat it as bytes and never
// parse it. It must change whenever the file changes, so it folds in a
// change-monotonic component (ctimeMs) and the inode alongside mtime + size:
// a same-size external edit within a single mtime tick would otherwise mint an
// identical token, bypassing the save guard and dropping watch modify events.
export function revisionFrom(stats: Pick<Stats, "mtimeMs" | "ctimeMs" | "ino" | "size">): string {
    return `${stats.mtimeMs}-${stats.ctimeMs}-${stats.ino}-${stats.size}`;
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

// Tagged error carrying the JSON-RPC resource-limit code (RPC_RESOURCE_LIMIT):
// the request was valid, but answering it would blow a bound this service keeps.
// Recognized by the dispatcher via the `code` field exactly like the revision
// mismatch above, so codeharbord stays decoupled from this module's internals.
export class ResourceLimitError extends Error {
    readonly code = RPC_RESOURCE_LIMIT;
    constructor(message: string) {
        super(message);
        this.name = "ResourceLimitError";
    }
}

export function isResourceLimit(err: unknown): err is ResourceLimitError {
    return (
        typeof err === "object" &&
        err !== null &&
        "code" in err &&
        err.code === RPC_RESOURCE_LIMIT
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

// Decode as UTF-8, or report "not text" (undefined) for any NUL byte or any
// byte sequence that is not valid UTF-8. One decode serves both the verdict and
// the string that is returned, so the bytes are walked once instead of twice.
//
// ignoreBOM is required for correctness, not speed: TextDecoder DROPS a leading
// byte-order mark by default, so decoding without it would hand the client a
// buffer three bytes shorter than the file. Saving that buffer back would then
// silently delete the BOM (SPEC 8.5 saves must round-trip the exact bytes).
function decodeUtf8(buf: Buffer): string | undefined {
    if (buf.includes(0)) return undefined;
    try {
        return new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(buf);
    } catch {
        return undefined;
    }
}
// Full-file and ranged reads share a raw-byte ceiling so a request cannot
// allocate an unbounded Buffer. The serialized result has its own lower
// ceiling, leaving room for the JSON-RPC envelope under the transport's
// 16 MiB line limit.
export const MAX_FILE_READ_BYTES = 16 * 1024 * 1024;
export const MAX_FILE_RESPONSE_BYTES = 15 * 1024 * 1024;

function assertReadFits(result: ReadFileResult): void {
    const bytes = Buffer.byteLength(JSON.stringify(result));
    if (bytes > MAX_FILE_RESPONSE_BYTES) {
        throw new ResourceLimitError(
            `Cannot read ${result.path}: the encoded response is ${bytes} bytes, ` +
                `above this server's ${MAX_FILE_RESPONSE_BYTES}-byte reply limit. ` +
                `Read the file in smaller ranges.`,
        );
    }
}

export async function stat(params: StatParams): Promise<StatResult> {
    // lstat so symlinks report kind "symlink" rather than their target.
    const stats = await fsp.lstat(params.path);
    // The revision must name the inode readFile and writeFile actually touch,
    // and both of those FOLLOW a symlink. Minting the token from the link
    // itself would make every save through a symlink a guaranteed — and
    // unresolvable — revision mismatch: the client would load one token from
    // stat and the write guard would compare it against a different one.
    // A dangling or looping link has no readable target; fall back to the
    // link's own token so stat still answers instead of failing.
    const target = stats.isSymbolicLink()
        ? await fsp.stat(params.path).catch(() => undefined)
        : undefined;
    // Writability is checked against the LINK-FOLLOWED target (X12): a symlink
    // to a read-only file must report writable:false even though the link node
    // itself is writable. fs.access resolves the path, so W_OK reflects the real
    // target's permissions; any error (no such target, no permission) => false.
    let writable = false;
    try {
        await fsp.access(params.path, fsConstants.W_OK);
        writable = true;
    } catch {
        writable = false;
    }
    // Which fields describe the LINK and which describe its TARGET is a mix, on
    // purpose, and worth stating because it is not guessable: `kind`, `size`,
    // `mtimeMs` and `mode` come from the link node itself (that is what makes
    // `kind` able to say "symlink" at all, and `mode` is documented as
    // symlink-blind on the client side — X12 in EditorController), while
    // `revision` and `writable` describe the target, because those two are what
    // readFile and writeFile act on. So `size` through a symlink is the length
    // of the link's own path text, NOT the size of the file readFile returns;
    // no consumer reads `size` for that purpose today, and one that starts to
    // must resolve the link itself.
    return {
        path: params.path,
        kind: nodeKind(stats),
        size: stats.size,
        mtimeMs: stats.mtimeMs,
        mode: stats.mode,
        revision: revisionFrom(target ?? stats),
        writable,
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
        const revision = revisionFrom(stats);
        const size = stats.size;
        if (
            params.offset === undefined &&
            params.length === undefined &&
            size > MAX_FILE_READ_BYTES
        ) {
            throw new ResourceLimitError(
                `Cannot read ${params.path}: the file is ${size} bytes, above this server's ` +
                    `${MAX_FILE_READ_BYTES}-byte full-read limit. Read it in smaller ranges.`,
            );
        }

        // offset/length are BYTE ranges. Normalize to non-negative integers: a
        // negative offset would otherwise index from the end of the file and
        // silently return the wrong tail.
        const offset = Math.max(0, Math.trunc(params.offset ?? 0));
        const wanted =
            params.length === undefined ? undefined : Math.max(0, Math.trunc(params.length));
        let slice: Buffer;
        if (params.offset !== undefined || wanted !== undefined) {
            // Ranged read: pull ONLY the requested window into memory via a
            // positioned read, so a multi-GiB file never loads whole (which
            // would also exceed Buffer's max length).
            if (offset >= size) {
                slice = Buffer.alloc(0);
            } else {
                const want = wanted === undefined ? size - offset : Math.min(wanted, size - offset);
                if (want > MAX_FILE_READ_BYTES) {
                    throw new ResourceLimitError(
                        `Cannot read ${params.path}: the requested range is ${want} bytes, ` +
                            `above this server's ${MAX_FILE_READ_BYTES}-byte range limit. ` +
                            `Read it in smaller ranges.`,
                    );
                }
                const dest = Buffer.alloc(want);
                const { bytesRead } = await handle.read(dest, 0, want, offset);
                slice = bytesRead === want ? dest : dest.subarray(0, bytesRead);
            }
        } else {
            // Read exactly the size observed by fstat rather than delegating to
            // readFile(), whose internal growth handling could allocate again if
            // another process appends to the file while this request is running.
            const dest = Buffer.alloc(size);
            const { bytesRead } = await handle.read(dest, 0, size, 0);
            slice = bytesRead === size ? dest : dest.subarray(0, bytesRead);
        }
        const fileChangedDuringRead = revisionFrom(await handle.stat()) !== revision;

        // `truncated` answers exactly ONE question: is `content` the WHOLE file?
        // It is true whenever any byte of the file is absent from the returned
        // content — bytes BEFORE the window (any offset past byte 0) count just
        // as much as bytes after it (a length that stops short of the end, and
        // the degenerate case of an offset at or beyond the end of a non-empty
        // file, which returns nothing at all). Every consumer depends on that
        // one meaning and on nothing else: the viewer refuses to render a
        // partial document (src/viewers/InternalUrlSchemeHandler.cpp,
        // src/viewers/ViewerModel.cpp) and the editor marks the buffer
        // read-only so a save can never delete the bytes it never received
        // (src/editor/EditorController.cpp).
        //
        // Derived ONCE from the CLAMPED window [start, end) against the file
        // size, so every return path above obeys the same definition. A change
        // observed on the open descriptor also makes the result partial/stale,
        // so the editor cannot save it over a version it did not receive.
        const start = Math.min(offset, size);
        const end = wanted === undefined ? size : Math.min(start + wanted, size);
        const truncated = fileChangedDuringRead || start > 0 || end < size;

        // A byte range that cuts a multibyte codepoint is not valid UTF-8, so
        // the decode fails and the encoding flips to base64 — the exact bytes
        // round-trip losslessly rather than being mangled by a lossy decode.
        const text = decodeUtf8(slice);
        const result: ReadFileResult = {
            path: params.path,
            encoding: text === undefined ? "base64" : "utf-8",
            content: text ?? slice.toString("base64"),
            revision,
            truncated,
        };
        assertReadFits(result);
        return result;
    } finally {
        await handle.close();
    }
}

// Enforce the revision guard (SPEC 8.4 / 8.6). "" means create-only. Factored
// out so the identical rule runs both at the start of a write and again just
// before the rename, shrinking the window where a file changes mid-write.
// The on-disk token is reported under the key `currentRevision`, which is the
// one the client reads to offer reload/overwrite straight from the error
// (EditorController::save, src/web/editor/src/index.ts); any other name costs
// the conflict path an extra stat round-trip.
function assertRevisionMatches(filePath: string, expectedRevision: string, current: Stats | undefined): void {
    if (expectedRevision === "") {
        if (current) {
            throw new RevisionMismatchError(
                `File already exists: ${filePath}`,
                { path: filePath, currentRevision: revisionFrom(current) },
            );
        }
    } else if (!current) {
        // A non-empty expectedRevision means the client loaded an existing file;
        // if it is now gone (deleted externally) that is a conflict, not a
        // silent recreate (SPEC 8.6: never silently overwrite a changed file).
        throw new RevisionMismatchError(
            `File no longer exists: ${filePath}`,
            { path: filePath, expected: expectedRevision, currentRevision: null },
        );
    } else {
        const rev = revisionFrom(current);
        if (rev !== expectedRevision) {
            throw new RevisionMismatchError(
                `Revision mismatch for ${filePath}`,
                { path: filePath, expected: expectedRevision, currentRevision: rev },
            );
        }
    }
}

// Per-resolved-path write lock (SPEC 8.6). codeharbord dispatches request lines
// concurrently, so two writeFile calls carrying the SAME expectedRevision can
// both pass the guard and then race the rename — the second silently
// overwriting the first (lost update). Chaining each write onto the previous
// one for the SAME resolved target makes the whole critical section — the final
// revision re-check, the rename, AND the post-rename stat that mints the
// returned revision — atomic per path, without globally serializing writes to
// unrelated files.
//
// RF16: because the lock spans rename+stat, the minted revision reflects
// exactly the bytes THIS save wrote — no concurrent same-service writer can
// rename a different version in between and have its stat leak into our token.
// This guarantee is limited to writers going through this module: a THIRD-PARTY
// process editing the same file in the sub-millisecond window between our rename
// and stat remains out of scope, since Node offers no portable OS advisory file
// lock (flock is non-portable and unavailable via fs/promises) to fence it out.
const writeLocks = new Map<string, Promise<void>>();

// Follow a symbolic-link chain by hand and return the path it ENDS at — the
// node an ordinary open(..., O_CREAT) through `p` would create or truncate.
//
// fsp.realpath cannot do this: it fails with ENOENT the moment a component of
// the chain is missing, and a DANGLING link is exactly the case that matters
// here. Because the status check below follows links, a link whose target does
// not exist reads as "no file at this path", so the atomic save took its create
// path and renamed a fresh regular file OVER THE LINK — silently destroying a
// symlink the user made, where a plain create through that same path would have
// created the link's TARGET. Resolving the chain first makes the save write
// through the link, matching open()'s semantics and leaving the link intact.
//
// The depth cap mirrors the kernel's own (Linux SYMLOOP_MAX is 40). A link cycle
// (`ln -s a b; ln -s b a`) would otherwise spin this loop forever inside an RPC
// handler; hitting the cap reports ELOOP, the same errno the kernel produces.
const MAX_SYMLINK_DEPTH = 40;

async function resolveLinkChain(p: string): Promise<string> {
    let current = p;
    for (let depth = 0; depth <= MAX_SYMLINK_DEPTH; depth += 1) {
        let link: string;
        try {
            link = await fsp.readlink(current);
        } catch {
            // Not a symlink (EINVAL), or nothing at this path at all (ENOENT):
            // either way this is the end of the chain and the node to write.
            return current;
        }
        // A link's body is relative to the directory HOLDING the link, not to
        // the process cwd.
        current = path.resolve(path.dirname(current), link);
    }
    throw Object.assign(new Error(`Too many levels of symbolic links: ${p}`), {
        code: "ELOOP",
    });
}

async function resolveWriteKey(p: string): Promise<string> {
    // Key on the node the write will actually touch. That is where the symlink
    // chain ends, because writeFileLocked writes THROUGH a link — so a write
    // addressed to a link and a write addressed to its target serialize against
    // each other instead of racing under two different keys.
    const end = await resolveLinkChain(p).catch(() => p);
    try {
        return await fsp.realpath(end);
    } catch {
        // Create-only (file absent) or a missing component: key on the real
        // directory + basename so concurrent creates of the same new path still
        // serialize; fall back to a lexically resolved path if even that fails.
        try {
            return path.join(await fsp.realpath(path.dirname(end)), path.basename(end));
        } catch {
            return path.resolve(end);
        }
    }
}

async function withWriteLock<T>(key: string, fn: () => Promise<T>): Promise<T> {
    const prev = writeLocks.get(key) ?? Promise.resolve();
    const run = prev.then(fn, fn);
    const settled = run.then(() => {}, () => {});
    writeLocks.set(key, settled);
    void settled.finally(() => {
        if (writeLocks.get(key) === settled) writeLocks.delete(key);
    });
    return run;
}

export async function writeFile(params: WriteFileParams): Promise<WriteFileResult> {
    const key = await resolveWriteKey(params.path);
    return withWriteLock(key, () => writeFileLocked(params));
}

// Name of the temp file the atomic save renames over `basename`. The random
// suffix is what makes it unique; the basename is only there so a leftover temp
// (a crash between open and rename) is recognizable.
//
// The basename half is CLIPPED to fit NAME_MAX (255 bytes on Linux and macOS).
// A legitimately long filename — 250 bytes is a perfectly valid name — plus the
// 18 characters this adds would otherwise overflow the limit, so open() failed
// with ENAMETOOLONG and EVERY save of that file was an error the user could do
// nothing about. Clipping is measured in BYTES, not code units, and never cuts
// a multibyte character in half, because the limit the kernel enforces is a
// byte count while a JavaScript string is counted in UTF-16 units.
const NAME_MAX_BYTES = 255;

function tempName(basename: string): string {
    const suffix = `.${randomBytes(6).toString("hex")}.tmp`;
    // The leading dot plus the suffix are fixed overhead.
    const room = NAME_MAX_BYTES - suffix.length - 1;
    let stem = basename;
    while (stem.length > 0 && Buffer.byteLength(stem) > room) stem = stem.slice(0, -1);
    return `.${stem}${suffix}`;
}

async function writeFileLocked(params: WriteFileParams): Promise<WriteFileResult> {
    const encoding = params.encoding ?? "utf-8";
    // Buffer.from(s, "base64") is LENIENT: it silently drops every character
    // outside the alphabet and discards a trailing partial group. A payload
    // mangled in transit would therefore be written as short, corrupt bytes and
    // reported back as a successful save. Node accepts the standard (+/) and
    // URL-safe (-_) alphabets and ignores whitespace, so validate the alphabet,
    // group length, and padding grammar before decoding.
    if (encoding === "base64") {
        const compact = params.content.replace(/\s/g, "");
        const match = /^([A-Za-z0-9+\/\-_]*)(=*)$/.exec(compact);
        const data = match?.[1] ?? "";
        const padding = match?.[2] ?? "";
        const remainder = data.length % 4;
        const validPadding =
            match !== null &&
            padding.length <= 2 &&
            remainder !== 1 &&
            (padding.length === 0 ||
                (padding.length === 1 && remainder === 3) ||
                (padding.length === 2 && remainder === 2));
        if (!validPadding) {
            throw new InvalidParamsError(`Invalid base64 content for ${params.path}`);
        }
    }
    const existing = await statOrUndefined(params.path);

    assertRevisionMatches(params.path, params.expectedRevision, existing);

    const buf = Buffer.from(params.content, encoding === "base64" ? "base64" : "utf-8");

    // Atomic save (SPEC 8.5): temp file in the same directory, flush, preserve
    // mode when overwriting, then rename over the target. When the target is a
    // symlink, resolve it to the file it names so the rename replaces the
    // linked-to file rather than severing the link — this also keeps the write
    // consistent with the revision guard above, which stat()'d through the link.
    //
    // BOTH branches resolve the link, for the same reason. An existing target is
    // resolved with realpath, which also normalizes symlinked parent
    // directories. A create needs resolveLinkChain instead, because realpath
    // refuses a chain that dangles — and a DANGLING link is precisely the path
    // that reaches the create branch, since statOrUndefined follows links and so
    // reports "nothing here" for it. Writing the resolved end preserves the
    // user's link and creates its target, exactly as a plain create through the
    // link would; renaming over params.path would have replaced the link itself
    // with a regular file and lost it with no error and no way back.
    const target = existing ? await fsp.realpath(params.path) : await resolveLinkChain(params.path);
    const dir = path.dirname(target);
    const tmp = path.join(dir, tempName(path.basename(target)));
    // Creating a file whose parent directory does not exist yet must WORK, not
    // fail with ENOENT: the frozen C1 method catalog has no createDirectory, so
    // writeFile is the only way a client can bring a new path into being. The
    // editor's crash-recovery snapshots (SPEC 11.3) are exactly this case — they
    // are written to a `.codeharbor-recovery/` directory beside the file that
    // nothing else ever creates, and the write is best-effort, so an ENOENT here
    // silently disabled recovery altogether. Only the create path needs this: an
    // overwrite proves the directory already exists. `recursive` also makes it a
    // no-op when it does. 0o700 applies ONLY to directories actually created,
    // and keeps a directory this service invented private to its owner — it can
    // hold unsaved user work (SPEC 11.3 wants recovery data at mode 0600).
    if (!existing) await fsp.mkdir(dir, { recursive: true, mode: 0o700 });
    // 0o7777 keeps the permission plus setuid/setgid/sticky bits and drops the
    // file-type bits stat reports. An explicit params.mode (C1: recovery
    // snapshots at 0o600, ED15) wins over the preserved/default mode and is
    // pinned EXACTLY via chmod below so umask cannot loosen it. The temp file is
    // created 0o600 whenever it will be chmod'd to a final mode (overwrite or an
    // explicit mode), so its half-written contents are never readable by other
    // users under a group- or world-readable target mode.
    const explicitMode = params.mode !== undefined ? params.mode & 0o7777 : undefined;
    const finalMode = explicitMode ?? (existing ? existing.mode & 0o7777 : 0o644);
    // Pin the mode with an explicit chmod (bypassing umask) whenever we overwrite
    // an existing file or a caller demanded a specific mode; a fresh create with
    // no explicit mode keeps the umask-masked open() mode (RF7).
    const pinMode = existing !== undefined || explicitMode !== undefined;
    let handle: FileHandle | undefined = await fsp.open(tmp, "wx", pinMode ? 0o600 : finalMode);
    try {
        await handle.writeFile(buf);
        await handle.sync();
        await handle.close();
        handle = undefined;
        // open()'s mode is masked by umask; chmod pins the exact mode.
        if (pinMode) await fsp.chmod(tmp, finalMode);
        // Re-verify as late as possible — right before the atomic replace — so a
        // file changed during the write + flush above is caught instead of being
        // silently overwritten (SPEC 8.6). This shrinks but cannot fully close
        // the window; POSIX offers no atomic compare-and-rename.
        assertRevisionMatches(params.path, params.expectedRevision, await statOrUndefined(params.path));
        await fsp.rename(tmp, target);
    } catch (err) {
        if (handle) await handle.close().catch(() => {});
        // Cleanup must never mask the real failure: a RevisionMismatchError
        // rethrown as an unlink error would reach the client as a generic
        // internal error and the conflict dialog (SPEC 8.6) would never open.
        await fsp.rm(tmp, { force: true }).catch(() => {});
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
//
// The flag is LEXICAL and deliberately so: it never touches the filesystem, so
// it costs nothing and works for paths that do not exist yet, but a symlink
// inside the root that points elsewhere still reports inside. It is a UI hint,
// NOT a sandbox — nothing in this service confines reads or writes to the root,
// because SPEC 9 requires paths outside the root to remain openable.
export function resolvePath(params: ResolvePathParams): ResolvePathResult {
    const base = path.resolve(params.base ?? process.cwd());
    const resolved = path.isAbsolute(params.path)
        ? path.resolve(params.path)
        : path.resolve(base, params.path);
    const rel = path.relative(base, resolved);
    // A leading ".." segment (rel === ".." or "../…") escapes the base; an
    // in-repo name that merely STARTS with ".." (e.g. "..config") does not.
    const inside =
        rel === "" ||
        (rel !== ".." && !rel.startsWith(".." + path.sep) && !path.isAbsolute(rel));
    return { path: resolved, insideRepositoryRoot: inside };
}

type WatchCallback = (event: WatchEvent) => void;
type WatchClosedCallback = (subscriptionId: string) => void;

interface Subscription {
    id: string;
    path: string;
    watcher?: FSWatcher;
    poll?: NodeJS.Timeout;
    lastRevision?: string;
    // Coalescing scheme (RF14) that bounds the check chain at ONE waiter: the
    // check currently reading the filesystem, plus at most one follow-up. It
    // still dedupes overlapping fs.watch and poll signals against a single
    // lastRevision without racing across awaits.
    inFlight?: Promise<void>;
    // A change signal that arrives while a check is in flight sets this
    // (idempotently) instead of appending a new link to the chain. When the
    // in-flight check finishes, exactly one follow-up runs if it was set.
    queued?: boolean;
    // Set on unwatch/closeAll so an in-flight diffAndEmit that is awaiting stat
    // bails instead of emitting a WatchEvent for a released subscription.
    closed?: boolean;
}

// Ceiling on LIVE watch subscriptions across the whole service.
//
// Every subscription holds an operating-system watch handle (an inotify watch on
// Linux, a kqueue descriptor on macOS) plus an interval timer, and the table
// holding them was unbounded. The producer is our own client, driven by the
// files it currently has open, but "the client only opens a few" is not a bound
// the server can verify: a client that leaks unwatch calls — or opens and closes
// files for long enough — walks the table up until the process hits the per-user
// inotify instance/watch limit, and then EVERY watch on the box (ours and other
// software's) starts failing, for a daemon that is meant to be long-lived.
//
// 512 is deliberately EQUAL to MAX_PENDING_WATCH_EVENTS in codeharbord.ts, which
// bounds the relay's coalescing queue. That queue coalesces per (subscription,
// path) and a subscription watches exactly one path, so its length can never
// exceed the number of live subscriptions — with the two numbers equal, the
// relay's count bound is provably unreachable and its byte bound is the one that
// can bite. A parity test pins the relationship.
//
// AT THE CAP the new watch is REFUSED with RPC_RESOURCE_LIMIT. Evicting the
// oldest subscription instead would leave an open file silently unwatched
// while it still believes it is being told about changes — showing stale content
// as current, the one outcome SPEC 8.7 exists to prevent. A refusal is visible:
// the file still opens and reads, only live change notification is missing, and
// the client is told why.
export const MAX_WATCH_SUBSCRIPTIONS = 512;

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

    // Announce that a subscription is gone (unwatch, or closeAll when the SSH
    // channel drops). codeharbord's notification relay may be holding queued
    // events for it while the client's end of the channel is stalled; without
    // this signal that queue would outlive its only possible consumer.
    onWatchClosed(callback: WatchClosedCallback): () => void {
        this.emitter.on("closed", callback);
        return () => this.emitter.off("closed", callback);
    }

    // Whether `id` is still an active subscription. The relay consults this
    // before queueing or delivering, so an event that was already in flight
    // when the client unwatched is never handed to a subscriber that is gone.
    hasSubscription(id: string): boolean {
        return this.subscriptions.has(id);
    }

    async watch(params: WatchParams): Promise<WatchResult> {
        // Checked AND reserved before the first await. The table insertion used
        // to happen after two awaits, so a burst of concurrent watch calls could
        // all pass one size check and overshoot the cap together; reserving the
        // slot up front makes the bound hold under the concurrent dispatch
        // codeharbord actually does.
        if (this.subscriptions.size >= MAX_WATCH_SUBSCRIPTIONS) {
            throw new ResourceLimitError(
                `Cannot watch ${params.path}: this server already has ` +
                    `${MAX_WATCH_SUBSCRIPTIONS} active file watches, which is its limit. ` +
                    `Close some editors or viewers and try again.`,
            );
        }
        const id = `sub-${(this.counter += 1)}-${randomBytes(4).toString("hex")}`;
        const sub: Subscription = { id, path: params.path };
        this.subscriptions.set(id, sub);
        let existing: Stats | undefined;
        try {
            existing = await statOrUndefined(params.path);
        } catch (err) {
            // An unreadable path (EACCES, ELOOP) must not leave the reservation
            // behind: the caller gets the error and no subscription exists.
            this.subscriptions.delete(id);
            throw err;
        }
        // closeAll() (the SSH channel dropped) may have released the reservation
        // while we were stat'ing. Installing an OS watch handle now would leak
        // exactly the handle this method exists to account for.
        if (sub.closed || !this.subscriptions.has(id)) return { subscriptionId: id };
        sub.lastRevision = existing ? revisionFrom(existing) : undefined;

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

        // No re-insertion here: the slot was reserved above.
        return { subscriptionId: id };
    }

    unwatch(params: UnwatchParams): UnwatchResult {
        const sub = this.subscriptions.get(params.subscriptionId);
        if (sub) {
            sub.closed = true;
            sub.watcher?.close();
            clearInterval(sub.poll);
            this.subscriptions.delete(params.subscriptionId);
            // Emitted only for a subscription that actually existed, so a
            // duplicate unwatch (or closeAll after unwatch) is silent.
            this.emitter.emit("closed", params.subscriptionId);
        }
        return { ok: true };
    }

    closeAll(): void {
        for (const id of [...this.subscriptions.keys()]) {
            this.unwatch({ subscriptionId: id });
        }
    }

    // Diff the on-disk revision against the last one seen, coalescing signals so
    // AT MOST ONE check waits behind the in-flight one (RF14). This closes the
    // await race where an fs.watch signal and a poll tick both read the
    // pre-change revision and emit twice for one change, while bounding memory:
    // a burst of signals collapses into a single queued follow-up rather than a
    // check per signal. The follow-up reads the filesystem AFTER it begins, so
    // any change whose signal arrived before it starts is still observed — no
    // event is lost. Event kind is derived purely from the state transition
    // (created/modified/deleted) so it is identical regardless of which signal —
    // fs.watch or poll — observed the change first.
    private reconcile(sub: Subscription): void {
        if (sub.inFlight) {
            // A check is running; record that one more is due. Idempotent: many
            // signals during one check collapse to a single follow-up.
            sub.queued = true;
            return;
        }
        this.runCheck(sub);
    }

    private runCheck(sub: Subscription): void {
        const done = this.diffAndEmit(sub)
            .catch(() => {})
            .then(() => {
                sub.inFlight = undefined;
                if (sub.queued) {
                    // Clear `queued` exactly when the follow-up check BEGINS, not
                    // when it was scheduled: the check reads the filesystem after
                    // this point, so any signal arriving from now on must queue a
                    // fresh follow-up rather than be absorbed by this one.
                    sub.queued = false;
                    this.runCheck(sub);
                }
            });
        sub.inFlight = done;
    }

    private async diffAndEmit(sub: Subscription): Promise<void> {
        const stats = await statOrUndefined(sub.path);
        // The subscription may have been unwatched while we awaited stat above;
        // emitting now would deliver an event for a released subscription.
        if (sub.closed || !this.subscriptions.has(sub.id)) return;
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

// Fixed JSON overhead of one DirectoryEntry: `{"name":,"kind":"directory"},` —
// the quotes around the name are already counted by JSON.stringify above, and
// "directory" is the longest kind nodeKind can report.
const LISTING_ENTRY_ENVELOPE_BYTES = 29;

// Ceiling on the SERIALIZED size of one listing's entries, in bytes.
//
// codeharbord frames every response as ONE JSON line, and both ends of the wire
// treat a line past MAX_LINE_BYTES (16 MiB, codeharbord.ts, mirrored by
// kMaxLineBytes in the C++ client) as a fault that drops the transport. A
// directory with hundreds of thousands of entries serializes past that, so
// listing it did not merely fail: the client's bounded reader tore down the SSH
// channel, taking every terminal, editor and watch subscription in the session
// with it, and the user saw a dropped connection with nothing naming the
// directory that caused it.
//
// 15 MiB leaves a megabyte for the JSON-RPC envelope and the `path` field, so a
// listing that passes this check cannot produce an over-cap line. A parity test
// pins it below MAX_LINE_BYTES.
//
// Over the cap the request is REFUSED with RPC_RESOURCE_LIMIT rather than
// answered with a truncated listing. A silently short listing is worse than an
// error: the viewer would present it as the directory's complete contents, and a
// user who does not see a file concludes it is not there. Reporting the real
// entry count tells them what they are looking at and what to do instead.
export const MAX_DIRECTORY_LISTING_BYTES = 15 * 1024 * 1024;

/**
 * Refuse a listing whose serialized entries would not fit in one transport
 * frame. Exported as its own contract because the only honest end-to-end test
 * of MAX_DIRECTORY_LISTING_BYTES would have to create tens of thousands of real
 * files; this way the bound, the boundary and the message are all testable
 * directly, and `listDirectory` keeps exactly one call site for it.
 *
 * Takes the dirents themselves rather than a name array so the production path
 * does not copy half a million strings just to measure them.
 */
export function assertListingFits(
    dirPath: string,
    entries: readonly Pick<Dirent, "name">[],
): void {
    // Measure the reply BEFORE building it. The size comes from the SERIALIZED
    // form of each name (JSON.stringify, not byte length) because escaping is
    // what actually expands: a filename may hold any byte but NUL and `/`, and a
    // control byte becomes the six characters \u00XX — so a byte-length estimate
    // can be off by 6x on exactly the pathological input this bound exists for.
    let bytes = 0;
    for (const entry of entries) {
        bytes += LISTING_ENTRY_ENVELOPE_BYTES + Buffer.byteLength(JSON.stringify(entry.name));
        if (bytes > MAX_DIRECTORY_LISTING_BYTES) {
            throw new ResourceLimitError(
                `Cannot list ${dirPath}: it holds ${entries.length} entries, whose listing ` +
                    `exceeds this server's ${MAX_DIRECTORY_LISTING_BYTES}-byte reply limit. ` +
                    `Open a subdirectory, or use a terminal to inspect it.`,
            );
        }
    }
}

export async function listDirectory(
    params: ListDirectoryParams,
): Promise<ListDirectoryResult> {
    const dirents = await fsp.readdir(params.path, { withFileTypes: true });
    // Filesystem enumeration order is platform-dependent; sort by the raw
    // filename so the same directory has one stable wire representation.
    dirents.sort((left, right) => (left.name < right.name ? -1 : left.name > right.name ? 1 : 0));
    assertListingFits(params.path, dirents);
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

// --- RPC handler table ------------------------------------------------------
//
// Keyed by the frozen wire names (RPC_METHODS); codeharbord merges this into
// its method map and awaits any returned promise.
//
// Every handler VALIDATES its params before touching the filesystem, the same
// way the `workspace.*` group does. The table used to cast opaque params
// straight to the C1 request shapes, which turned every malformed request into
// a misleading failure deeper in:
//   * an omitted `path` reached fs as `undefined` and came back as a generic
//     internal error naming an "argument", not the field the client got wrong;
//   * an omitted `expectedRevision` compared `undefined` against the on-disk
//     token and always lost, so a plainly malformed write was reported as a
//     revision CONFLICT and the client offered the user a reload/overwrite
//     dialog for a conflict that did not exist;
//   * a non-numeric `mode` (say the string "x") survived `mode & 0o7777` as 0,
//     so the atomic save chmod'd the user's file to 000 and locked them out of
//     their own data;
//   * a non-numeric `offset` propagated as NaN into Buffer.alloc and failed
//     with an out-of-range message about "size".
// Guard failures are tagged InvalidParamsError, which the dispatcher answers
// with JSON-RPC -32602.

const FILE_ENCODINGS = ["utf-8", "base64"] as const;

// POSIX permission bits plus setuid/setgid/sticky — exactly the range
// writeFile keeps (`mode & 0o7777`).
const MAX_FILE_MODE = 0o7777;

function pathParams<T extends { path: string }>(params: unknown, method: string): T {
    const obj = requireObject(params, method);
    return { path: requireString(obj, "path", method) } as T;
}

function readFileParams(params: unknown, method: string): ReadFileParams {
    const obj = requireObject(params, method);
    const result: ReadFileParams = { path: requireString(obj, "path", method) };
    const offset = optionalIndex(obj, "offset", method);
    if (offset !== undefined) result.offset = offset;
    const length = optionalIndex(obj, "length", method);
    if (length !== undefined) result.length = length;
    return result;
}

function writeFileParams(params: unknown, method: string): WriteFileParams {
    const obj = requireObject(params, method);
    const result: WriteFileParams = {
        path: requireString(obj, "path", method),
        content: requireString(obj, "content", method),
        expectedRevision: requireString(obj, "expectedRevision", method),
    };
    // An unlisted encoding used to fall through to utf-8, which for a client
    // that meant base64 writes the base64 TEXT into the file as if it were the
    // document — a silent corruption reported as a successful save.
    const encoding = optionalOneOf(obj, "encoding", method, FILE_ENCODINGS);
    if (encoding !== undefined) result.encoding = encoding;
    const mode = optionalIntegerInRange(obj, "mode", method, 0, MAX_FILE_MODE);
    if (mode !== undefined) result.mode = mode;
    return result;
}

function resolvePathParams(params: unknown, method: string): ResolvePathParams {
    const obj = requireObject(params, method);
    const result: ResolvePathParams = { path: requireString(obj, "path", method) };
    const base = optionalString(obj, "base", method);
    if (typeof base === "string") result.base = base;
    return result;
}

function unwatchParams(params: unknown, method: string): UnwatchParams {
    const obj = requireObject(params, method);
    return { subscriptionId: requireString(obj, "subscriptionId", method) };
}

export const fileMethods: Record<RpcMethodName, (params: unknown) => unknown | Promise<unknown>> = {
    [RPC_METHODS.stat]: (params) => stat(pathParams<StatParams>(params, RPC_METHODS.stat)),
    [RPC_METHODS.readFile]: (params) => readFile(readFileParams(params, RPC_METHODS.readFile)),
    [RPC_METHODS.writeFile]: (params) => writeFile(writeFileParams(params, RPC_METHODS.writeFile)),
    [RPC_METHODS.resolvePath]: (params) =>
        resolvePath(resolvePathParams(params, RPC_METHODS.resolvePath)),
    [RPC_METHODS.watch]: (params) =>
        fileWatchService.watch(pathParams<WatchParams>(params, RPC_METHODS.watch)),
    [RPC_METHODS.unwatch]: (params) =>
        fileWatchService.unwatch(unwatchParams(params, RPC_METHODS.unwatch)),
    [RPC_METHODS.listDirectory]: (params) =>
        listDirectory(pathParams<ListDirectoryParams>(params, RPC_METHODS.listDirectory)),
};
