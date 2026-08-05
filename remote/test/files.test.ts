import { test } from "node:test";
import assert from "node:assert/strict";
import { promises as fs } from "node:fs";
import os from "node:os";
import path from "node:path";
import { EventEmitter } from "node:events";
import { spawn } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";
import { fileURLToPath } from "node:url";

import {
    stat,
    readFile,
    writeFile,
    resolvePath,
    listDirectory,
    assertListingFits,
    getMimeType,
    revisionFrom,
    isRevisionMismatch,
    fileWatchService,
    FileWatchService,
    MAX_DIRECTORY_LISTING_BYTES,
    MAX_FILE_READ_BYTES,
    MAX_FILE_RESPONSE_BYTES,
    MAX_WATCH_SUBSCRIPTIONS,
} from "../src/files.ts";
import {
    RPC_REVISION_MISMATCH,
    RPC_RESOURCE_LIMIT,
    RPC_WATCH_EVENT_NOTIFICATION,
    RPC_WATCH_EVENTS_LOST_NOTIFICATION,
} from "../src/rpc-types.ts";
import {
    createWatchNotificationRelay,
    dispatch,
    MAX_LINE_BYTES,
    MAX_PENDING_WATCH_EVENTS,
    RPC_INVALID_PARAMS,
} from "../src/codeharbord.ts";
import type { WatchEvent } from "../src/rpc-types.ts";

// The daemon's CLI entry point, spawned as a real child by the shutdown tests
// below: signal handling is process-wide, so it cannot be exercised in-process
// without tearing down the test runner's own stdin.
const DAEMON_ENTRY = fileURLToPath(new URL("../src/codeharbord.ts", import.meta.url));

async function tmpDir(): Promise<string> {
    return fs.mkdtemp(path.join(os.tmpdir(), "codeharbord-files-"));
}

// Await the actual event the service exposes rather than sleeping a guessed
// duration. `withTimeout` uses a wall-clock guard because real filesystem
// events (fs.watch / polling against the OS clock) cannot be driven by fake
// timers; the guard converts a regression into a failure instead of a hang.
// The executor form is used because `resolve` IS the callback the service
// wants, so there is nothing for Promise.withResolvers to add here.
function firstWatchEvent(service: FileWatchService): Promise<WatchEvent> {
    return new Promise<WatchEvent>((resolve) => {
        service.onWatchEvent(resolve);
    });
}

function withTimeout<T>(work: Promise<T>, ms: number): Promise<T> {
    let handle: NodeJS.Timeout | undefined;
    const guard = new Promise<never>((_, reject) => {
        handle = setTimeout(() => reject(new Error(`timed out after ${ms}ms`)), ms);
        handle.unref?.();
    });
    return Promise.race([work, guard]).finally(() => clearTimeout(handle));
}

test("stat + readFile round-trip a UTF-8 file with a matching revision", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "round.txt");
    await fs.writeFile(file, "round trip");

    const s = await stat({ path: file });
    assert.equal(s.kind, "file");
    assert.equal(s.size, Buffer.byteLength("round trip"));

    const r = await readFile({ path: file });
    assert.equal(r.encoding, "utf-8");
    assert.equal(r.content, "round trip");
    assert.equal(r.truncated, false);
    // stat (lstat) and readFile (stat) mint the same revision for a plain file.
    assert.equal(r.revision, s.revision);

    await fs.rm(dir, { recursive: true, force: true });
});

test("readFile returns base64 for binary content and honors offset/length", async () => {
    const dir = await tmpDir();
    const bin = path.join(dir, "blob.bin");
    await fs.writeFile(bin, Buffer.from([0, 1, 2, 255, 254]));
    const rb = await readFile({ path: bin });
    assert.equal(rb.encoding, "base64");
    assert.equal(Buffer.from(rb.content, "base64").length, 5);

    const text = path.join(dir, "digits.txt");
    await fs.writeFile(text, "0123456789");
    const part = await readFile({ path: text, offset: 2, length: 3 });
    assert.equal(part.encoding, "utf-8");
    assert.equal(part.content, "234");
    assert.equal(part.truncated, true);

    // A read starting at or past the end of a NON-EMPTY file returns no bytes
    // at all, which is as far from "the whole file" as a result can get.
    const past = await readFile({ path: text, offset: 1000 });
    assert.equal(past.content, "");
    assert.equal(past.truncated, true);
    assert.equal(past.encoding, "utf-8");

    await fs.rm(dir, { recursive: true, force: true });
});

// `truncated` means "content is not the whole file", so it must also cover the
// bytes missing BEFORE the window. A tail read used to report truncated=false,
// which tells the editor the buffer is safe to save back — writing a tail over
// the whole file silently deletes everything ahead of the offset.
test("readFile reports a tail read as truncated and an exact read as complete", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "tail.txt");
    await fs.writeFile(file, "0123456789");

    // Offset only, no length: the tail through end of file, still not all of it.
    const tail = await readFile({ path: file, offset: 4 });
    assert.equal(tail.content, "456789");
    assert.equal(tail.truncated, true);

    // A window that covers the file exactly, and one that overshoots it, are
    // both complete reads.
    const exact = await readFile({ path: file, offset: 0, length: 10 });
    assert.equal(exact.content, "0123456789");
    assert.equal(exact.truncated, false);
    const over = await readFile({ path: file, offset: 0, length: 99 });
    assert.equal(over.content, "0123456789");
    assert.equal(over.truncated, false);

    // An EMPTY file has no bytes to miss: any offset returns its whole content.
    const empty = path.join(dir, "empty.txt");
    await fs.writeFile(empty, "");
    const pastEmpty = await readFile({ path: empty, offset: 7, length: 3 });
    assert.equal(pastEmpty.content, "");
    assert.equal(pastEmpty.truncated, false);

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile atomically replaces content and returns a new revision", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "atomic.txt");

    const created = await writeFile({ path: file, content: "hello", expectedRevision: "" });
    const before = await readFile({ path: file });
    assert.equal(before.content, "hello");

    const updated = await writeFile({
        path: file,
        content: "goodbye world",
        expectedRevision: before.revision,
    });
    const after = await readFile({ path: file });
    assert.equal(after.content, "goodbye world");
    // Content AND revision must change on an atomic replace.
    assert.notEqual(after.revision, before.revision);
    assert.equal(after.revision, updated.revision);
    assert.notEqual(updated.revision, created.revision);

    // No stray temp files left in the directory (atomic rename cleaned up).
    const entries = await fs.readdir(dir);
    assert.deepEqual(entries, ["atomic.txt"]);

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile preserves the original file mode when overwriting", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "mode.txt");
    await fs.writeFile(file, "seed");
    await fs.chmod(file, 0o640);

    const current = await stat({ path: file });
    await writeFile({ path: file, content: "changed", expectedRevision: current.revision });

    const st = await fs.stat(file);
    assert.equal(st.mode & 0o777, 0o640);

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile rejects a stale revision with RPC_REVISION_MISMATCH", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "guarded.txt");
    await fs.writeFile(file, "current");

    await assert.rejects(
        () => writeFile({ path: file, content: "clobber", expectedRevision: "0-0" }),
        (err: unknown) => isRevisionMismatch(err) && err.code === RPC_REVISION_MISMATCH,
    );
    // The file was NOT overwritten.
    assert.equal((await readFile({ path: file })).content, "current");

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile create-only fails when the file already exists", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "create-only.txt");
    await writeFile({ path: file, content: "first", expectedRevision: "" });

    await assert.rejects(
        () => writeFile({ path: file, content: "second", expectedRevision: "" }),
        (err: unknown) => isRevisionMismatch(err) && err.code === RPC_REVISION_MISMATCH,
    );
    assert.equal((await readFile({ path: file })).content, "first");

    await fs.rm(dir, { recursive: true, force: true });
});

test("resolvePath flags inside vs outside the repository root", () => {
    const base = path.resolve("/repo/project");

    const inside = resolvePath({ path: "src/index.ts", base });
    assert.equal(inside.insideRepositoryRoot, true);
    assert.equal(inside.path, path.join(base, "src/index.ts"));

    const self = resolvePath({ path: ".", base });
    assert.equal(self.insideRepositoryRoot, true);

    const outside = resolvePath({ path: "../elsewhere/file.ts", base });
    assert.equal(outside.insideRepositoryRoot, false);

    const absoluteOutside = resolvePath({ path: "/etc/hosts", base });
    assert.equal(absoluteOutside.insideRepositoryRoot, false);
    assert.equal(absoluteOutside.path, path.resolve("/etc/hosts"));
});

test("watch emits a WatchEvent when the file changes", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "watched.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 25; // polling fallback covers platforms without fs.watch
    const eventPromise = firstWatchEvent(service);

    // An unreleased fs.watch handle keeps Node's event loop alive, so the
    // release has to survive a failed assertion or a timed-out wait — otherwise
    // a regression hangs the whole runner instead of reporting the failure.
    try {
        const { subscriptionId } = await service.watch({ path: file });
        // A different length guarantees the revision (mtime-size) changes.
        await fs.writeFile(file, "two — different length");

        const event = await withTimeout(eventPromise, 5000);
        service.unwatch({ subscriptionId });

        assert.equal(event.subscriptionId, subscriptionId);
        assert.equal(event.path, file);
        assert.equal(event.event, "modified");
        assert.equal(typeof event.revision, "string");
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});
test("watch re-arms after atomic replacement and sees later edits", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "replaced.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    try {
        const { subscriptionId } = await service.watch({ path: file });

        // Atomic replacement is exactly how writeFile saves. It leaves the
        // original fs.watch handle attached to an unlinked inode on Linux.
        const replacement = path.join(dir, ".replacement");
        await fs.writeFile(replacement, "two");
        await fs.rename(replacement, file);
        await delay(100);

        const eventPromise = firstWatchEvent(service);
        await fs.writeFile(file, "three — after replacement");
        const event = await withTimeout(eventPromise, 5000);

        service.unwatch({ subscriptionId });
        assert.equal(event.event, "modified");
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});

test("watch reports deletion when a parent is replaced by a file", async () => {
    const dir = await tmpDir();
    const parent = path.join(dir, "parent");
    const file = path.join(parent, "nested.txt");
    await fs.mkdir(parent);
    await fs.writeFile(file, "here");

    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    try {
        const { subscriptionId } = await service.watch({ path: file });
        const eventPromise = firstWatchEvent(service);
        await fs.rm(parent, { recursive: true });
        await fs.writeFile(parent, "not a directory");
        const event = await withTimeout(eventPromise, 5000);

        service.unwatch({ subscriptionId });
        assert.equal(event.event, "deleted");
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});


test("listDirectory (RPC) sorts entries, includes hidden files, and classifies them", async () => {
    const dir = await tmpDir();
    await fs.writeFile(path.join(dir, "readme.md"), "# hi");
    await fs.writeFile(path.join(dir, "aaa.txt"), "first");
    await fs.writeFile(path.join(dir, ".hidden"), "secret");
    await fs.mkdir(path.join(dir, "sub"));

    const result = await listDirectory({ path: dir });
    assert.equal(result.path, dir);
    assert.deepEqual(
        result.entries.map((entry) => entry.name),
        [".hidden", "aaa.txt", "readme.md", "sub"],
    );
    const byName: Record<string, string> = {};
    for (const entry of result.entries) byName[entry.name] = entry.kind;
    assert.equal(byName["readme.md"], "file");
    assert.equal(byName["sub"], "directory");
    assert.equal(byName[".hidden"], "file");

    assert.equal(getMimeType("a/b/readme.md"), "text/markdown");
    assert.equal(getMimeType("photo.PNG"), "image/png");
    assert.equal(getMimeType("mystery.xyz"), "application/octet-stream");

    // revisionFrom folds mtime, ctime, inode, and size so a same-size edit
    // within one mtime tick still changes the token (SPEC 8.6).
    assert.equal(
        revisionFrom({ mtimeMs: 12.5, ctimeMs: 13.5, ino: 42, size: 7 }),
        "12.5-13.5-42-7",
    );

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile rejects a non-empty revision when the file was deleted", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "gone.txt");
    await fs.writeFile(file, "loaded");
    const loaded = await stat({ path: file });
    await fs.rm(file);

    // A non-empty expectedRevision means the client loaded an existing file; if
    // it is gone now, that is a conflict (SPEC 8.6), not a silent recreate.
    await assert.rejects(
        () => writeFile({ path: file, content: "recreated", expectedRevision: loaded.revision }),
        (err: unknown) => isRevisionMismatch(err) && err.code === RPC_REVISION_MISMATCH,
    );
    // The file was NOT recreated behind the client's back.
    assert.equal(await fs.access(file).then(() => true, () => false), false);

    await fs.rm(dir, { recursive: true, force: true });
});

test("watch emits a deleted WatchEvent when the file is removed", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "vanish.txt");
    await fs.writeFile(file, "here");

    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    const eventPromise = firstWatchEvent(service);

    try {
        const { subscriptionId } = await service.watch({ path: file });
        await fs.rm(file);

        const event = await withTimeout(eventPromise, 5000);
        service.unwatch({ subscriptionId });

        assert.equal(event.event, "deleted");
        assert.equal(event.revision, undefined);
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});

test("readFile clamps a negative offset to the start of the file", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "neg.txt");
    await fs.writeFile(file, "0123456789");


    // A negative offset must NOT index from the end of the buffer
    // (Buffer.subarray semantics); it is clamped to 0 so the whole file reads.
    const r = await readFile({ path: file, offset: -3 });
    assert.equal(r.content, "0123456789");
    assert.equal(r.truncated, false);

    await fs.rm(dir, { recursive: true, force: true });
});

test("readFile returns lossless base64 when a byte range cuts a multibyte codepoint", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "utf8.txt");
    await fs.writeFile(file, "café", "utf-8"); // 'é' is 0xC3 0xA9 (2 bytes)

    // Bytes [0,4) split the 'é' in half -> not valid UTF-8 -> base64, byte-exact.
    const r = await readFile({ path: file, offset: 0, length: 4 });
    assert.equal(r.encoding, "base64");
    assert.deepEqual(Buffer.from(r.content, "base64"), Buffer.from("café", "utf-8").subarray(0, 4));
    assert.equal(r.truncated, true);

    await fs.rm(dir, { recursive: true, force: true });
});

test("readFile handles an empty file", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "empty.txt");
    await fs.writeFile(file, "");

    const r = await readFile({ path: file });
    assert.equal(r.content, "");
    assert.equal(r.encoding, "utf-8");
    assert.equal(r.truncated, false);

    await fs.rm(dir, { recursive: true, force: true });
});

// Create a named pipe. `mkfifo` is a real process because Node has no mknod
// binding; every caller needs the same four lines, so they live here once.
async function makeFifo(fifoPath: string): Promise<void> {
    await new Promise<void>((resolve, reject) => {
        const child = spawn("mkfifo", [fifoPath]);
        child.once("error", reject);
        child.once("exit", (code) => (code === 0 ? resolve() : reject(new Error(`mkfifo exited ${code}`))));
    });
}

test("readFile rejects a FIFO without blocking the daemon", async () => {
    const dir = await tmpDir();
    const fifo = path.join(dir, "pipe");
    await makeFifo(fifo);

    await assert.rejects(
        () => readFile({ path: fifo }),
        (err: unknown) => err instanceof Error && "code" in err && err.code === "EINVAL",
    );
    await fs.rm(dir, { recursive: true, force: true });
});

test("readFile rejects a directory with EISDIR", async () => {
    const dir = await tmpDir();

    await assert.rejects(
        () => readFile({ path: dir }),
        (err: unknown) => err instanceof Error && "code" in err && err.code === "EISDIR",
    );

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile decodes base64 content and reads it back byte-exact", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "blob.bin");
    const bytes = Buffer.from([0, 1, 2, 255, 254, 128]);

    await writeFile({
        path: file,
        content: bytes.toString("base64"),
        encoding: "base64",
        expectedRevision: "",
    });
    assert.deepEqual(await fs.readFile(file), bytes);

    const r = await readFile({ path: file });
    assert.equal(r.encoding, "base64");
    assert.deepEqual(Buffer.from(r.content, "base64"), bytes);

    await fs.rm(dir, { recursive: true, force: true });
});
test("writeFile rejects unpaired UTF-16 surrogates instead of replacing them", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "invalid.txt");

    await assert.rejects(
        () => writeFile({ path: file, content: "\ud800", expectedRevision: "" }),
        /unpaired surrogate/,
    );
    assert.equal(await fs.access(file).then(() => true, () => false), false);
    await fs.rm(dir, { recursive: true, force: true });
});


test("concurrent writeFile with the same revision: exactly one wins", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "race.txt");
    const created = await writeFile({ path: file, content: "seed", expectedRevision: "" });

    // Both writes carry the SAME expectedRevision. The per-path lock must let
    // only the first commit; the second re-checks against the now-changed
    // revision and fails with RPC_REVISION_MISMATCH — no silent lost update.
    const results = await Promise.allSettled([
        writeFile({ path: file, content: "AAAA", expectedRevision: created.revision }),
        writeFile({ path: file, content: "BBBB", expectedRevision: created.revision }),
    ]);

    const fulfilled = results.filter((r) => r.status === "fulfilled");
    const rejected = results.filter((r) => r.status === "rejected");
    assert.equal(fulfilled.length, 1);
    assert.equal(rejected.length, 1);
    const reason = (rejected[0] as PromiseRejectedResult).reason;
    assert.ok(isRevisionMismatch(reason) && reason.code === RPC_REVISION_MISMATCH);

    // The surviving content is one writer's, intact — never a torn interleave.
    const final = (await readFile({ path: file })).content;
    assert.ok(final === "AAAA" || final === "BBBB");

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile rejects an old revision after a same-size external edit", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "samesize.txt");

    // Whole-second timestamp so utimes round-trips losslessly through mtimeMs.
    const t = new Date(Math.floor(Date.now() / 1000) * 1000);
    await fs.writeFile(file, "AAAA");
    await fs.utimes(file, t, t);
    const loaded = await stat({ path: file });

    // An external editor rewrites the SAME number of bytes, then mtime is forced
    // back to the original so an mtime+size token would ALIAS. Only ctime (which
    // utimes cannot rewind) still distinguishes the versions.
    await fs.writeFile(file, "BBBB");
    await fs.utimes(file, t, t);
    const after = await stat({ path: file });

    assert.equal(after.size, loaded.size); // same size
    assert.equal(after.mtimeMs, loaded.mtimeMs); // same mtime -> old token aliases
    assert.notEqual(after.revision, loaded.revision); // new token distinguishes

    await assert.rejects(
        () => writeFile({ path: file, content: "CCCC", expectedRevision: loaded.revision }),
        (err: unknown) => isRevisionMismatch(err) && err.code === RPC_REVISION_MISMATCH,
    );
    // The external edit survived; the stale save did not clobber it.
    assert.equal((await readFile({ path: file })).content, "BBBB");

    await fs.rm(dir, { recursive: true, force: true });
});

test("dispatch returns no response for a JSON-RPC notification (no id)", async () => {
    // Absent id -> notification: dispatched for side effects, no response line.
    assert.equal(await dispatch({ jsonrpc: "2.0", method: "ping" }), null);
    // Unknown-method notifications are also silent.
    assert.equal(await dispatch({ jsonrpc: "2.0", method: "nope.nope" }), null);

    // A normal request (id present) still gets a response.
    const response = await dispatch({ jsonrpc: "2.0", id: 1, method: "ping" });
    assert.ok(response !== null);
    assert.ok("result" in response);
    assert.equal(response.id, 1);
});

test("ranged readFile returns only the window of a huge (sparse) file", async () => {
    const dir = await tmpDir();
    const big = path.join(dir, "big.bin");

    // 3 GiB sparse file: exceeds the ~2 GiB whole-buffer read limit, so a
    // full-file read would throw. The ranged path must read ONLY the window.
    const size = 3 * 1024 * 1024 * 1024;
    const marker = Buffer.from("HELLO-WINDOW");
    const offset = 1_000_000;
    const handle = await fs.open(big, "w");
    try {
        await handle.truncate(size);
        await handle.write(marker, 0, marker.length, offset);
    } finally {
        await handle.close();
    }

    const win = await readFile({ path: big, offset, length: marker.length });
    assert.equal(win.encoding, "utf-8");
    assert.equal(win.content, "HELLO-WINDOW");
    assert.equal(win.truncated, true); // the file extends well past the window

    await fs.rm(dir, { recursive: true, force: true });
});

test("readFile refuses oversized whole reads and oversized encoded responses", async () => {
    const dir = await tmpDir();
    const sparse = path.join(dir, "sparse.bin");
    const handle = await fs.open(sparse, "w");
    try {
        await handle.truncate(MAX_FILE_READ_BYTES + 1);
    } finally {
        await handle.close();
    }

    const isResourceLimit = (err: unknown): boolean =>
        err instanceof Error && "code" in err && err.code === RPC_RESOURCE_LIMIT;
    await assert.rejects(() => readFile({ path: sparse }), isResourceLimit);
    await assert.rejects(
        () => readFile({ path: sparse, offset: 0, length: MAX_FILE_READ_BYTES + 1 }),
        isResourceLimit,
    );

    // A raw read can be under the allocation cap but still expand beyond the
    // transport cap when JSON escapes control bytes in the text payload.
    const escaped = path.join(dir, "escaped.txt");
    await fs.writeFile(escaped, "\u0001".repeat(Math.ceil(MAX_FILE_RESPONSE_BYTES / 6)));
    await assert.rejects(() => readFile({ path: escaped }), isResourceLimit);

    await fs.rm(dir, { recursive: true, force: true });
});

test("watch emits no WatchEvent for a subscription after unwatch", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "unwatched.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    const seen: WatchEvent[] = [];
    service.onWatchEvent((event) => seen.push(event));

    try {
        const first = await service.watch({ path: file });
        // Release the first subscription, then open a fresh one on the same file.
        // Awaiting the FRESH subscription's event (a real signal, no sleep) gives
        // the released one every chance to wrongly fire — including the in-flight
        // diffAndEmit-after-unwatch race the closed guard must swallow.
        service.unwatch({ subscriptionId: first.subscriptionId });
        const second = await service.watch({ path: file });

        const next = firstWatchEvent(service);
        await fs.writeFile(file, "two — a clearly different length");
        const event = await withTimeout(next, 5000);

        assert.equal(event.subscriptionId, second.subscriptionId);
        // The released subscription never emitted for anyone.
        assert.ok(seen.every((e) => e.subscriptionId !== first.subscriptionId));
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});

test("resolvePath treats an in-repo name starting with '..' as inside", () => {
    const base = path.resolve("/repo/project");

    // "..config" is a real in-repo filename, NOT a parent-directory escape.
    const dotName = resolvePath({ path: "..config", base });
    assert.equal(dotName.insideRepositoryRoot, true);
    assert.equal(dotName.path, path.join(base, "..config"));

    // A genuine parent escape ("..") is still flagged outside.
    assert.equal(resolvePath({ path: "..", base }).insideRepositoryRoot, false);
    assert.equal(resolvePath({ path: "../x", base }).insideRepositoryRoot, false);
});

test("readFile keeps a UTF-8 byte-order mark, and writeFile puts it back byte-exact", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "bom.txt");
    const bytes = Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), Buffer.from("hello", "utf-8")]);
    await fs.writeFile(file, bytes);

    // A BOM is valid UTF-8, so the file is text — and the mark must survive as
    // U+FEFF in the content. A decoder that strips it (TextDecoder's default)
    // would hand the editor a buffer three bytes shorter than the file, and the
    // next save would silently delete the mark.
    const r = await readFile({ path: file });
    assert.equal(r.encoding, "utf-8");
    assert.equal(r.content, "\uFEFFhello");

    await writeFile({ path: file, content: r.content, expectedRevision: r.revision });
    assert.deepEqual(await fs.readFile(file), bytes);

    await fs.rm(dir, { recursive: true, force: true });
});

test("stat, readFile and writeFile agree on the revision through a symlink", async () => {
    const dir = await tmpDir();
    const real = path.join(dir, "real.txt");
    const link = path.join(dir, "link.txt");
    await fs.writeFile(real, "original");
    await fs.symlink(real, link);

    const s = await stat({ path: link });
    assert.equal(s.kind, "symlink"); // the entry itself is still reported as a link
    const r = await readFile({ path: link });
    // readFile and writeFile both FOLLOW the link, so stat's token must name the
    // target too; otherwise a save through a symlink is a permanent conflict.
    assert.equal(s.revision, r.revision);

    await writeFile({ path: link, content: "replaced", expectedRevision: s.revision });
    assert.equal(await fs.readFile(real, "utf-8"), "replaced");
    // The link was not severed by the atomic rename.
    assert.equal((await fs.lstat(link)).isSymbolicLink(), true);

    const listing = await listDirectory({ path: dir });
    const kinds: Record<string, string> = {};
    for (const entry of listing.entries) kinds[entry.name] = entry.kind;
    assert.equal(kinds["link.txt"], "symlink");
    assert.equal(kinds["real.txt"], "file");

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile rejects malformed base64 instead of writing corrupt bytes", async () => {
    const dir = await tmpDir();
    const junk = path.join(dir, "junk.bin");

    // Buffer.from(s, "base64") silently drops characters outside the alphabet,
    // so without validation this would "succeed" and store the wrong bytes.
    await assert.rejects(
        () => writeFile({ path: junk, content: "!!! not base64 !!!", encoding: "base64", expectedRevision: "" }),
        /Invalid base64 content/,
    );
    assert.equal(await fs.access(junk).then(() => true, () => false), false);
    const rpcTarget = path.join(dir, "rpc-invalid.bin");
    const rpcError = await callFile("file.writeFile", {
        path: rpcTarget,
        content: "!!! not base64 !!!",
        encoding: "base64",
        expectedRevision: "",
    });
    assert.equal(rpcError.code, RPC_INVALID_PARAMS);
    assert.match(rpcError.message, /Invalid base64 content/);
    assert.equal(await fs.access(rpcTarget).then(() => true, () => false), false);

    // A payload cut mid-group has a stripped length of 1 mod 4, which encodes no
    // byte string: Buffer would discard the partial group and write short data.
    const truncated = path.join(dir, "truncated.bin");
    await assert.rejects(
        () => writeFile({ path: truncated, content: "QUJDR", encoding: "base64", expectedRevision: "" }),
        /Invalid base64 content/,
    );
    assert.equal(await fs.access(truncated).then(() => true, () => false), false);
    // Padding is part of the base64 grammar: a lone "=" or more than two
    // trailing "=" characters must not be silently accepted as an empty or
    // shortened payload.
    for (const [index, content] of ["=", "aGk===", "ab=", "a=="].entries()) {
        const target = path.join(dir, `bad-padding-${index}.bin`);
        await assert.rejects(
            () => writeFile({ path: target, content, encoding: "base64", expectedRevision: "" }),
            /Invalid base64 content/,
        );
        assert.equal(await fs.access(target).then(() => true, () => false), false);
    }

    await fs.rm(dir, { recursive: true, force: true });
});

test("a revision-mismatch error carries the current revision for the conflict dialog", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "conflict.txt");
    await fs.writeFile(file, "on disk");
    const current = await stat({ path: file });

    // The client reads `currentRevision` out of the error data to offer
    // reload/overwrite (SPEC 8.6) without a second stat round-trip.
    const stale = await writeFile({
        path: file,
        content: "mine",
        expectedRevision: "1-1-1-1",
    }).then(() => undefined, (err: unknown) => err);
    if (!isRevisionMismatch(stale)) throw new Error("expected a revision mismatch");
    assert.deepEqual(stale.data, {
        path: file,
        expected: "1-1-1-1",
        currentRevision: current.revision,
    });

    // The create-only guard reports it too, so "it already exists" can offer the
    // same choice rather than a bare failure.
    const exists = await writeFile({ path: file, content: "mine", expectedRevision: "" })
        .then(() => undefined, (err: unknown) => err);
    if (!isRevisionMismatch(exists)) throw new Error("expected a revision mismatch");
    assert.deepEqual(exists.data, { path: file, currentRevision: current.revision });
    const rpcResponse = await dispatch({
        jsonrpc: "2.0",
        id: 8,
        method: "file.writeFile",
        params: { path: file, content: "mine", expectedRevision: "1-1-1-1" },
    });
    assert.ok(rpcResponse && "error" in rpcResponse);
    assert.equal(rpcResponse.error.code, RPC_REVISION_MISMATCH);

    await fs.rm(dir, { recursive: true, force: true });
});

test("watch on a path that does not exist yet emits created when it appears", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "later.txt");

    // fs.watch cannot attach to a missing path (it throws ENOENT), so this is
    // the polling fallback's job — and the transition must be "created", not
    // "modified", because the subscription started with no revision at all.
    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    const eventPromise = firstWatchEvent(service);

    try {
        const { subscriptionId } = await service.watch({ path: file });
        await fs.writeFile(file, "now I exist");

        const event = await withTimeout(eventPromise, 5000);

        assert.equal(event.subscriptionId, subscriptionId);
        assert.equal(event.event, "created");
        assert.equal(typeof event.revision, "string");
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});

test("writeFile creates a missing parent directory for a new file", async () => {
    const dir = await tmpDir();
    // This is the crash-recovery snapshot shape (SPEC 11.3): a directory beside
    // the edited file that nothing else ever creates. The frozen method catalog
    // has no createDirectory, so writeFile is the only thing that can bring the
    // path into being — without this the very first snapshot fails with ENOENT
    // and recovery is silently dead.
    const nested = path.join(dir, ".codeharbor-recovery", "deeper", "notes.txt");

    const created = await writeFile({
        path: nested,
        content: "unsaved work",
        expectedRevision: "",
    });
    assert.equal((await readFile({ path: nested })).content, "unsaved work");
    assert.notEqual(created.revision, "");

    // A directory this service invented stays private to its owner: it can hold
    // unsaved user work. (Asserting "no group/other access" rather than exactly
    // 0700 because the process umask can only take permission bits away.)
    const parent = await fs.stat(path.dirname(nested));
    assert.equal(parent.mode & 0o077, 0);

    await fs.rm(dir, { recursive: true, force: true });
});

// The atomic save writes a temp file NAMED AFTER the target in the same
// directory and renames it over the original. A file name may be up to 255
// bytes, and the temp name adds 18 characters, so a long — but perfectly
// legal — name overflowed the limit: open() failed with ENAMETOOLONG and every
// save of that file was an error the user could do nothing about.
test("writeFile saves a file whose name nearly fills the 255-byte limit", async () => {
    const dir = await tmpDir();
    const name = "n".repeat(250);
    const file = path.join(dir, name);

    const created = await writeFile({ path: file, content: "first", expectedRevision: "" });
    assert.equal(await fs.readFile(file, "utf-8"), "first");

    // Overwriting takes the other branch of the save (mode preserved, target
    // resolved through realpath) and must clear the same hurdle.
    await writeFile({ path: file, content: "second", expectedRevision: created.revision });
    assert.equal(await fs.readFile(file, "utf-8"), "second");

    // Neither save left its temp file behind.
    assert.deepEqual(await fs.readdir(dir), [name]);

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile honors an explicit mode on create (masks to 0o600)", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "recovery.snapshot");

    // C1/ED15: a recovery snapshot is written private (0o600). The final file
    // must carry exactly that mode, not a umask-loosened one.
    await writeFile({ path: file, content: "unsaved", expectedRevision: "", mode: 0o600 });

    const st = await fs.stat(file);
    assert.equal(st.mode & 0o777, 0o600);
    assert.equal((await readFile({ path: file })).content, "unsaved");

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile honors an explicit mode when overwriting", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "pinned.txt");
    await fs.writeFile(file, "seed");
    await fs.chmod(file, 0o644);

    const current = await stat({ path: file });
    // An explicit mode wins over the preserved 0o644 mode.
    await writeFile({ path: file, content: "changed", expectedRevision: current.revision, mode: 0o600 });

    const st = await fs.stat(file);
    assert.equal(st.mode & 0o777, 0o600);

    await fs.rm(dir, { recursive: true, force: true });
});

test("stat reports writable true for a writable file and false for a read-only one", async () => {
    const dir = await tmpDir();
    const writableFile = path.join(dir, "rw.txt");
    const readOnlyFile = path.join(dir, "ro.txt");
    await fs.writeFile(writableFile, "rw");
    await fs.writeFile(readOnlyFile, "ro");
    await fs.chmod(writableFile, 0o644);
    await fs.chmod(readOnlyFile, 0o444);

    assert.equal((await stat({ path: writableFile })).writable, true);
    assert.equal((await stat({ path: readOnlyFile })).writable, false);

    await fs.rm(dir, { recursive: true, force: true });
});

test("stat writable reflects the link-followed target, not the link", async () => {
    const dir = await tmpDir();
    const real = path.join(dir, "target.txt");
    const link = path.join(dir, "link.txt");
    await fs.writeFile(real, "data");
    await fs.chmod(real, 0o444);
    await fs.symlink(real, link);

    // X12: the link node is writable but the target it resolves to is not, so
    // writability must follow the link and report false.
    const s = await stat({ path: link });
    assert.equal(s.kind, "symlink");
    assert.equal(s.writable, false);

    await fs.rm(dir, { recursive: true, force: true });
});

test("watch coalesces a burst of rapid changes and observes the final revision", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "busy.log");
    await fs.writeFile(file, "start");

    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    const events: WatchEvent[] = [];
    service.onWatchEvent((e) => events.push(e));

    try {
        const { subscriptionId } = await service.watch({ path: file });

        // Append in a tight loop so change signals arrive faster than a check can
        // complete: RF14's coalescing must still let the LAST state through — no
        // lost events — even though intermediate signals collapse into one queued
        // check rather than one check per signal.
        let content = "start";
        for (let i = 0; i < 50; i += 1) {
            content += ` ${i}`;
            await fs.writeFile(file, content);
        }
        const finalRevision = revisionFrom(await fs.stat(file));

        // The final revision must eventually be observed by an emitted event.
        await withTimeout(
            new Promise<void>((resolve) => {
                const seen = events.some((e) => e.revision === finalRevision);
                if (seen) return resolve();
                const off = service.onWatchEvent((e) => {
                    if (e.revision === finalRevision) {
                        off();
                        resolve();
                    }
                });
            }),
            5000,
        );

        service.unwatch({ subscriptionId });
        assert.equal(events.at(-1)?.revision, finalRevision);
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});

// --- file.* param validation -------------------------------------------------
//
// Every file method validates its params before touching the filesystem. These
// pin the four failures the unvalidated table used to produce: a misleading
// "internal error", a phantom revision CONFLICT, a chmod to 000, and an
// out-of-range Buffer error.

async function callFile(method: string, params: unknown) {
    const response = await dispatch({ jsonrpc: "2.0", id: 7, method, params });
    assert.ok(response !== null && "error" in response, `${method} should have failed`);
    return response.error;
}

test("file methods reject a missing or non-string path with Invalid params", async () => {
    for (const method of ["file.stat", "file.readFile", "file.resolvePath", "file.watch", "file.listDirectory"]) {
        const missing = await callFile(method, {});
        assert.equal(missing.code, RPC_INVALID_PARAMS, `${method} with no path`);
        assert.match(missing.message, /field 'path'/);

        const wrongType = await callFile(method, { path: 42 });
        assert.equal(wrongType.code, RPC_INVALID_PARAMS, `${method} with a numeric path`);
    }

    const unwatch = await callFile("file.unwatch", {});
    assert.equal(unwatch.code, RPC_INVALID_PARAMS);
    assert.match(unwatch.message, /field 'subscriptionId'/);
});

// An omitted expectedRevision used to be compared against the on-disk token and
// always lose, so a plainly malformed request came back as a revision CONFLICT
// (-32001) — which makes the editor offer the user a reload/overwrite choice
// for a conflict that never happened.
test("file.writeFile reports a missing expectedRevision as Invalid params, not a conflict", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "no-rev.txt");

    const error = await callFile("file.writeFile", { path: file, content: "hello" });
    assert.equal(error.code, RPC_INVALID_PARAMS);
    assert.notEqual(error.code, RPC_REVISION_MISMATCH);
    assert.match(error.message, /field 'expectedRevision'/);
    // The rejected request wrote nothing.
    await assert.rejects(fs.stat(file));

    await fs.rm(dir, { recursive: true, force: true });
});

// `mode & 0o7777` turns any non-numeric value into 0, so a client bug that sent
// the mode as a string used to save the file and then chmod it to 000 — locking
// the user out of their own document.
test("file.writeFile rejects a non-integer or out-of-range mode instead of chmod 000", async () => {
    const dir = await tmpDir();

    for (const mode of ["0600", 0.5, -1, 0o10000, null]) {
        const target = path.join(dir, `mode-${String(mode)}.txt`);
        const error = await callFile("file.writeFile", {
            path: target,
            content: "x",
            expectedRevision: "",
            mode,
        });
        assert.equal(error.code, RPC_INVALID_PARAMS, `mode ${String(mode)} must be rejected`);
        assert.match(error.message, /field 'mode'/);
        await assert.rejects(fs.stat(target), `mode ${String(mode)} must not create a file`);
    }

    // A legitimate mode still works and is pinned exactly.
    const ok = path.join(dir, "ok.txt");
    const response = await dispatch({
        jsonrpc: "2.0",
        id: 1,
        method: "file.writeFile",
        params: { path: ok, content: "x", expectedRevision: "", mode: 0o600 },
    });
    assert.ok(response !== null && "result" in response);
    assert.equal((await fs.stat(ok)).mode & 0o7777, 0o600);

    await fs.rm(dir, { recursive: true, force: true });
});

// An unlisted encoding used to fall through to utf-8. For a client that meant
// base64 that writes the base64 TEXT into the file as if it were the document,
// and reports the corruption as a successful save.
test("file.writeFile rejects an unknown encoding rather than defaulting to utf-8", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "enc.bin");

    const error = await callFile("file.writeFile", {
        path: file,
        content: "aGk=",
        expectedRevision: "",
        encoding: "utf16",
    });
    assert.equal(error.code, RPC_INVALID_PARAMS);
    assert.match(error.message, /field 'encoding'/);
    await assert.rejects(fs.stat(file));

    // Both listed encodings still round-trip.
    for (const [encoding, content, expected] of [
        ["utf-8", "plain", "plain"],
        ["base64", "aGk=", "hi"],
    ] as const) {
        const target = path.join(dir, `enc-${encoding}.txt`);
        const response = await dispatch({
            jsonrpc: "2.0",
            id: 1,
            method: "file.writeFile",
            params: { path: target, content, expectedRevision: "", encoding },
        });
        assert.ok(response !== null && "result" in response);
        assert.equal(await fs.readFile(target, "utf8"), expected);
    }

    await fs.rm(dir, { recursive: true, force: true });
});

test("file.readFile rejects a non-integer or negative offset/length", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "range.txt");
    await fs.writeFile(file, "0123456789");

    for (const params of [
        { path: file, offset: "3" },
        { path: file, offset: -1 },
        { path: file, offset: 1.5 },
        { path: file, length: Number.NaN },
        { path: file, length: -4 },
        // Beyond Number.MAX_SAFE_INTEGER an integer no longer round-trips as
        // itself, so it is not a byte count anything downstream can honour.
        { path: file, offset: Number.MAX_SAFE_INTEGER + 1 },
        { path: file, length: Number.POSITIVE_INFINITY },
    ]) {
        const error = await callFile("file.readFile", params);
        assert.equal(error.code, RPC_INVALID_PARAMS, JSON.stringify(params));
        assert.match(error.message, /non-negative integer/);
    }

    // A well-formed window still reads. Goes through the typed readFile API so
    // the assertion reads a checked shape rather than an inline cast of an
    // opaque RPC result.
    const result = await readFile({ path: file, offset: 2, length: 3 });
    assert.equal(result.content, "234");
    assert.equal(result.truncated, true);

    await fs.rm(dir, { recursive: true, force: true });
});

// --- Bounded watch-notification relay ---------------------------------------
//
// The daemon writes watch notifications to stdout, which SSH forwards. When the
// client's end stalls, an unchecked write() lets Node buffer without bound and
// a churning directory eventually kills the daemon — taking the whole workspace
// connection with it. createWatchNotificationRelay bounds that queue, coalesces
// per (subscription, path), and reports whatever it still had to drop.

// A stand-in for stdout whose stall is under the test's control: while
// `stalled`, write() reports a full buffer exactly as a real stream does (the
// chunk is still accepted — that is what an unbounded internal buffer IS), and
// drain() releases it. No wall clock anywhere.
function fakeOut(): {
    out: NodeJS.WritableStream;
    lines: string[];
    stall: () => void;
    drain: () => void;
} {
    const emitter = new EventEmitter();
    const lines: string[] = [];
    let stalled = false;
    const out = {
        write(chunk: string): boolean {
            lines.push(chunk);
            return !stalled;
        },
        on(event: string, handler: () => void): unknown {
            emitter.on(event, handler);
            return out;
        },
    } as unknown as NodeJS.WritableStream;
    return {
        out,
        lines,
        stall: () => {
            stalled = true;
        },
        drain: () => {
            stalled = false;
            emitter.emit("drain");
        },
    };
}

function relayEvent(subscriptionId: string, filePath: string, revision: string): WatchEvent {
    return { subscriptionId, path: filePath, event: "modified", revision };
}

// Parse the relay's output lines into (method, params) pairs.
function parseLines(lines: string[]): { method: string; params: Record<string, unknown> }[] {
    return lines.map((line) => {
        const message = JSON.parse(line) as {
            method: string;
            params: Record<string, unknown>;
        };
        return { method: message.method, params: message.params };
    });
}

test("a stalled consumer cannot grow the relay's queue past its bound", () => {
    const sink = fakeOut();
    // Every subscription is live, so nothing is dropped for being unknown: the
    // bound is the only thing that can stop the queue growing.
    const live = new Set<string>();
    const relay = createWatchNotificationRelay(sink.out, (id) => live.has(id));

    sink.stall();
    // Far more DISTINCT subscriptions than the bound allows — distinct keys are
    // the only thing coalescing cannot absorb.
    const total = MAX_PENDING_WATCH_EVENTS * 3;
    for (let i = 0; i < total; i += 1) {
        const id = `sub-${i}`;
        live.add(id);
        relay.deliver(relayEvent(id, `/w/${i}.txt`, `r${i}`));
    }

    // The first delivery went out before the stall was observed (its write is
    // what reports the full buffer); everything after it is queued, capped.
    assert.equal(sink.lines.length, 1);
    assert.ok(
        relay.pendingCount() <= MAX_PENDING_WATCH_EVENTS,
        `queued ${relay.pendingCount()} notifications, above the ${MAX_PENDING_WATCH_EVENTS} bound`,
    );
});

// The relay shares stdout with the RPC response writer and only learns of a
// stall from the return value of its OWN writes. A large response that filled
// the stream's buffer therefore left the relay believing the output was still
// flowing, and every notification after it went straight into an internal
// buffer with no bound — exactly the failure the queue exists to prevent.
// runStdio reports that foreign stall through relay.stall().
test("a stall caused by a response write still bounds the relay's queue", () => {
    const sink = fakeOut();
    const live = new Set(["sub-1", "sub-2"]);
    const relay = createWatchNotificationRelay(sink.out, (id) => live.has(id));

    // An RPC response — written by runStdio, not by the relay — fills the
    // stream's buffer and reports the stall.
    sink.stall();
    relay.stall();

    relay.deliver(relayEvent("sub-1", "/w/a.txt", "r1"));
    relay.deliver(relayEvent("sub-2", "/w/b.txt", "r1"));
    assert.deepEqual(sink.lines, [], "a notification must not be written into a full buffer");
    assert.equal(relay.pendingCount(), 2);

    sink.drain();
    assert.equal(relay.pendingCount(), 0);
    assert.equal(sink.lines.length, 2);
});

test("the relay coalesces a burst for one path into a single latest notification", () => {
    const sink = fakeOut();
    const live = new Set(["sub-1"]);
    const relay = createWatchNotificationRelay(sink.out, (id) => live.has(id));

    sink.stall();
    // A build rewriting one watched file thousands of times. Each notification
    // only says "re-read this path", so the newest one carries everything the
    // older ones did — and its revision is the current on-disk one.
    for (let i = 0; i < 5000; i += 1) {
        relay.deliver(relayEvent("sub-1", "/w/a.txt", `r${i}`));
    }
    assert.equal(relay.pendingCount(), 1);

    sink.drain();
    const messages = parseLines(sink.lines);
    // One pre-stall write plus one coalesced flush; no loss report, because
    // coalescing lost no information.
    assert.equal(messages.length, 2);
    assert.ok(messages.every((m) => m.method === RPC_WATCH_EVENT_NOTIFICATION));
    assert.equal((messages[1]?.params as unknown as WatchEvent).revision, "r4999");
    assert.equal(relay.pendingCount(), 0);
});

test("the relay reports lost events only when the bound actually dropped some", () => {
    const sink = fakeOut();
    const live = new Set<string>();
    const relay = createWatchNotificationRelay(sink.out, (id) => live.has(id));

    // Exactly fill the queue: one pre-stall write plus MAX queued entries.
    sink.stall();
    for (let i = 0; i <= MAX_PENDING_WATCH_EVENTS; i += 1) {
        const id = `sub-${i}`;
        live.add(id);
        relay.deliver(relayEvent(id, `/w/${i}.txt`, "r1"));
    }
    assert.equal(relay.pendingCount(), MAX_PENDING_WATCH_EVENTS);

    // Nothing has been dropped yet, so draining here must produce watch events
    // and NO loss report: a spurious "you lost changes" forces the client into
    // pointless re-reads and erodes the signal's meaning.
    sink.drain();
    assert.equal(
        parseLines(sink.lines).filter(
            (m) => m.method === RPC_WATCH_EVENTS_LOST_NOTIFICATION,
        ).length,
        0,
    );

    // Now overflow it for real: refill to the bound, then push one more event
    // for a subscription that has no queued entry to coalesce into.
    sink.lines.length = 0;
    sink.stall();
    for (let i = 0; i <= MAX_PENDING_WATCH_EVENTS; i += 1) {
        relay.deliver(relayEvent(`sub-${i}`, `/w/${i}.txt`, "r2"));
    }
    live.add("sub-overflow");
    relay.deliver(relayEvent("sub-overflow", "/w/overflow.txt", "r2"));
    assert.equal(relay.pendingCount(), MAX_PENDING_WATCH_EVENTS);

    sink.drain();
    const lost = parseLines(sink.lines).filter(
        (m) => m.method === RPC_WATCH_EVENTS_LOST_NOTIFICATION,
    );
    assert.equal(lost.length, 1);
    assert.deepEqual(lost[0]?.params.subscriptionIds, ["sub-overflow"]);

    // And the report is not repeated once delivered.
    sink.lines.length = 0;
    sink.stall();
    relay.deliver(relayEvent("sub-overflow", "/w/overflow.txt", "r3"));
    sink.drain();
    assert.equal(
        parseLines(sink.lines).filter(
            (m) => m.method === RPC_WATCH_EVENTS_LOST_NOTIFICATION,
        ).length,
        0,
    );
});

test("a cancelled subscription's queued events and loss record are discarded", () => {
    const sink = fakeOut();
    const live = new Set(["sub-keep", "sub-drop"]);
    const relay = createWatchNotificationRelay(sink.out, (id) => live.has(id));

    // The first delivery is the one that observes the stall, so stall first and
    // spend it on a subscription neither assertion below depends on.
    sink.stall();
    relay.deliver(relayEvent("sub-keep", "/w/keep.txt", "r0"));
    relay.deliver(relayEvent("sub-drop", "/w/drop.txt", "r1"));
    relay.deliver(relayEvent("sub-keep", "/w/keep.txt", "r1"));
    assert.equal(relay.pendingCount(), 2);

    // The client unwatches while its events are still queued behind the stall.
    live.delete("sub-drop");
    relay.forget("sub-drop");
    assert.equal(relay.pendingCount(), 1);

    sink.drain();
    const delivered = parseLines(sink.lines).map(
        (m) => (m.params as unknown as WatchEvent).subscriptionId,
    );
    assert.deepEqual(delivered, ["sub-keep", "sub-keep"]);
});

test("the relay queues nothing for a subscription that no longer exists", () => {
    const sink = fakeOut();
    const live = new Set(["sub-live"]);
    const relay = createWatchNotificationRelay(sink.out, (id) => live.has(id));

    sink.stall();
    relay.deliver(relayEvent("sub-live", "/w/live.txt", "r0"));
    // An event still in flight inside the service when the client unwatched.
    relay.deliver(relayEvent("sub-gone", "/w/gone.txt", "r1"));
    assert.equal(relay.pendingCount(), 0);

    sink.drain();
    const delivered = parseLines(sink.lines).map(
        (m) => (m.params as unknown as WatchEvent).subscriptionId,
    );
    assert.deepEqual(delivered, ["sub-live"]);
});

test("unwatch announces the released subscription so no queue outlives it", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "announced.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    const closed: string[] = [];
    service.onWatchClosed((id) => closed.push(id));

    const { subscriptionId } = await service.watch({ path: file });
    assert.equal(service.hasSubscription(subscriptionId), true);

    service.unwatch({ subscriptionId });
    assert.equal(service.hasSubscription(subscriptionId), false);
    assert.deepEqual(closed, [subscriptionId]);

    // A second unwatch has nothing to release and must stay silent, so the
    // relay is never asked to forget a subscription id twice.
    service.unwatch({ subscriptionId });
    assert.deepEqual(closed, [subscriptionId]);

    // closeAll is the client-disconnect path: it announces every live handle.
    const second = await service.watch({ path: file });
    service.closeAll();
    assert.deepEqual(closed, [subscriptionId, second.subscriptionId]);

    await fs.rm(dir, { recursive: true, force: true });
});

// A dangling symlink reads as "nothing at this path" to the link-following
// status check, so a create-only save took the create branch and renamed a
// fresh regular file OVER the link — destroying a link the user made, with no
// error and no way back. A plain `open(link, O_CREAT)` creates the link's
// TARGET, and that is what the save must do too.
test("writeFile through a dangling symlink creates the target and keeps the link", async () => {
    const dir = await tmpDir();
    const target = path.join(dir, "target.txt");
    const link = path.join(dir, "link.txt");
    await fs.symlink(target, link); // dangling: target does not exist yet

    const created = await writeFile({ path: link, content: "through", expectedRevision: "" });
    assert.equal(created.path, link);

    // The link survived, still pointing where the user pointed it...
    const linkStats = await fs.lstat(link);
    assert.equal(linkStats.isSymbolicLink(), true, "the symlink was replaced by a regular file");
    assert.equal(await fs.readlink(link), target);
    // ...and the bytes landed in the file it names.
    assert.equal(await fs.readFile(target, "utf-8"), "through");
    // Nothing else was created, in particular no leftover temp file.
    assert.deepEqual((await fs.readdir(dir)).sort(), ["link.txt", "target.txt"]);

    // The revision the save returned is the one a stat through the link reports,
    // so the very next save guards on the right token instead of conflicting.
    const seen = await stat({ path: link });
    assert.equal(seen.kind, "symlink");
    assert.equal(seen.revision, created.revision);

    // And the follow-up overwrite still writes through the link.
    await writeFile({ path: link, content: "again", expectedRevision: created.revision });
    assert.equal((await fs.lstat(link)).isSymbolicLink(), true);
    assert.equal(await fs.readFile(target, "utf-8"), "again");

    await fs.rm(dir, { recursive: true, force: true });
});

// stat's `revision` and `writable` describe the node readFile and writeFile act
// on, and through a DANGLING link that node is a file which does not exist yet.
// Reporting the LINK's own revision made the next save an unresolvable
// conflict — the write guard compares the token against the absent target and
// always loses — and writable:false told the editor a save was impossible when
// the save in fact succeeds by creating the link's target.
test("stat reports a dangling symlink as creatable, not as a phantom conflict", async () => {
    const dir = await tmpDir();
    const target = path.join(dir, "absent.txt");
    const link = path.join(dir, "dangling.lnk");
    await fs.symlink(target, link);

    const seen = await stat({ path: link });
    assert.equal(seen.kind, "symlink");
    // "" is the create-only token writeFile's revision guard expects.
    assert.equal(seen.revision, "");
    // The directory that will hold the created target is writable, so the save
    // is possible and stat has to say so.
    assert.equal(seen.writable, true);

    // And that token is exactly the one the save accepts.
    const created = await writeFile({
        path: link,
        content: "made",
        expectedRevision: seen.revision,
    });
    assert.notEqual(created.revision, "");
    assert.equal(await fs.readFile(target, "utf-8"), "made");
    assert.equal((await fs.lstat(link)).isSymbolicLink(), true);

    // A link whose chain cycles has no target either, so the same token — but
    // nothing can write through it, and writable must stay false.
    const a = path.join(dir, "a");
    const b = path.join(dir, "b");
    await fs.symlink(b, a);
    await fs.symlink(a, b);
    const cycle = await withTimeout(stat({ path: a }), 5000);
    assert.equal(cycle.revision, "");
    assert.equal(cycle.writable, false);

    await fs.rm(dir, { recursive: true, force: true });
});

// Following the chain by hand is what makes the test above possible (realpath
// refuses a dangling chain), and a hand-written follow must not spin on a cycle
// inside an RPC handler. ELOOP is the errno the kernel reports for this.
test("writeFile refuses a symbolic-link cycle instead of looping forever", async () => {
    const dir = await tmpDir();
    const a = path.join(dir, "a");
    const b = path.join(dir, "b");
    await fs.symlink(b, a);
    await fs.symlink(a, b);

    await withTimeout(
        assert.rejects(
            () => writeFile({ path: a, content: "x", expectedRevision: "" }),
            (err: NodeJS.ErrnoException) => err.code === "ELOOP",
        ),
        5000,
    );

    await fs.rm(dir, { recursive: true, force: true });
});

test("writeFile follows the maximum supported symbolic-link depth", async () => {
    const dir = await tmpDir();
    const target = path.join(dir, "target.txt");
    let link = target;
    for (let index = 40; index >= 1; index -= 1) {
        const next = path.join(dir, `link-${index}.txt`);
        await fs.symlink(path.basename(link), next);
        link = next;
    }

    await writeFile({ path: link, content: "deep", expectedRevision: "" });
    assert.equal(await fs.readFile(target, "utf-8"), "deep");

    await fs.rm(dir, { recursive: true, force: true });
});

// A listing longer than one transport frame did not merely fail: MAX_LINE_BYTES
// is enforced by BOTH ends, so the client's bounded reader dropped the SSH
// channel and took every terminal, editor and watch subscription in the session
// with it — over a directory the user could not even identify from the failure.
test("a listing too big for one transport frame is refused, not put on the wire", () => {
    // The bound must leave room for the JSON-RPC envelope around the entries.
    assert.ok(
        MAX_DIRECTORY_LISTING_BYTES < MAX_LINE_BYTES,
        `listing cap ${MAX_DIRECTORY_LISTING_BYTES} must stay under the ${MAX_LINE_BYTES}-byte frame cap`,
    );

    // Just under the cap passes: 40-byte names, sized so the total lands short.
    const name = "n".repeat(40);
    const perEntry = 40 + 2 + 29; // the name, its quotes, the entry envelope
    const fits = Math.floor(MAX_DIRECTORY_LISTING_BYTES / perEntry);
    assertListingFits("/big", Array.from({ length: fits }, () => ({ name })));

    // A few entries more does not, and the refusal names the directory, the real
    // entry count and the limit — everything the user needs to act.
    const over = Array.from({ length: fits + 16 }, () => ({ name }));
    assert.throws(
        () => assertListingFits("/big", over),
        (err: Error & { code?: number }) => {
            assert.equal(err.code, RPC_RESOURCE_LIMIT);
            assert.match(err.message, /\/big/);
            assert.match(err.message, new RegExp(`${over.length} entries`));
            return true;
        },
    );

    // The measurement is of the SERIALIZED name, not its raw byte length: a
    // control character costs the six characters \u0001 once escaped. The exact
    // same COUNT of 40-byte names that fitted above is therefore refused when
    // those bytes are control characters — which a byte-length estimate, off by
    // 6x here, would have waved through onto the wire.
    const escaped = Array.from({ length: fits }, () => ({ name: "\u0001".repeat(40) }));
    assert.throws(
        () => assertListingFits("/esc", escaped),
        (err: Error & { code?: number }) => err.code === RPC_RESOURCE_LIMIT,
    );
});

// The two watch bounds are documented as EQUAL, and files.ts says a parity test
// pins that — it did not, so nothing stopped one of them drifting.
//
// Why they must match: the relay coalesces its queue per (subscription, path)
// and a subscription watches exactly one path, so the queue can never hold more
// entries than there are live subscriptions. Equal bounds make the relay's
// COUNT limit provably unreachable, leaving its byte limit as the one that can
// actually fire. Raising the subscription cap alone would start the relay
// dropping watch events — and reporting them as lost — for no reason a user
// could act on.
test("the watch-subscription cap and the relay's queue bound stay equal", () => {
    assert.equal(MAX_WATCH_SUBSCRIPTIONS, MAX_PENDING_WATCH_EVENTS);
});

// Each subscription holds an OS watch handle plus a poll timer, and the table
// holding them was unbounded: a client that leaks unwatch calls walks it up
// until every watch on the host starts failing.
test("watch refuses a new subscription past the live-subscription cap", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "capped.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 1_000_000; // no polling wanted here, just the handles

    const ids: string[] = [];
    for (let i = 0; i < MAX_WATCH_SUBSCRIPTIONS; i += 1) {
        ids.push((await service.watch({ path: file })).subscriptionId);
    }
    await assert.rejects(
        () => service.watch({ path: file }),
        (err: Error & { code?: number }) => {
            assert.equal(err.code, RPC_RESOURCE_LIMIT);
            assert.match(err.message, new RegExp(`${MAX_WATCH_SUBSCRIPTIONS} active file watches`));
            return true;
        },
    );

    // The cap counts LIVE subscriptions, not lifetime ones: releasing one makes
    // room for the next, so an editor that closes a pane can open another.
    service.unwatch({ subscriptionId: ids[0] });
    const replacement = await service.watch({ path: file });
    assert.equal(service.hasSubscription(replacement.subscriptionId), true);

    service.closeAll();
    await fs.rm(dir, { recursive: true, force: true });
});

// codeharbord dispatches request lines concurrently, so the cap has to hold
// against a burst that is all in flight at once — the table insertion sits after
// two awaits, and a size check that is not also a reservation lets every caller
// in the burst pass it.
test("a concurrent burst of watch calls cannot overshoot the cap", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "burst.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 1_000_000;

    const attempts = MAX_WATCH_SUBSCRIPTIONS + 64;
    const settled = await Promise.allSettled(
        Array.from({ length: attempts }, () => service.watch({ path: file })),
    );
    const granted = settled.filter((r) => r.status === "fulfilled");
    assert.equal(granted.length, MAX_WATCH_SUBSCRIPTIONS);
    for (const rejection of settled.filter((r) => r.status === "rejected")) {
        const reason: unknown = rejection.reason;
        assert.ok(reason instanceof Error && "code" in reason);
        assert.equal(reason.code, RPC_RESOURCE_LIMIT);
    }

    service.closeAll();
    await fs.rm(dir, { recursive: true, force: true });
});

// The wire half of the two bounds above: a resource limit must reach the client
// as its own code with the server's message intact, not as -32603 "internal
// error", which tells the user the server broke.
test("file.watch past the cap answers with the resource-limit code, not internal error", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "rpc-cap.txt");
    await fs.writeFile(file, "one");

    // The dispatcher uses the module singleton, so every id taken here is given
    // back before the test ends.
    const previousInterval = fileWatchService.pollIntervalMs;
    fileWatchService.pollIntervalMs = 1_000_000;
    const taken: string[] = [];
    try {
        while (taken.length < MAX_WATCH_SUBSCRIPTIONS) {
            taken.push((await fileWatchService.watch({ path: file })).subscriptionId);
        }
        const response = await dispatch({
            jsonrpc: "2.0",
            id: 1,
            method: "file.watch",
            params: { path: file },
        });
        assert.ok(response && "error" in response);
        assert.equal(response.error.code, RPC_RESOURCE_LIMIT);
        assert.match(response.error.message, /active file watches/);
    } finally {
        for (const subscriptionId of taken) fileWatchService.unwatch({ subscriptionId });
        fileWatchService.pollIntervalMs = previousInterval;
    }

    await fs.rm(dir, { recursive: true, force: true });
});

// The daemon had no signal handlers at all: its only orderly shutdown was stdin
// closing, so an ordinary `kill` (or the SIGHUP an ending SSH session delivers)
// hit Node's default disposition and killed it where it stood, abandoning every
// watch handle and reporting "died on a signal" to whatever supervises it.
// Driven through a real child process, because installing a process-wide signal
// handler is exactly the behaviour under test.
async function runDaemonUntilSignalled(
    signals: readonly NodeJS.Signals[],
    keepSignallingUntilExit = false,
): Promise<{ code: number | null; signal: NodeJS.Signals | null }> {
    // A throwaway workspace database. The daemon opens one lazily, so `ping`
    // alone never creates it — but nothing in this suite may be one code change
    // away from writing into the developer's real ~/.local/share workspace.
    const dbDir = await tmpDir();
    const child = spawn(process.execPath, [DAEMON_ENTRY, "rpc", "--stdio"], {
        stdio: ["pipe", "pipe", "pipe"],
        env: { ...process.env, CODEHARBOR_DB: path.join(dbDir, "codeharbor.sqlite") },
    });
    const exited = new Promise<{ code: number | null; signal: NodeJS.Signals | null }>((resolve) => {
        child.on("exit", (code, signal) => resolve({ code, signal }));
    });
    try {
        // Serve one request first, so the signal lands on a daemon that is
        // genuinely up rather than on a process still importing its modules.
        const answered = new Promise<string>((resolve) => {
            let buffered = "";
            child.stdout.on("data", (chunk: Buffer) => {
                buffered += chunk.toString("utf-8");
                const newline = buffered.indexOf("\n");
                if (newline >= 0) resolve(buffered.slice(0, newline));
            });
        });
        child.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id: 7, method: "ping" })}\n`);
        assert.match(await withTimeout(answered, 20000), /"id":7/);

        for (const signal of signals) child.kill(signal);
        if (keepSignallingUntilExit) {
            // Keep re-sending SIGTERM every few milliseconds right up to the
            // moment the process is gone, so one of them is guaranteed to land
            // in the narrow window while the process is finishing its exit. A
            // single late signal only hits that window by luck. Real delays,
            // deliberately: the thing under test is signal delivery to a
            // separate operating-system process, and a fake clock in this
            // process cannot move that process's timeline at all.
            while (child.exitCode === null && child.signalCode === null) {
                child.kill("SIGTERM");
                await delay(3);
            }
        }
        return await withTimeout(exited, 20000);
    } finally {
        if (child.exitCode === null && child.signalCode === null) child.kill("SIGKILL");
        await fs.rm(dbDir, { recursive: true, force: true });
    }
}

test("SIGTERM shuts the daemon down cleanly instead of killing it", async () => {
    const { code, signal } = await runDaemonUntilSignalled(["SIGTERM"]);
    assert.equal(signal, null, "the daemon died on the signal instead of shutting down");
    assert.equal(code, 0);
});

test("SIGHUP — what an ending SSH session sends — also shuts the daemon down cleanly", async () => {
    const { code, signal } = await runDaemonUntilSignalled(["SIGHUP"]);
    assert.equal(signal, null);
    assert.equal(code, 0);
});

// A supervisor that does not see the process go immediately sends another
// SIGTERM. The handler must stay installed and simply ignore it: deregistering
// on the first signal restores the default disposition, and the second signal
// would then kill the process mid-shutdown — reintroducing exactly the
// signal-death this fix removes.
test("a second signal during shutdown does not turn a clean exit into a kill", async () => {
    const { code, signal } = await runDaemonUntilSignalled(["SIGTERM", "SIGTERM", "SIGINT"]);
    assert.equal(signal, null);
    assert.equal(code, 0);
});

// The same second signal, but delivered while the daemon is actually leaving,
// rather than in the same instant as the first. The tight loop above only
// reproduces that by accident, when the machine happens to be loaded enough to
// spread the three calls out. By the time a late signal arrives the first
// shutdown has already finished its work and the process is on its way out, so
// this is the case that catches Node closing its internal signal watchers
// before the process is really gone and letting the late signal kill it after
// all. Removing the fix in the daemon makes this test fail every time.
test("signals that keep arriving while the daemon exits still do not kill it", async () => {
    const { code, signal } = await runDaemonUntilSignalled(["SIGTERM"], true);
    assert.equal(signal, null, "a late signal killed the daemon during its exit");
    assert.equal(code, 0);
});

// A kernel-generated file reports size 0 in stat while holding real content.
// Sizing the buffer from stat therefore returned an empty string with
// `truncated: false` — the viewer presented /proc/version as a document that
// genuinely has nothing in it, which is indistinguishable from a real empty
// file and gives the user no hint that anything went wrong.
test(
    "readFile returns the contents of a file whose reported size is 0",
    { skip: process.platform === "linux" ? false : "needs a /proc filesystem" },
    async () => {
        assert.equal((await fs.stat("/proc/version")).size, 0, "precondition: stat reports 0");

        const r = await readFile({ path: "/proc/version" });
        assert.equal(r.encoding, "utf-8");
        assert.match(r.content, /Linux version/);
        // The read went to end of file, so it IS the whole file. Reporting it as
        // partial would make the viewer refuse to render it.
        assert.equal(r.truncated, false);

        // A window still applies, and now against the bytes that are really
        // there rather than against the size of zero.
        const head = await readFile({ path: "/proc/version", offset: 0, length: 5 });
        assert.equal(head.content, r.content.slice(0, 5));
        assert.equal(head.truncated, true);
    },
);

// An atomic save replaces a file by renaming over its name, which needs write
// permission on the DIRECTORY and none at all on the file. So a file the user
// deliberately marked read-only — one stat() had just reported as
// writable:false — was overwritten anyway, where a plain open for writing, and
// every ordinary editor, refuses with EACCES.
test(
    "writeFile refuses a read-only file instead of renaming over it",
    { skip: process.getuid?.() === 0 ? "root ignores file permissions" : false },
    async () => {
        const dir = await tmpDir();
        const file = path.join(dir, "readonly.txt");
        await fs.writeFile(file, "protected");
        await fs.chmod(file, 0o444);

        const current = await stat({ path: file });
        assert.equal(current.writable, false);

        await assert.rejects(
            () => writeFile({ path: file, content: "clobber", expectedRevision: current.revision }),
            (err: NodeJS.ErrnoException) => err.code === "EACCES",
        );
        assert.equal(await fs.readFile(file, "utf-8"), "protected");
        // The refusal happens before any temp file is created, so nothing is
        // left behind for the user to clean up.
        assert.deepEqual(await fs.readdir(dir), ["readonly.txt"]);

        await fs.chmod(file, 0o644);
        await fs.rm(dir, { recursive: true, force: true });
    },
);

// Resolve once an event satisfying `predicate` has been delivered, checking the
// events already collected first so one that arrived before this call is not
// missed.
//
// Waits for a CONDITION, never for a duration or for an event COUNT. One
// `writeFile` is a truncate followed by a write, and Linux is free to report
// that as two changes or, if the two land inside one check, as one — so any
// assertion on "how many events arrived" is a coin flip. What is stable is the
// state the file ends in.
function untilWatched(
    service: FileWatchService,
    collected: readonly WatchEvent[],
    predicate: (event: WatchEvent) => boolean,
): Promise<void> {
    return new Promise<void>((resolve) => {
        if (collected.some(predicate)) return resolve();
        const off = service.onWatchEvent((event) => {
            if (predicate(event)) {
                off();
                resolve();
            }
        });
    });
}

// Events are handed out through an EventEmitter, which runs its listeners in
// order and lets an exception escape: one broken subscriber silenced every
// later one, turned a perfectly good unwatch into a failed RPC call, and — the
// worst of the three — aborted closeAll() halfway through, leaving the rest of
// a dead session's watch handles installed on the operating system.
test("a throwing watch subscriber cannot silence the others or break teardown", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "throwing.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    const seen: WatchEvent[] = [];
    const closed: string[] = [];
    // Counted rather than asserted-on directly: the contract is that the good
    // subscriber is invoked for EVERY delivery the broken one was, whatever
    // number the kernel happens to produce. If isolation regressed, the broken
    // subscriber's throw would stop the emit and `seen` would fall behind.
    let thrown = 0;
    service.onWatchEvent(() => {
        thrown += 1;
        throw new Error("this subscriber is broken");
    });
    service.onWatchEvent((event) => seen.push(event));
    service.onWatchClosed(() => {
        throw new Error("this closed-subscriber is broken");
    });
    service.onWatchClosed((id) => closed.push(id));

    // Every path out of this block releases the subscriptions. A failed
    // assertion used to skip the teardown below, and an unreleased fs.watch
    // handle keeps Node's event loop alive: the runner reported the failure and
    // then hung until something killed it, which buries the failure it just
    // found under a timeout.
    try {
        const first = await service.watch({ path: file });
        await fs.writeFile(file, "two — a clearly different length");
        const finalRevision = revisionFrom(await fs.stat(file));
        await withTimeout(
            untilWatched(service, seen, (event) => event.revision === finalRevision),
            5000,
        );
        assert.ok(thrown >= 1, "the broken subscriber was never invoked");
        assert.equal(seen.length, thrown, "a throw skipped the subscriber behind it");
        assert.equal(seen.at(-1)?.event, "modified");

        // unwatch emits "closed" synchronously, straight out of the RPC handler.
        service.unwatch({ subscriptionId: first.subscriptionId });
        assert.deepEqual(closed, [first.subscriptionId]);

        // closeAll is the channel-drop path and must release EVERY handle even
        // though the first listener throws on each one.
        const second = await service.watch({ path: file });
        const third = await service.watch({ path: file });
        service.closeAll();
        assert.deepEqual(closed, [
            first.subscriptionId,
            second.subscriptionId,
            third.subscriptionId,
        ]);
        assert.equal(service.hasSubscription(second.subscriptionId), false);
        assert.equal(service.hasSubscription(third.subscriptionId), false);
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});

// Subscribers are wrapped before registration (so one that throws cannot take
// the others down), which is exactly the kind of change that quietly breaks
// removal: `off(callback)` does not match a wrapper. A subscriber that cannot
// be removed keeps its whole closure alive for the life of the process.
test("the disposer returned by onWatchEvent really removes the subscriber", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "detached.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    const afterOff: WatchEvent[] = [];
    const off = service.onWatchEvent((event) => afterOff.push(event));
    off();

    const seen: WatchEvent[] = [];
    service.onWatchEvent((event) => seen.push(event));

    // Same rule as the test above: an unreleased fs.watch handle keeps Node's
    // event loop alive, so a failed assertion here would hang the whole runner
    // instead of just failing.
    try {
        const { subscriptionId } = await service.watch({ path: file });
        await fs.writeFile(file, "two — a clearly different length");
        const finalRevision = revisionFrom(await fs.stat(file));
        await withTimeout(
            untilWatched(service, seen, (event) => event.revision === finalRevision),
            5000,
        );
        service.unwatch({ subscriptionId });
        // The removed subscriber saw nothing, while the one still registered did.
        assert.ok(seen.length >= 1);
        assert.deepEqual(afterOff, []);
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});

// The same wrapper rule applies to the "subscription released" signal, which is
// registered once per connection by the notification relay: a disposer that
// fails to match its wrapper keeps a dead connection's callback — and the
// closure behind it — alive for the life of the daemon.
test("the disposer returned by onWatchClosed really removes the subscriber", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "released.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 1_000_000; // nothing here waits for a poll tick
    const afterOff: string[] = [];
    const off = service.onWatchClosed((id) => afterOff.push(id));
    const kept: string[] = [];
    service.onWatchClosed((id) => kept.push(id));
    off();

    try {
        const { subscriptionId } = await service.watch({ path: file });
        service.unwatch({ subscriptionId });
        // The signal is emitted synchronously by unwatch, so no waiting.
        assert.deepEqual(kept, [subscriptionId]);
        assert.deepEqual(afterOff, []);
    } finally {
        service.closeAll();
        await fs.rm(dir, { recursive: true, force: true });
    }
});

// A directory holds more than files and folders. None of these may make the
// listing fail as a whole: a user who cannot list a directory because ONE entry
// in it is unusual has lost access to everything else in it.
test("listDirectory classifies special files and a dangling symlink", async () => {
    const dir = await tmpDir();
    await fs.writeFile(path.join(dir, "plain.txt"), "x");
    // A link to a name that does not exist. Its target cannot be stat'd, so any
    // classification that resolves the link would fail on it.
    await fs.symlink(path.join(dir, "nowhere.txt"), path.join(dir, "broken.lnk"));
    await makeFifo(path.join(dir, "pipe"));

    const listing = await listDirectory({ path: dir });
    assert.deepEqual(
        listing.entries.map((entry) => entry.name),
        ["broken.lnk", "pipe", "plain.txt"],
    );
    const kinds: Record<string, string> = {};
    for (const entry of listing.entries) kinds[entry.name] = entry.kind;
    assert.equal(kinds["broken.lnk"], "symlink");
    assert.equal(kinds["pipe"], "other");
    assert.equal(kinds["plain.txt"], "file");

    await fs.rm(dir, { recursive: true, force: true });
});

// A filesystem path is a NUL-terminated byte string, so no name can hold a NUL.
// Node enforces that deep inside fs with a TypeError, which the dispatcher can
// only report as -32603 "internal error" — telling the user the SERVER broke
// when the REQUEST was malformed. resolvePath never touches the filesystem at
// all, so it answered with an ordinary-looking result that failed later.
test("file methods reject a path containing a NUL character", async () => {
    const bad = "/tmp/a\u0000b";
    for (const method of [
        "file.stat",
        "file.readFile",
        "file.writeFile",
        "file.resolvePath",
        "file.watch",
        "file.listDirectory",
    ]) {
        const params =
            method === "file.writeFile"
                ? { path: bad, content: "x", expectedRevision: "" }
                : { path: bad };
        const error = await callFile(method, params);
        assert.equal(error.code, RPC_INVALID_PARAMS, method);
        assert.match(error.message, /field 'path' must not contain a NUL character/);
    }

    // The repository root a path is resolved against is a path too.
    const base = await callFile("file.resolvePath", { path: "x", base: "/repo\u0000" });
    assert.equal(base.code, RPC_INVALID_PARAMS);
    assert.match(base.message, /field 'base' must not contain a NUL character/);
});