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

    const past = await readFile({ path: text, offset: 1000 });
    assert.equal(past.content, "");
    assert.equal(past.truncated, false);
    assert.equal(past.encoding, "utf-8");

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

test("internal helpers: listDirectory and getMimeType classify entries", async () => {
    const dir = await tmpDir();
    await fs.writeFile(path.join(dir, "readme.md"), "# hi");
    await fs.mkdir(path.join(dir, "sub"));

    const entries = await listDirectory(dir);
    const byName: Record<string, string> = {};
    for (const entry of entries) byName[entry.name] = entry.kind;
    assert.equal(byName["readme.md"], "file");
    assert.equal(byName["sub"], "directory");

    assert.equal(getMimeType("a/b/readme.md"), "text/markdown");
    assert.equal(getMimeType("photo.PNG"), "image/png");
    assert.equal(getMimeType("mystery.xyz"), "application/octet-stream");

    // revisionFrom is deterministic from mtime + size.
    assert.equal(revisionFrom({ mtimeMs: 12.5, size: 7 }), "12.5-7");

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
