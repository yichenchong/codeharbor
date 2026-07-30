import { test } from "node:test";
import assert from "node:assert/strict";
import { promises as fs } from "node:fs";
import os from "node:os";
import path from "node:path";

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
import { RPC_REVISION_MISMATCH } from "../src/rpc-types.ts";
import { dispatch } from "../src/codeharbord.ts";
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
