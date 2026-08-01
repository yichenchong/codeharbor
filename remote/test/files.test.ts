import { test } from "node:test";
import assert from "node:assert/strict";
import { promises as fs } from "node:fs";
import os from "node:os";
import path from "node:path";
import { EventEmitter } from "node:events";

import {
    stat,
    readFile,
    writeFile,
    resolvePath,
    listDirectory,
    getMimeType,
    revisionFrom,
    isRevisionMismatch,
    FileWatchService,
} from "../src/files.ts";
import {
    RPC_REVISION_MISMATCH,
    RPC_WATCH_EVENT_NOTIFICATION,
    RPC_WATCH_EVENTS_LOST_NOTIFICATION,
} from "../src/rpc-types.ts";
import {
    createWatchNotificationRelay,
    dispatch,
    MAX_PENDING_WATCH_EVENTS,
    RPC_INVALID_PARAMS,
} from "../src/codeharbord.ts";
import type { WatchEvent } from "../src/rpc-types.ts";

async function tmpDir(): Promise<string> {
    return fs.mkdtemp(path.join(os.tmpdir(), "codeharbord-files-"));
}

// Await the actual event the service exposes rather than sleeping a guessed
// duration. `withTimeout` uses a wall-clock guard because real filesystem
// events (fs.watch / polling against the OS clock) cannot be driven by fake
// timers; the guard converts a regression into a failure instead of a hang.
// The executor form (not Promise.withResolvers) is required: tsconfig targets
// ES2022, whose lib predates the withResolvers typing.
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

test("resolvePath flags inside vs outside the repository root", async () => {
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

    await Promise.resolve();
});

test("watch emits a WatchEvent when the file changes", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "watched.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 25; // polling fallback covers platforms without fs.watch
    const eventPromise = firstWatchEvent(service);

    const { subscriptionId } = await service.watch({ path: file });
    // A different length guarantees the revision (mtime-size) changes.
    await fs.writeFile(file, "two — different length");

    const event = await withTimeout(eventPromise, 5000);
    service.unwatch({ subscriptionId });
    service.closeAll();

    assert.equal(event.subscriptionId, subscriptionId);
    assert.equal(event.path, file);
    assert.equal(event.event, "modified");
    assert.equal(typeof event.revision, "string");

    await fs.rm(dir, { recursive: true, force: true });
});

test("listDirectory (RPC) classifies entries; getMimeType maps extensions", async () => {
    const dir = await tmpDir();
    await fs.writeFile(path.join(dir, "readme.md"), "# hi");
    await fs.mkdir(path.join(dir, "sub"));

    const result = await listDirectory({ path: dir });
    assert.equal(result.path, dir);
    const byName: Record<string, string> = {};
    for (const entry of result.entries) byName[entry.name] = entry.kind;
    assert.equal(byName["readme.md"], "file");
    assert.equal(byName["sub"], "directory");

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

    const { subscriptionId } = await service.watch({ path: file });
    await fs.rm(file);

    const event = await withTimeout(eventPromise, 5000);
    service.unwatch({ subscriptionId });
    service.closeAll();

    assert.equal(event.event, "deleted");
    assert.equal(event.revision, undefined);

    await fs.rm(dir, { recursive: true, force: true });
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

test("watch emits no WatchEvent for a subscription after unwatch", async () => {
    const dir = await tmpDir();
    const file = path.join(dir, "unwatched.txt");
    await fs.writeFile(file, "one");

    const service = new FileWatchService();
    service.pollIntervalMs = 25;
    const seen: WatchEvent[] = [];
    service.onWatchEvent((event) => seen.push(event));

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
    service.closeAll();

    assert.equal(event.subscriptionId, second.subscriptionId);
    // The released subscription never emitted for anyone.
    assert.ok(seen.every((e) => e.subscriptionId !== first.subscriptionId));

    await fs.rm(dir, { recursive: true, force: true });
});

test("resolvePath treats an in-repo name starting with '..' as inside", async () => {
    const base = path.resolve("/repo/project");

    // "..config" is a real in-repo filename, NOT a parent-directory escape.
    const dotName = resolvePath({ path: "..config", base });
    assert.equal(dotName.insideRepositoryRoot, true);
    assert.equal(dotName.path, path.join(base, "..config"));

    // A genuine parent escape ("..") is still flagged outside.
    assert.equal(resolvePath({ path: "..", base }).insideRepositoryRoot, false);
    assert.equal(resolvePath({ path: "../x", base }).insideRepositoryRoot, false);

    await Promise.resolve();
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

    // A payload cut mid-group has a stripped length of 1 mod 4, which encodes no
    // byte string: Buffer would discard the partial group and write short data.
    const truncated = path.join(dir, "truncated.bin");
    await assert.rejects(
        () => writeFile({ path: truncated, content: "QUJDR", encoding: "base64", expectedRevision: "" }),
        /Invalid base64 content/,
    );
    assert.equal(await fs.access(truncated).then(() => true, () => false), false);

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

    const { subscriptionId } = await service.watch({ path: file });
    await fs.writeFile(file, "now I exist");

    const event = await withTimeout(eventPromise, 5000);
    service.closeAll();

    assert.equal(event.subscriptionId, subscriptionId);
    assert.equal(event.event, "created");
    assert.equal(typeof event.revision, "string");

    await fs.rm(dir, { recursive: true, force: true });
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
    service.closeAll();

    assert.equal(events.at(-1)?.revision, finalRevision);

    await fs.rm(dir, { recursive: true, force: true });
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