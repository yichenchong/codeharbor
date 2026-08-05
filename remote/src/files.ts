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

// "Nothing at this path" has TWO errnos, not one. ENOENT is the obvious one;
// ENOTDIR means a component of the path is not a directory (a file was created
// where a parent directory used to be), which likewise means the path names no
// node. Treating only ENOENT as absent made the watch service SWALLOW that
// second case — statOrUndefined threw, diffAndEmit's catch discarded it, and no
// "deleted" event was ever sent, so an editor kept presenting a file that no
// longer exists as live content.
async function statOrUndefined(target: string): Promise<Stats | undefined> {
    try {
        return await fsp.stat(target);
    } catch (err) {
        if (
            err &&
            typeof err === "object" &&
            "code" in err &&
            (err.code === "ENOENT" || err.code === "ENOTDIR")
        ) {
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

// Upper bound on the bytes ONE UTF-16 code unit can occupy inside a JSON
// string: a control character becomes the six characters `\u00XX`, and neither
// any other escape nor the UTF-8 encoding of a single unit is longer.
const JSON_MAX_BYTES_PER_UNIT = 6;

function assertReadFits(result: ReadFileResult): void {
    // Serializing a 15 MiB payload purely to measure it doubles the peak memory
    // of every large read, so try a cheap sufficient bound first: if even the
    // worst-case escaping of this content fits, no measurement is needed. Only
    // a payload that could plausibly be over the cap pays for the exact count.
    const overhead =
        JSON_MAX_BYTES_PER_UNIT * (result.path.length + result.revision.length) + 128;
    if (result.content.length * JSON_MAX_BYTES_PER_UNIT + overhead <= MAX_FILE_RESPONSE_BYTES) {
        return;
    }
    const bytes = Buffer.byteLength(JSON.stringify(result));
    if (bytes > MAX_FILE_RESPONSE_BYTES) {
        throw new ResourceLimitError(
            `Cannot read ${result.path}: the encoded response is ${bytes} bytes, ` +
                `above this server's ${MAX_FILE_RESPONSE_BYTES}-byte reply limit. ` +
                `Read the file in smaller ranges.`,
        );
    }
}

// Read `length` bytes at `position` into `dest`, looping until the window is
// full or the file ends, and return how many bytes landed.
//
// read(2) is permitted to return FEWER bytes than asked for even for a regular
// file; a single call is not a guarantee. A one-shot read could therefore hand
// back a prefix of the requested window while the result reported it as the
// whole of it — a silently short file in the editor, which would then save that
// prefix over the complete file.
async function readFully(
    handle: FileHandle,
    dest: Buffer,
    length: number,
    position: number,
): Promise<number> {
    let filled = 0;
    while (filled < length) {
        const { bytesRead } = await handle.read(dest, filled, length - filled, position + filled);
        if (bytesRead === 0) break; // end of file
        filled += bytesRead;
    }
    return filled;
}

// Chunk size for reading a file whose reported size is a lie (see readToEnd).
// 64 KiB swallows every /proc and /sys file in a single round trip.
const EOF_READ_CHUNK_BYTES = 64 * 1024;

// Read from byte 0 to end of file WITHOUT trusting the size fstat reported.
//
// Kernel-generated files (/proc, /sys) report size 0 while holding real
// content, so sizing the buffer from stat returned an empty string and
// `truncated: false` — the viewer presented /proc/version as a document that
// genuinely has nothing in it. The full-read ceiling still applies, so this
// cannot allocate without bound.
async function readToEnd(handle: FileHandle, filePath: string): Promise<Buffer> {
    const chunks: Buffer[] = [];
    let total = 0;
    for (;;) {
        const chunk = Buffer.alloc(EOF_READ_CHUNK_BYTES);
        const filled = await readFully(handle, chunk, EOF_READ_CHUNK_BYTES, total);
        if (filled === 0) break;
        total += filled;
        if (total > MAX_FILE_READ_BYTES) {
            throw new ResourceLimitError(
                `Cannot read ${filePath}: the file is larger than this server's ` +
                    `${MAX_FILE_READ_BYTES}-byte full-read limit. Read it in smaller ranges.`,
            );
        }
        chunks.push(filled === chunk.length ? chunk : chunk.subarray(0, filled));
    }
    if (chunks.length === 1) return chunks[0]!;
    return Buffer.concat(chunks, total);
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
    //
    // O_NONBLOCK is a correctness requirement, not a tuning knob: open(2) on a
    // FIFO opened for reading BLOCKS until some other process opens the same
    // FIFO for writing, and fs.promises.open runs that blocking call on libuv's
    // threadpool, which has FOUR threads by default. So four readFile requests
    // naming named pipes — a `.fifo` a build system left in the tree is enough —
    // wedged EVERY filesystem operation in the daemon: no listing, no save, no
    // watch check, for the life of the process, with no error anywhere. Opening
    // non-blocking returns immediately for every file type and is ignored for
    // regular files. Windows has no O_NONBLOCK, hence the fallback to 0.
    const handle = await fsp.open(
        params.path,
        fsConstants.O_RDONLY | (fsConstants.O_NONBLOCK ?? 0),
    );
    try {
        const stats = await handle.stat();
        if (stats.isDirectory()) {
            throw Object.assign(
                new Error(`EISDIR: illegal operation on a directory, read '${params.path}'`),
                { code: "EISDIR" },
            );
        }
        // Anything that is neither a regular file nor a directory — a FIFO, a
        // socket, a character or block device — has no meaningful "contents" to
        // return. fstat reports size 0 for all of them, so the read used to
        // answer with empty content and `truncated: false`, i.e. "this file is
        // empty" for a device holding a whole disk. Say what it is instead.
        if (!stats.isFile()) {
            throw Object.assign(
                new Error(`EINVAL: not a regular file, read '${params.path}'`),
                { code: "EINVAL" },
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
        // The size fstat reported, corrected below when the file turns out to
        // hold more than stat admitted. `truncated` is derived from THIS rather
        // than from the raw stat size, so a kernel-generated file is never
        // reported as a complete read of nothing.
        let fileSize = size;
        // A file that fstat calls empty may not be: /proc and /sys report size 0
        // for files with real content. The only way to know is to read to the
        // end, which for a genuinely empty file costs one read returning
        // nothing. The window, if any, is then applied to what was actually
        // there rather than to a size that was never true.
        if (size === 0) {
            const whole = await readToEnd(handle, params.path);
            fileSize = whole.length;
            const from = Math.min(offset, fileSize);
            const to = wanted === undefined ? fileSize : Math.min(from + wanted, fileSize);
            slice = whole.subarray(from, to);
        } else if (params.offset !== undefined || wanted !== undefined) {
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
                const bytesRead = await readFully(handle, dest, want, offset);
                slice = bytesRead === want ? dest : dest.subarray(0, bytesRead);
            }
        } else {
            // Read exactly the size observed by fstat rather than delegating to
            // readFile(), whose internal growth handling could allocate again if
            // another process appends to the file while this request is running.
            const dest = Buffer.alloc(size);
            const bytesRead = await readFully(handle, dest, size, 0);
            slice = bytesRead === size ? dest : dest.subarray(0, bytesRead);
        }
        // Skipped for a stat-size-0 file: its revision token is minted from a
        // size and an mtime that describe nothing (procfs restamps mtime on
        // every stat, so the comparison would ALWAYS report a change), and the
        // read above went to end of file, so the content is whole by
        // construction.
        const fileChangedDuringRead =
            size > 0 && revisionFrom(await handle.stat()) !== revision;

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
        const start = Math.min(offset, fileSize);
        const end = wanted === undefined ? fileSize : Math.min(start + wanted, fileSize);
        const truncated = fileChangedDuringRead || start > 0 || end < fileSize;

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
        // A throwing close() inside `finally` REPLACES whatever the body threw,
        // so a resource-limit or EISDIR failure would reach the client as an
        // unrelated close error. Nothing is read after this point, so a failure
        // here costs no data; the original diagnosis is worth more.
        await handle.close().catch(() => {});
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
    // The loop above trims one UTF-16 CODE UNIT at a time, and a character
    // outside the Basic Multilingual Plane (an emoji, say) is stored as TWO of
    // them. Dropping only the second half leaves a lone leading half, which
    // encodes as the three bytes of U+FFFD — so the trim could stop with a
    // question-mark diamond glued to the end of the temp name. Drop the orphan.
    const last = stem.length > 0 ? stem.charCodeAt(stem.length - 1) : 0;
    if (last >= 0xd800 && last <= 0xdbff) stem = stem.slice(0, -1);
    return `.${stem}${suffix}`;
}

// A JSON string can carry an UNPAIRED UTF-16 surrogate ("\ud800"), which is not
// a character and has no UTF-8 encoding. Buffer.from(s, "utf-8") does not
// refuse it: it substitutes U+FFFD, so a mangled payload would be written as
// different bytes than the client sent and reported back as a successful save —
// exactly the silent corruption the base64 grammar check below exists to stop,
// so the utf-8 path gets the same treatment.
const UNPAIRED_SURROGATE = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/;

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
    if (encoding !== "base64" && UNPAIRED_SURROGATE.test(params.content)) {
        throw new InvalidParamsError(
            `Invalid utf-8 content for ${params.path}: unpaired surrogate code unit`,
        );
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
    //
    // A rename-over also BREAKS a hard link: the other names for the old inode
    // keep the old bytes. That is inherent to an atomic save — there is no way
    // to replace a file's contents in one step while keeping its inode — and
    // rewriting in place instead would trade a rare surprise for the routine
    // risk of a half-written file, so the atomicity wins.
    const target = existing ? await fsp.realpath(params.path) : await resolveLinkChain(params.path);
    const dir = path.dirname(target);
    const tmp = path.join(dir, tempName(path.basename(target)));
    // An atomic save replaces the file by RENAMING over its name, which needs
    // write permission on the containing DIRECTORY and none whatsoever on the
    // file itself. So a file the user deliberately marked read-only — one that
    // stat() truthfully reported as writable:false a moment earlier — was
    // cheerfully overwritten, where a plain open(O_WRONLY) and every ordinary
    // editor refuse with EACCES. Ask the kernel the same question open() asks.
    if (existing) {
        try {
            await fsp.access(target, fsConstants.W_OK);
        } catch {
            throw Object.assign(
                new Error(`EACCES: permission denied, write '${params.path}'`),
                { code: "EACCES" },
            );
        }
    }
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
        // Preserve ownership. The temp file belongs to the daemon's own user, so
        // renaming it over a file owned by somebody else — the daemon running as
        // root over a user's file, or over a file in a shared group — silently
        // rewrote that file's owner and group, a permission change nobody asked
        // for and that the user only discovers when their own tooling can no
        // longer write it. Best effort: an unprivileged process may not give a
        // file away, and the save is still the right thing to do. Done BEFORE
        // the chmod, because chown clears the setuid and setgid bits.
        const uid = process.getuid?.();
        const gid = process.getgid?.();
        if (
            existing !== undefined &&
            uid !== undefined &&
            gid !== undefined &&
            (existing.uid !== uid || existing.gid !== gid)
        ) {
            await fsp.chown(tmp, existing.uid, existing.gid).catch(() => {});
        }
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

// Wrap a subscriber so a throw inside it stays inside it. The failure is
// reported on stderr — the daemon's diagnostic channel — rather than swallowed
// in silence, because a subscriber that has started throwing is a bug somebody
// needs to see even though the watch service must survive it.
function isolate<A>(callback: (arg: A) => void): (arg: A) => void {
    return (arg: A) => {
        try {
            callback(arg);
        } catch (err) {
            const message = err instanceof Error ? err.message : String(err);
            process.stderr.write(`file-watch: subscriber threw: ${message}\n`);
        }
    };
}

interface Subscription {
    id: string;
    path: string;
    watcher?: FSWatcher;
    // The inode number fs.watch was armed on, or undefined when no watcher is
    // installed. An OS watch handle names an INODE, not a path, so it goes deaf
    // the moment the path is repointed at a different inode — see armWatcher.
    watchedIno?: number;
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

    // Subscribe to change notifications. Returns a disposer.
    //
    // The callback is wrapped so a THROWING subscriber cannot take anything
    // else down with it. EventEmitter.emit runs its listeners in order and lets
    // an exception escape, which skipped every later subscriber and — for the
    // "closed" signal below, emitted straight from unwatch() — turned a
    // successful unwatch into a failed RPC call, and aborted closeAll() halfway
    // through, leaving the rest of the session's watch handles installed on a
    // connection that had already gone away.
    onWatchEvent(callback: WatchCallback): () => void {
        const guarded = isolate(callback);
        this.emitter.on("event", guarded);
        return () => this.emitter.off("event", guarded);
    }

    // Announce that a subscription is gone (unwatch, or closeAll when the SSH
    // channel drops). codeharbord's notification relay may be holding queued
    // events for it while the client's end of the channel is stalled; without
    // this signal that queue would outlive its only possible consumer.
    onWatchClosed(callback: WatchClosedCallback): () => void {
        const guarded = isolate(callback);
        this.emitter.on("closed", guarded);
        return () => this.emitter.off("closed", guarded);
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

        this.armWatcher(sub, existing?.ino);

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

    // Install (or re-install) the fs.watch handle for `sub`.
    //
    // An OS watch handle is bound to the INODE the path named when it was
    // created, not to the path. Every atomic save (SPEC 8.5, and this module's
    // own writeFile) replaces the file by renaming a NEW inode over the old
    // name, so the handle is left watching an unlinked inode and never fires
    // again — verified on Linux: after a rename-over, an in-place append to the
    // same path produced no fs.watch callback at all. The subscription then
    // silently degraded to the once-per-second polling fallback FOREVER, for
    // every file any client had ever saved, which is the exact opposite of the
    // intended "fs.watch is the primary signal" design. Re-arming on the new
    // inode restores it.
    //
    // `ino` is the inode the handle ends up watching; undefined means the path
    // has nothing at it, in which case fs.watch throws (ENOENT) and polling is
    // the only signal until the path appears and a check re-arms.
    private armWatcher(sub: Subscription, ino: number | undefined): void {
        sub.watcher?.close();
        sub.watcher = undefined;
        sub.watchedIno = undefined;
        if (sub.closed) return;
        try {
            const watcher = fsWatch(sub.path, () => {
                void this.reconcile(sub);
            });
            watcher.on("error", () => {
                // A dead handle must not stay in the table pretending to watch:
                // clearing it lets the next observed change re-arm.
                watcher.close();
                if (sub.watcher === watcher) {
                    sub.watcher = undefined;
                    sub.watchedIno = undefined;
                }
            });
            sub.watcher = watcher;
            sub.watchedIno = ino;
        } catch {
            // fs.watch unavailable for this path; polling covers it.
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
            // Nothing at the path: drop the handle so a later re-create arms a
            // fresh one on the new inode instead of trusting a dead handle.
            if (sub.watcher) this.armWatcher(sub, undefined);
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
        // Re-arm whenever the path now names a DIFFERENT inode than the handle
        // was bound to (an atomic save, a delete-and-recreate, a re-pointed
        // symlink), or whenever there is no handle at all because arming failed
        // or the handle errored. Same inode plus a live handle costs nothing.
        if (sub.watcher === undefined || sub.watchedIno !== stats.ino) {
            this.armWatcher(sub, stats.ino);
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

// How many unknown entries are re-checked at once (see listDirectory). Enough
// to keep libuv's four-thread pool busy without queueing a whole directory's
// worth of stat calls at once.
const LISTING_STAT_BATCH = 64;

/**
 * True when readdir could not say what this entry is.
 *
 * The kind normally comes from the directory entry itself (`d_type`), which
 * several filesystems decline to fill in — XFS in some configurations, plenty
 * of network mounts, overlay filesystems — reporting "unknown" instead. Node
 * turns that into a Dirent whose predicates ALL answer false, and `nodeKind`
 * then reports "other" for every single name: the file tree shows no folders
 * at all and the viewer offers nothing to open. Such an entry has to be
 * lstat'd to be classified.
 */
function direntKindUnknown(entry: Dirent): boolean {
    return (
        !entry.isFile() &&
        !entry.isDirectory() &&
        !entry.isSymbolicLink() &&
        !entry.isFIFO() &&
        !entry.isSocket() &&
        !entry.isBlockDevice() &&
        !entry.isCharacterDevice()
    );
}

export async function listDirectory(
    params: ListDirectoryParams,
): Promise<ListDirectoryResult> {
    const dirents = await fsp.readdir(params.path, { withFileTypes: true });
    // Filesystem enumeration order is platform-dependent; sort by the raw
    // filename so the same directory has one stable wire representation.
    dirents.sort((left, right) => (left.name < right.name ? -1 : left.name > right.name ? 1 : 0));
    assertListingFits(params.path, dirents);
    const entries: ListDirectoryResult["entries"] = dirents.map((entry) => ({
        name: entry.name,
        kind: nodeKind(entry),
    }));
    // Re-classify only the entries readdir could not type. On the common
    // filesystems this list is empty and costs one predicate call per name.
    const unknown = dirents.flatMap((entry, index) => (direntKindUnknown(entry) ? [index] : []));
    for (let from = 0; from < unknown.length; from += LISTING_STAT_BATCH) {
        await Promise.all(
            unknown.slice(from, from + LISTING_STAT_BATCH).map(async (index) => {
                const name = entries[index]!.name;
                // A failing lstat — the entry was deleted between the readdir
                // and now, or its parent is not searchable — must NOT abort the
                // listing: one unreadable name is no reason to refuse to show
                // the other thousand, so it keeps the "other" kind it has.
                const stats = await fsp.lstat(path.join(params.path, name)).catch(() => undefined);
                if (stats) entries[index]!.kind = nodeKind(stats);
            }),
        );
    }
    return { path: params.path, entries };
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

// A path field, rejected for an embedded NUL.
//
// A filesystem path is a NUL-terminated byte string, so no name can contain
// one. Node enforces that deep inside fs with a TypeError
// (ERR_INVALID_ARG_VALUE), which the dispatcher can only report as -32603
// "internal error" — telling the user the SERVER broke when in fact the
// REQUEST was malformed. Worse, `resolvePath` never touches the filesystem, so
// it answered a NUL-bearing path with a perfectly ordinary-looking result that
// then failed at every later use. Catch it here, where the answer is -32602
// and names the field.
function requirePath(obj: Record<string, unknown>, field: string, method: string): string {
    const value = requireString(obj, field, method);
    if (value.includes("\0")) {
        throw new InvalidParamsError(
            `${method}: field '${field}' must not contain a NUL character`,
        );
    }
    return value;
}

function pathParams<T extends { path: string }>(params: unknown, method: string): T {
    const obj = requireObject(params, method);
    return { path: requirePath(obj, "path", method) } as T;
}

function readFileParams(params: unknown, method: string): ReadFileParams {
    const obj = requireObject(params, method);
    const result: ReadFileParams = { path: requirePath(obj, "path", method) };
    const offset = optionalIndex(obj, "offset", method);
    if (offset !== undefined) result.offset = offset;
    const length = optionalIndex(obj, "length", method);
    if (length !== undefined) result.length = length;
    return result;
}

function writeFileParams(params: unknown, method: string): WriteFileParams {
    const obj = requireObject(params, method);
    const result: WriteFileParams = {
        path: requirePath(obj, "path", method),
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
    const result: ResolvePathParams = { path: requirePath(obj, "path", method) };
    const base = optionalString(obj, "base", method);
    if (typeof base === "string") {
        if (base.includes("\0")) {
            throw new InvalidParamsError(
                `${method}: field 'base' must not contain a NUL character`,
            );
        }
        result.base = base;
    }
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
