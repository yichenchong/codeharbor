import { test } from "node:test";
import assert from "node:assert/strict";
import { promises as fs } from "node:fs";
import os from "node:os";
import path from "node:path";

import { openWorkspace, WORKSPACE_SCHEMA_VERSION, WORKSPACE_METHODS } from "../src/workspace.ts";
import type { Workspace } from "../src/workspace.ts";

// A fresh temp-file database path (never :memory:) so the reopen tests exercise
// real on-disk persistence, simulating a codeharbord restart (SPEC 11.1).
async function tmpDbPath(): Promise<string> {
    const dir = await fs.mkdtemp(path.join(os.tmpdir(), "codeharbord-ws-"));
    return path.join(dir, "codeharbor.sqlite");
}

async function cleanup(dbPath: string): Promise<void> {
    await fs.rm(path.dirname(dbPath), { recursive: true, force: true });
}

const SERVER = "srv-1";

// Populate one group + one session with a viewer pane, a terminal pane, and a
// split layout per region. Returns the created ids so callers can assert.
function seed(ws: Workspace) {
    const group = ws.createGroup({ serverId: SERVER, name: "Work" });
    const session = ws.createSession({
        serverId: SERVER,
        groupId: group.id,
        name: "codeharbor",
        repositoryRoot: "/home/dev/codeharbor",
        defaultWorkingDirectory: "/home/dev/codeharbor/src",
        taskDescription: "wave 2 persistence",
    });
    const viewer = ws.createViewerPane({
        serverId: SERVER,
        devSessionId: session.id,
        url: "https://localhost:3000",
        handler: "web",
        title: "Docs",
    });
    const terminal = ws.createTerminalPane({
        serverId: SERVER,
        devSessionId: session.id,
        name: "shell",
        workingDirectory: "/home/dev/codeharbor",
        tmuxTarget: `ch_${session.id}_orig`,
        startupCommand: "git status",
        harness: "oh-my-pi",
    });
    ws.setLayout({
        serverId: SERVER,
        devSessionId: session.id,
        region: "viewer",
        tree: { type: "leaf", paneId: viewer.id },
    });
    ws.setLayout({
        serverId: SERVER,
        devSessionId: session.id,
        region: "terminal",
        tree: { type: "leaf", paneId: terminal.id },
    });
    return { group, session, viewer, terminal };
}

test("list on an empty database returns no groups", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    assert.deepEqual(ws.list("unknown-server"), []);
    ws.close();
    await cleanup(dbPath);
});

test("workspace survives a close+reopen with byte-identical state (SPEC 11.1 stop gate)", async () => {
    const dbPath = await tmpDbPath();

    const first = openWorkspace(dbPath);
    const { session, viewer, terminal } = seed(first);
    const before = JSON.stringify(first.list(SERVER));
    first.close();

    // Reopen the same file, simulating a codeharbord restart.
    const second = openWorkspace(dbPath);
    const after = JSON.stringify(second.list(SERVER));
    assert.equal(after, before, "reloaded state must be byte-identical");

    // Structural spot-checks on the reloaded tree.
    const tree = second.list(SERVER);
    assert.equal(tree.length, 1);
    const [group] = tree;
    assert.equal(group.sessions.length, 1);
    const node = group.sessions[0];
    assert.equal(node.id, session.id);
    assert.equal(node.repositoryRoot, "/home/dev/codeharbor");
    assert.equal(node.viewerPanes.length, 1);
    assert.equal(node.viewerPanes[0].id, viewer.id);
    assert.equal(node.terminalPanes.length, 1);
    assert.equal(node.terminalPanes[0].id, terminal.id);
    assert.deepEqual(node.layouts.viewer, { type: "leaf", paneId: viewer.id });
    assert.deepEqual(node.layouts.terminal, { type: "leaf", paneId: terminal.id });

    second.close();
    await cleanup(dbPath);
});

test("reopening an existing database does not re-migrate or duplicate version rows", async () => {
    const dbPath = await tmpDbPath();
    const first = openWorkspace(dbPath);
    seed(first);
    first.close();

    const second = openWorkspace(dbPath);
    const version = second.db
        .prepare("SELECT version FROM schema_version WHERE id = 1")
        .get() as { version: number };
    assert.equal(version.version, WORKSPACE_SCHEMA_VERSION);
    const count = second.db.prepare("SELECT COUNT(*) AS n FROM schema_version").get() as { n: number };
    assert.equal(count.n, 1);
    second.close();
    await cleanup(dbPath);
});

test("duplicateSession copies defs with fresh ids and fresh tmux targets (SPEC 4.2)", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { session, viewer, terminal } = seed(ws);

    const dup = ws.duplicateSession({ id: session.id });

    // A distinct session that nonetheless copies metadata.
    assert.notEqual(dup.id, session.id);
    assert.equal(dup.name, "codeharbor");
    assert.equal(dup.repositoryRoot, "/home/dev/codeharbor");
    assert.equal(dup.taskDescription, "wave 2 persistence");
    assert.equal(dup.groupId, session.groupId);

    // Viewer pane copied with a fresh id but identical definition.
    assert.equal(dup.viewerPanes.length, 1);
    const dupViewer = dup.viewerPanes[0];
    assert.notEqual(dupViewer.id, viewer.id);
    assert.equal(dupViewer.url, "https://localhost:3000");
    assert.equal(dupViewer.title, "Docs");

    // Terminal pane copied with a fresh id AND a fresh tmux target.
    assert.equal(dup.terminalPanes.length, 1);
    const dupTerm = dup.terminalPanes[0];
    assert.notEqual(dupTerm.id, terminal.id);
    assert.equal(dupTerm.name, "shell");
    assert.equal(dupTerm.workingDirectory, "/home/dev/codeharbor");
    assert.equal(dupTerm.startupCommand, "git status");
    assert.equal(dupTerm.harness, "oh-my-pi");
    assert.equal(dupTerm.tmuxTarget, `ch_${dup.id}_${dupTerm.id}`);
    assert.notEqual(dupTerm.tmuxTarget, terminal.tmuxTarget);

    // Split layouts copied and remapped to the new panes (SPEC 4.5).
    assert.deepEqual(dup.layouts.viewer, { type: "leaf", paneId: dupViewer.id });
    assert.deepEqual(dup.layouts.terminal, { type: "leaf", paneId: dupTerm.id });

    // The original session is untouched; the group now holds both.
    const groups = ws.list(SERVER);
    assert.equal(groups[0].sessions.length, 2);
    const original = groups[0].sessions.find((s) => s.id === session.id);
    assert.notEqual(original, undefined);
    assert.equal(original?.terminalPanes[0].tmuxTarget, terminal.tmuxTarget);

    ws.close();
    await cleanup(dbPath);
});

test("deleteSession cascades to its panes and layouts", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { group, session } = seed(ws);

    ws.deleteSession({ id: session.id });

    assert.equal(ws.list(SERVER)[0].sessions.length, 0);
    const counts = (table: string): number => {
        const row = ws.db
            .prepare(`SELECT COUNT(*) AS n FROM ${table} WHERE dev_session_id = ?`)
            .get(session.id) as { n: number };
        return row.n;
    };
    assert.equal(counts("viewer_panes"), 0);
    assert.equal(counts("terminal_panes"), 0);
    assert.equal(counts("session_layouts"), 0);
    // The group itself remains.
    assert.equal(ws.getGroup(group.id).id, group.id);

    ws.close();
    await cleanup(dbPath);
});

test("deleteGroup cascades to sessions, panes, and layouts", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { group, session } = seed(ws);

    ws.deleteGroup({ id: group.id });

    assert.deepEqual(ws.list(SERVER), []);
    const remaining = ws.db
        .prepare("SELECT COUNT(*) AS n FROM dev_sessions WHERE id = ?")
        .get(session.id) as { n: number };
    assert.equal(remaining.n, 0);

    ws.close();
    await cleanup(dbPath);
});

test("reorderGroups rewrites positions to match the given order", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const a = ws.createGroup({ serverId: SERVER, name: "A" });
    const b = ws.createGroup({ serverId: SERVER, name: "B" });
    const c = ws.createGroup({ serverId: SERVER, name: "C" });
    assert.deepEqual(
        ws.list(SERVER).map((g) => g.name),
        ["A", "B", "C"],
    );

    ws.reorderGroups({ serverId: SERVER, orderedIds: [c.id, a.id, b.id] });

    assert.deepEqual(
        ws.list(SERVER).map((g) => g.name),
        ["C", "A", "B"],
    );

    ws.close();
    await cleanup(dbPath);
});

test("update and move operations mutate persisted rows", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { group, session } = seed(ws);
    const other = ws.createGroup({ serverId: SERVER, name: "Archive" });

    ws.updateSession({ id: session.id, name: "renamed", archived: true });
    ws.moveSessionToGroup({ id: session.id, groupId: other.id });

    const groups = ws.list(SERVER);
    const source = groups.find((g) => g.id === group.id);
    const dest = groups.find((g) => g.id === other.id);
    assert.equal(source?.sessions.length, 0);
    assert.equal(dest?.sessions.length, 1);
    assert.equal(dest?.sessions[0].name, "renamed");
    assert.equal(dest?.sessions[0].archived, true);
    assert.equal(dest?.sessions[0].groupId, other.id);

    ws.close();
    await cleanup(dbPath);
});

test("duplicateSession rolls back completely when a copy step fails (transaction atomicity)", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { group, session } = seed(ws);

    // Corrupt one stored layout tree so duplicateSession's JSON.parse throws
    // AFTER the new session and panes are inserted, forcing a full rollback.
    ws.db
        .prepare("UPDATE session_layouts SET tree = ? WHERE dev_session_id = ? AND region = ?")
        .run("{ not valid json", session.id, "terminal");

    assert.throws(() => ws.duplicateSession({ id: session.id }));

    // Nothing partial survived: exactly the original session and its panes.
    const count = (sql: string, ...args: string[]): number => {
        const row = ws.db.prepare(sql).get(...args) as { n: number };
        return row.n;
    };
    assert.equal(count("SELECT COUNT(*) AS n FROM dev_sessions WHERE group_id = ?", group.id), 1);
    assert.equal(count("SELECT COUNT(*) AS n FROM viewer_panes"), 1);
    assert.equal(count("SELECT COUNT(*) AS n FROM terminal_panes"), 1);

    ws.close();
    await cleanup(dbPath);
});

test("reorderSessions rewrites positions within a group", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const group = ws.createGroup({ serverId: SERVER, name: "G" });
    const mk = (name: string) =>
        ws.createSession({ serverId: SERVER, groupId: group.id, name, repositoryRoot: "/r" });
    const a = mk("A");
    const b = mk("B");
    const c = mk("C");
    assert.deepEqual(
        ws.list(SERVER)[0].sessions.map((s) => s.name),
        ["A", "B", "C"],
    );

    ws.reorderSessions({ groupId: group.id, orderedIds: [c.id, a.id, b.id] });

    assert.deepEqual(
        ws.list(SERVER)[0].sessions.map((s) => s.name),
        ["C", "A", "B"],
    );

    ws.close();
    await cleanup(dbPath);
});

test("duplicateSession remaps a nested split layout to the copied panes (SPEC 4.5)", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const group = ws.createGroup({ serverId: SERVER, name: "G" });
    const session = ws.createSession({
        serverId: SERVER,
        groupId: group.id,
        name: "S",
        repositoryRoot: "/r",
    });
    const v1 = ws.createViewerPane({ serverId: SERVER, devSessionId: session.id, url: "http://a" });
    const v2 = ws.createViewerPane({ serverId: SERVER, devSessionId: session.id, url: "http://b" });
    ws.setLayout({
        serverId: SERVER,
        devSessionId: session.id,
        region: "viewer",
        tree: {
            type: "split",
            orientation: "horizontal",
            ratios: [0.5, 0.5],
            children: [
                { type: "leaf", paneId: v1.id },
                { type: "leaf", paneId: v2.id },
            ],
        },
    });

    const dup = ws.duplicateSession({ id: session.id });
    const [dv1, dv2] = dup.viewerPanes;
    assert.notEqual(dv1.id, v1.id);
    assert.notEqual(dv2.id, v2.id);
    // Both leaves reference the COPIED pane ids in order, never the originals.
    assert.deepEqual(dup.layouts.viewer, {
        type: "split",
        orientation: "horizontal",
        ratios: [0.5, 0.5],
        children: [
            { type: "leaf", paneId: dv1.id },
            { type: "leaf", paneId: dv2.id },
        ],
    });

    ws.close();
    await cleanup(dbPath);
});

test("getLayout returns null for a region with no layout", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const group = ws.createGroup({ serverId: SERVER, name: "G" });
    const session = ws.createSession({
        serverId: SERVER,
        groupId: group.id,
        name: "S",
        repositoryRoot: "/r",
    });
    assert.equal(ws.getLayout({ devSessionId: session.id, region: "viewer" }), null);
    ws.close();
    await cleanup(dbPath);
});

test("updateSession keeps unset fields but clears explicit nulls (undefined vs null)", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { session } = seed(ws);

    // Omitting a nullable field keeps the current value...
    let s = ws.updateSession({ id: session.id, name: "renamed" });
    assert.equal(s.name, "renamed");
    assert.equal(s.defaultWorkingDirectory, "/home/dev/codeharbor/src");
    assert.equal(s.taskDescription, "wave 2 persistence");

    // ...while passing null explicitly clears it.
    s = ws.updateSession({ id: session.id, defaultWorkingDirectory: null, taskDescription: null });
    assert.equal(s.defaultWorkingDirectory, null);
    assert.equal(s.taskDescription, null);

    // archived:false is honored, not mistaken for "unset" (?? must not eat false).
    s = ws.updateSession({ id: session.id, archived: true });
    assert.equal(s.archived, true);
    s = ws.updateSession({ id: session.id, archived: false });
    assert.equal(s.archived, false);

    ws.close();
    await cleanup(dbPath);
});

test("updateViewerPane/updateTerminalPane clear nullable fields on null, keep on omit", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { viewer, terminal } = seed(ws);

    let v = ws.updateViewerPane({ id: viewer.id, url: "https://changed" });
    assert.equal(v.url, "https://changed");
    assert.equal(v.handler, "web"); // omitted -> kept
    assert.equal(v.title, "Docs");
    v = ws.updateViewerPane({ id: viewer.id, handler: null, title: null });
    assert.equal(v.handler, null);
    assert.equal(v.title, null);

    let t = ws.updateTerminalPane({ id: terminal.id, name: "sh2" });
    assert.equal(t.name, "sh2");
    assert.equal(t.workingDirectory, "/home/dev/codeharbor"); // kept
    assert.equal(t.tmuxTarget, terminal.tmuxTarget); // kept
    t = ws.updateTerminalPane({
        id: terminal.id,
        workingDirectory: null,
        tmuxTarget: null,
        startupCommand: null,
        harness: null,
    });
    assert.equal(t.workingDirectory, null);
    assert.equal(t.tmuxTarget, null);
    assert.equal(t.startupCommand, null);
    assert.equal(t.harness, null);

    ws.close();
    await cleanup(dbPath);
});

test("reorder is scoped: reorderGroups by server, reorderSessions by group", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const OTHER = "srv-2";
    const a1 = ws.createGroup({ serverId: SERVER, name: "A1" });
    const a2 = ws.createGroup({ serverId: SERVER, name: "A2" });
    ws.createGroup({ serverId: OTHER, name: "B1" });
    ws.createGroup({ serverId: OTHER, name: "B2" });

    ws.reorderGroups({ serverId: SERVER, orderedIds: [a2.id, a1.id] });
    assert.deepEqual(ws.list(SERVER).map((g) => g.name), ["A2", "A1"]);
    // A different server's ordering must be untouched by the scoped WHERE.
    assert.deepEqual(ws.list(OTHER).map((g) => g.name), ["B1", "B2"]);

    const s1 = ws.createSession({ serverId: SERVER, groupId: a1.id, name: "s1", repositoryRoot: "/r" });
    const s2 = ws.createSession({ serverId: SERVER, groupId: a1.id, name: "s2", repositoryRoot: "/r" });
    ws.createSession({ serverId: SERVER, groupId: a2.id, name: "o1", repositoryRoot: "/r" });
    ws.createSession({ serverId: SERVER, groupId: a2.id, name: "o2", repositoryRoot: "/r" });

    ws.reorderSessions({ groupId: a1.id, orderedIds: [s2.id, s1.id] });
    const groups = ws.list(SERVER);
    assert.deepEqual(groups.find((g) => g.id === a1.id)?.sessions.map((s) => s.name), ["s2", "s1"]);
    // Sessions in the sibling group keep their order (scoped by group_id).
    assert.deepEqual(groups.find((g) => g.id === a2.id)?.sessions.map((s) => s.name), ["o1", "o2"]);

    ws.close();
    await cleanup(dbPath);
});

test("setLayout upserts a single row and returns the updated tree", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });

    ws.setLayout({ serverId: SERVER, devSessionId: s.id, region: "viewer", tree: { type: "leaf", paneId: "p1" } });
    const updated = ws.setLayout({ serverId: SERVER, devSessionId: s.id, region: "viewer", tree: { type: "leaf", paneId: "p2" } });
    assert.deepEqual(updated.tree, { type: "leaf", paneId: "p2" });
    assert.deepEqual(ws.getLayout({ devSessionId: s.id, region: "viewer" })?.tree, { type: "leaf", paneId: "p2" });
    const count = ws.db
        .prepare("SELECT COUNT(*) AS n FROM session_layouts WHERE dev_session_id = ? AND region = ?")
        .get(s.id, "viewer") as { n: number };
    assert.equal(count.n, 1);

    ws.close();
    await cleanup(dbPath);
});

test("duplicateSession mints a fresh tmux target per terminal and remaps the terminal region (SPEC 4.2/4.5)", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });
    const t1 = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "t1", tmuxTarget: "orig1" });
    const t2 = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "t2", tmuxTarget: "orig2" });
    ws.setLayout({
        serverId: SERVER,
        devSessionId: s.id,
        region: "terminal",
        tree: {
            type: "split",
            orientation: "vertical",
            ratios: [0.4, 0.6],
            children: [
                { type: "leaf", paneId: t1.id },
                { type: "leaf", paneId: t2.id },
            ],
        },
    });

    const dup = ws.duplicateSession({ id: s.id });
    const [d1, d2] = dup.terminalPanes;
    assert.notEqual(d1.id, t1.id);
    assert.notEqual(d2.id, t2.id);
    assert.equal(d1.tmuxTarget, `ch_${dup.id}_${d1.id}`);
    assert.equal(d2.tmuxTarget, `ch_${dup.id}_${d2.id}`);
    assert.notEqual(d1.tmuxTarget, d2.tmuxTarget);
    assert.notEqual(d1.tmuxTarget, "orig1");
    // The terminal-region tree remaps to the copied terminal panes, in order.
    assert.deepEqual(dup.layouts.terminal, {
        type: "split",
        orientation: "vertical",
        ratios: [0.4, 0.6],
        children: [
            { type: "leaf", paneId: d1.id },
            { type: "leaf", paneId: d2.id },
        ],
    });
    // The originals are untouched.
    const orig = ws.list(SERVER)[0].sessions.find((x) => x.id === s.id);
    assert.equal(orig?.terminalPanes[0].tmuxTarget, "orig1");
    assert.equal(orig?.terminalPanes[1].tmuxTarget, "orig2");

    ws.close();
    await cleanup(dbPath);
});

test("duplicateSession of an empty session yields fresh id, no panes, null layouts", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });

    const dup = ws.duplicateSession({ id: s.id });
    assert.notEqual(dup.id, s.id);
    assert.equal(dup.viewerPanes.length, 0);
    assert.equal(dup.terminalPanes.length, 0);
    assert.deepEqual(dup.layouts, { viewer: null, terminal: null });

    ws.close();
    await cleanup(dbPath);
});

test("a naive group delete is rejected by the foreign key (manual cascade is load-bearing)", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { group } = seed(ws);
    // Bypassing deleteGroup's manual cascade must be rejected by the FK, proving
    // a delete can never orphan child sessions/panes.
    assert.throws(() => ws.db.prepare("DELETE FROM groups WHERE id = ?").run(group.id));
    ws.close();
    await cleanup(dbPath);
});

test("updateGroup toggles collapsed and preserves it when omitted", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G", collapsed: true });
    assert.equal(g.collapsed, true);
    let u = ws.updateGroup({ id: g.id, collapsed: false });
    assert.equal(u.collapsed, false);
    // Omitting collapsed keeps the current false (?? must not fall back on false).
    u = ws.updateGroup({ id: g.id, name: "G2" });
    assert.equal(u.collapsed, false);
    assert.equal(u.name, "G2");
    ws.close();
    await cleanup(dbPath);
});

test("migrate persists the target schema version even when the seeded row is stale", async () => {
    const dbPath = await tmpDbPath();
    const first = openWorkspace(dbPath);
    // Simulate a database left by an older build whose stored version lags the
    // code's target. schema.sql's INSERT OR IGNORE cannot advance an existing
    // row, so migrate() must record the target version explicitly on next open;
    // otherwise the version silently drifts and schema.sql re-runs every open.
    first.db.prepare("UPDATE schema_version SET version = 0 WHERE id = 1").run();
    first.close();

    const second = openWorkspace(dbPath);
    const row = second.db
        .prepare("SELECT version FROM schema_version WHERE id = 1")
        .get() as { version: number };
    assert.equal(row.version, WORKSPACE_SCHEMA_VERSION);
    const count = second.db.prepare("SELECT COUNT(*) AS n FROM schema_version").get() as { n: number };
    assert.equal(count.n, 1);

    second.close();
    await cleanup(dbPath);
});

test("child rows inherit the parent's server_id, overriding a mismatched param (SPEC 3.5)", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);

    const group = ws.createGroup({ serverId: SERVER, name: "Work" });

    // A caller sends a serverId that does NOT match the parent group. The
    // authoritative value must come from the parent row, not the param, so the
    // session cannot surface under a foreign server's group.
    const session = ws.createSession({
        serverId: "srv-EVIL",
        groupId: group.id,
        name: "s",
        repositoryRoot: "/r",
    });
    assert.equal(session.serverId, SERVER);

    const viewer = ws.createViewerPane({
        serverId: "srv-EVIL",
        devSessionId: session.id,
        url: "http://x",
    });
    assert.equal(viewer.serverId, SERVER);

    const terminal = ws.createTerminalPane({
        serverId: "srv-EVIL",
        devSessionId: session.id,
        name: "sh",
    });
    assert.equal(terminal.serverId, SERVER);

    // The mismatched server sees nothing; the real parent server sees it all.
    assert.deepEqual(ws.list("srv-EVIL"), []);
    const listed = ws.list(SERVER);
    assert.equal(listed.length, 1);
    assert.equal(listed[0].sessions.length, 1);
    assert.equal(listed[0].sessions[0].serverId, SERVER);
    assert.equal(listed[0].sessions[0].viewerPanes[0].serverId, SERVER);
    assert.equal(listed[0].sessions[0].terminalPanes[0].serverId, SERVER);

    ws.close();
    await cleanup(dbPath);
});

test("createSession/createViewerPane reject an unknown parent before minting a row", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);

    assert.throws(
        () => ws.createSession({ serverId: SERVER, groupId: "nope", name: "s", repositoryRoot: "/r" }),
        /groups not found: nope/,
    );
    assert.throws(
        () => ws.createViewerPane({ serverId: SERVER, devSessionId: "nope", url: "http://x" }),
        /dev_sessions not found: nope/,
    );

    ws.close();
    await cleanup(dbPath);
});

test("migrate applies cleanly to a fresh database and is idempotent across reopens", async () => {
    const dbPath = await tmpDbPath();

    // Fresh open: migrate must create every table and record the target version.
    const first = openWorkspace(dbPath);
    const freshVersion = first.db
        .prepare("SELECT version FROM schema_version WHERE id = 1")
        .get() as { version: number };
    assert.equal(freshVersion.version, WORKSPACE_SCHEMA_VERSION);
    const group = first.createGroup({ serverId: SERVER, name: "Work" });
    first.close();

    // Reopen repeatedly: each migrate() is a no-op (stored >= target), leaves a
    // single version row at the target, and never disturbs existing data.
    for (let i = 0; i < 3; i++) {
        const ws = openWorkspace(dbPath);
        const version = ws.db
            .prepare("SELECT version FROM schema_version WHERE id = 1")
            .get() as { version: number };
        assert.equal(version.version, WORKSPACE_SCHEMA_VERSION);
        const count = ws.db
            .prepare("SELECT COUNT(*) AS n FROM schema_version")
            .get() as { n: number };
        assert.equal(count.n, 1);
        assert.deepEqual(ws.getGroup(group.id), group);
        ws.close();
    }

    await cleanup(dbPath);
});

// --- moveSessionToGroup ordering (drag-to-position) -------------------------

// A bare session in `groupId`, named so ordering assertions read as a sequence.
function mkSession(ws: Workspace, groupId: string, name: string) {
    return ws.createSession({ serverId: SERVER, groupId, name, repositoryRoot: "/repo" });
}

// The group's sessions exactly as a client sees them: listing order (which is
// `ORDER BY position, id`) plus the stored positions.
function ordered(ws: Workspace, groupId: string): { names: string[]; positions: number[] } {
    const group = ws.list(SERVER).find((g) => g.id === groupId);
    assert.ok(group, `group not listed: ${groupId}`);
    return {
        names: group.sessions.map((s) => s.name),
        positions: group.sessions.map((s) => s.position),
    };
}

test("moveSessionToGroup at position 0 lands on top of a group that already has a row at 0", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const alpha = ws.createGroup({ serverId: SERVER, name: "Alpha" });
    const beta = ws.createGroup({ serverId: SERVER, name: "Beta" });
    mkSession(ws, alpha.id, "A0"); // already occupies position 0
    mkSession(ws, alpha.id, "A1");
    const moved = mkSession(ws, beta.id, "X");

    // The live failure: the moved row used to keep the raw position 0, tying with
    // A0, and the `position, id` tiebreak dropped it wherever its UUID sorted.
    const result = ws.moveSessionToGroup({ id: moved.id, groupId: alpha.id, position: 0 });

    assert.equal(result.position, 0);
    assert.deepEqual(ordered(ws, alpha.id), { names: ["X", "A0", "A1"], positions: [0, 1, 2] });
    assert.deepEqual(ordered(ws, beta.id), { names: [], positions: [] });

    ws.close();
    await cleanup(dbPath);
});

test("moveSessionToGroup re-packs both the source and the target group to 0..n-1", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const alpha = ws.createGroup({ serverId: SERVER, name: "Alpha" });
    const beta = ws.createGroup({ serverId: SERVER, name: "Beta" });
    mkSession(ws, alpha.id, "A0");
    const a1 = mkSession(ws, alpha.id, "A1"); // middle row: removing it leaves a hole
    mkSession(ws, alpha.id, "A2");
    mkSession(ws, beta.id, "B0");
    mkSession(ws, beta.id, "B1");

    ws.moveSessionToGroup({ id: a1.id, groupId: beta.id, position: 1 });

    assert.deepEqual(ordered(ws, alpha.id), { names: ["A0", "A2"], positions: [0, 1] });
    assert.deepEqual(ordered(ws, beta.id), { names: ["B0", "A1", "B1"], positions: [0, 1, 2] });

    ws.close();
    await cleanup(dbPath);
});

test("moveSessionToGroup reorders within the same group and appends at position n", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const group = ws.createGroup({ serverId: SERVER, name: "Alpha" });
    mkSession(ws, group.id, "A0");
    mkSession(ws, group.id, "A1");
    const a2 = mkSession(ws, group.id, "A2");

    // Last -> first: a same-group move is a reorder, never a duplicate position.
    ws.moveSessionToGroup({ id: a2.id, groupId: group.id, position: 0 });
    assert.deepEqual(ordered(ws, group.id), { names: ["A2", "A0", "A1"], positions: [0, 1, 2] });

    // P = n (the count after removing the moved row) means "the end".
    ws.moveSessionToGroup({ id: a2.id, groupId: group.id, position: 2 });
    assert.deepEqual(ordered(ws, group.id), { names: ["A0", "A1", "A2"], positions: [0, 1, 2] });

    // An omitted position also appends, and still leaves the group packed.
    const a0 = ws.list(SERVER).find((g) => g.id === group.id)!.sessions[0];
    ws.moveSessionToGroup({ id: a0.id, groupId: group.id });
    assert.deepEqual(ordered(ws, group.id), { names: ["A1", "A2", "A0"], positions: [0, 1, 2] });

    ws.close();
    await cleanup(dbPath);
});

test("moveSessionToGroup clamps an out-of-range position instead of storing it raw", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const alpha = ws.createGroup({ serverId: SERVER, name: "Alpha" });
    const beta = ws.createGroup({ serverId: SERVER, name: "Beta" });
    mkSession(ws, alpha.id, "A0");
    mkSession(ws, alpha.id, "A1");
    const x = mkSession(ws, beta.id, "X");
    const y = mkSession(ws, beta.id, "Y");

    const far = ws.moveSessionToGroup({ id: x.id, groupId: alpha.id, position: 99 });
    assert.equal(far.position, 2);
    assert.deepEqual(ordered(ws, alpha.id), { names: ["A0", "A1", "X"], positions: [0, 1, 2] });

    const negative = ws.moveSessionToGroup({ id: y.id, groupId: alpha.id, position: -5 });
    assert.equal(negative.position, 0);
    assert.deepEqual(ordered(ws, alpha.id), { names: ["Y", "A0", "A1", "X"], positions: [0, 1, 2, 3] });

    ws.close();
    await cleanup(dbPath);
});

test("session ordering stays tie-free and stable across repeated reads after moves", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const alpha = ws.createGroup({ serverId: SERVER, name: "Alpha" });
    const beta = ws.createGroup({ serverId: SERVER, name: "Beta" });
    const ids: string[] = [];
    for (let i = 0; i < 6; i++) ids.push(mkSession(ws, i % 2 === 0 ? alpha.id : beta.id, `S${i}`).id);

    // A pile of cross-group and same-group drags, always at the contested top.
    for (const id of ids) {
        ws.moveSessionToGroup({ id, groupId: alpha.id, position: 0 });
        ws.moveSessionToGroup({ id, groupId: beta.id, position: 1 });
    }

    for (const groupId of [alpha.id, beta.id]) {
        const rows = ws.db
            .prepare("SELECT position FROM dev_sessions WHERE group_id = ?").all(groupId) as unknown as Array<{ position: number }>;
        const positions = rows.map((r) => r.position);
        // No two rows in a group share a position, so no read depends on the
        // UUID tiebreak in `ORDER BY position, id`.
        assert.equal(new Set(positions).size, positions.length);
        assert.deepEqual(
            [...positions].sort((a, b) => a - b),
            positions.map((_, i) => i),
        );
        // Repeated reads return the identical sequence.
        const first = ordered(ws, groupId);
        assert.deepEqual(ordered(ws, groupId), first);
        assert.deepEqual(ordered(ws, groupId), first);
    }

    ws.close();
    await cleanup(dbPath);
});

// --- Server identity (SPEC 3.5) ---------------------------------------------

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;

test("serverId mints a UUID on first call, stores it, and repeats it", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);

    // Nothing is minted until asked: opening the DB must not write identity.
    const before = ws.db.prepare("SELECT COUNT(*) AS n FROM server_identity").get() as { n: number };
    assert.equal(before.n, 0);

    const id = ws.serverId();
    assert.match(id, UUID_RE);
    // Same connection, repeated calls: one identity, one row.
    assert.equal(ws.serverId(), id);
    assert.equal(ws.serverId(), id);
    const stored = ws.db
        .prepare("SELECT id, server_id, created_at FROM server_identity")
        .all() as unknown as Array<{ id: number; server_id: string; created_at: number }>;
    assert.equal(stored.length, 1);
    assert.equal(stored[0]?.id, 1);
    assert.equal(stored[0]?.server_id, id);
    assert.ok((stored[0]?.created_at ?? 0) > 0);

    ws.close();
    await cleanup(dbPath);
});

test("serverId survives a restart: reopening the same file returns the same id", async () => {
    const dbPath = await tmpDbPath();
    const first = openWorkspace(dbPath);
    const id = first.serverId();
    first.close();

    // A fresh process would do exactly this: reopen the file and ask again.
    const second = openWorkspace(dbPath);
    assert.equal(second.serverId(), id);
    const count = second.db.prepare("SELECT COUNT(*) AS n FROM server_identity").get() as { n: number };
    assert.equal(count.n, 1);
    second.close();
    await cleanup(dbPath);
});

test("two databases mint different identities", async () => {
    const a = await tmpDbPath();
    const b = await tmpDbPath();
    const wsA = openWorkspace(a);
    const wsB = openWorkspace(b);
    assert.notEqual(wsA.serverId(), wsB.serverId());
    wsA.close();
    wsB.close();
    await cleanup(a);
    await cleanup(b);
});

test("concurrent mints from two connections converge on one id", async () => {
    const dbPath = await tmpDbPath();
    // BOTH connections are opened before either mints — the exact race two
    // codeharbord processes hit against a shared file (tst_liveshell starts a
    // second server). Each sees an empty server_identity and tries to insert.
    const first = openWorkspace(dbPath);
    const second = openWorkspace(dbPath);

    const a = first.serverId();
    const b = second.serverId();
    assert.match(a, UUID_RE);
    assert.equal(b, a);

    // The loser's INSERT OR IGNORE left no second row and no stale value: a
    // third opener sees the same single winner.
    const rows = second.db
        .prepare("SELECT server_id FROM server_identity")
        .all() as unknown as Array<{ server_id: string }>;
    assert.deepEqual(rows.map((r) => r.server_id), [a]);
    first.close();
    second.close();

    const third = openWorkspace(dbPath);
    assert.equal(third.serverId(), a);
    third.close();
    await cleanup(dbPath);
});

test("the v2 migration adds server_identity to a v1 database without losing rows", async () => {
    const dbPath = await tmpDbPath();
    // Simulate a database written before server_identity existed: drop the
    // table and rewind the stored version so migrate() has real work to do.
    const legacy = openWorkspace(dbPath);
    const { group, session } = seed(legacy);
    legacy.db.exec("DROP TABLE server_identity");
    legacy.db.prepare("UPDATE schema_version SET version = 1 WHERE id = 1").run();
    legacy.close();

    const upgraded = openWorkspace(dbPath);
    const version = upgraded.db
        .prepare("SELECT version FROM schema_version WHERE id = 1")
        .get() as { version: number };
    assert.equal(version.version, WORKSPACE_SCHEMA_VERSION);
    assert.match(upgraded.serverId(), UUID_RE);
    // The pre-existing rows are untouched by the upgrade.
    assert.deepEqual(upgraded.getGroup(group.id), group);
    assert.deepEqual(upgraded.getSession(session.id), session);
    upgraded.close();
    await cleanup(dbPath);
});

test("serverId is independent of the database's path", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const id = ws.serverId();
    ws.close();

    // Moving the file (a relocated deployment, a renamed home directory) must
    // not change the identity: it names the DATA, not the route to it.
    const movedDir = await fs.mkdtemp(path.join(os.tmpdir(), "codeharbord-moved-"));
    const moved = path.join(movedDir, "elsewhere.sqlite");
    await fs.rename(dbPath, moved);
    const reopened = openWorkspace(moved);
    assert.equal(reopened.serverId(), id);
    reopened.close();
    await fs.rm(movedDir, { recursive: true, force: true });
    await cleanup(dbPath);
});

// --- Re-pack robustness, layout upsert, cross-server moves, migration guard --

test("reorderGroups/reorderSessions keep the scope packed when the given order is partial", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const a = mkSession(ws, g.id, "A");
    const b = mkSession(ws, g.id, "B");
    mkSession(ws, g.id, "C");

    // A client that sends a filtered/stale list (only B, plus an id that is not
    // in this group at all, plus a repeat) must still leave the group with the
    // requested rows first and EVERY row on a unique, contiguous position.
    // Renumbering only the listed ids used to leave B at 0 next to A at 0, and
    // `ORDER BY position, id` then broke the tie by UUID, i.e. at random.
    ws.reorderSessions({ groupId: g.id, orderedIds: [b.id, "not-a-session", b.id] });
    assert.deepEqual(ordered(ws, g.id), { names: ["B", "A", "C"], positions: [0, 1, 2] });

    // A stale id belonging to another group cannot drag that row into this one.
    const other = ws.createGroup({ serverId: SERVER, name: "Other" });
    const outsider = mkSession(ws, other.id, "X");
    ws.reorderSessions({ groupId: g.id, orderedIds: [outsider.id, a.id] });
    assert.deepEqual(ordered(ws, g.id), { names: ["A", "B", "C"], positions: [0, 1, 2] });
    assert.deepEqual(ordered(ws, other.id), { names: ["X"], positions: [0] });

    // Same contract for groups, scoped by server.
    const h = ws.createGroup({ serverId: SERVER, name: "H" });
    ws.reorderGroups({ serverId: SERVER, orderedIds: [h.id] });
    const groups = ws.list(SERVER);
    assert.deepEqual(groups.map((x) => x.name), ["H", "G", "Other"]);
    assert.deepEqual(groups.map((x) => x.position), [0, 1, 2]);

    ws.close();
    await cleanup(dbPath);
});

test("setLayout takes server_id from the parent session and rejects an unknown one (SPEC 3.5)", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });

    // Same rule as createViewerPane/createTerminalPane: a mismatched serverId in
    // the params cannot detach the layout row from its session's server.
    const first = ws.setLayout({
        serverId: "srv-EVIL",
        devSessionId: s.id,
        region: "viewer",
        tree: { type: "leaf", paneId: "p1" },
    });
    assert.equal(first.serverId, SERVER);

    // The upsert rewrites the SAME row: one row, stable id, original created_at.
    const second = ws.setLayout({
        serverId: SERVER,
        devSessionId: s.id,
        region: "viewer",
        tree: { type: "leaf", paneId: "p2" },
    });
    assert.equal(second.id, first.id);
    assert.equal(second.createdAt, first.createdAt);
    assert.deepEqual(second.tree, { type: "leaf", paneId: "p2" });

    assert.throws(
        () => ws.setLayout({ serverId: SERVER, devSessionId: "nope", region: "viewer", tree: {} }),
        /dev_sessions not found: nope/,
    );

    ws.close();
    await cleanup(dbPath);
});

test("moveSessionToGroup re-homes the session and its children onto the target group's server", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const OTHER = "srv-2";
    const home = ws.createGroup({ serverId: SERVER, name: "Home" });
    const away = ws.createGroup({ serverId: OTHER, name: "Away" });
    const s = ws.createSession({ serverId: SERVER, groupId: home.id, name: "S", repositoryRoot: "/r" });
    const v = ws.createViewerPane({ serverId: SERVER, devSessionId: s.id, url: "http://x" });
    const t = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "sh" });
    ws.setLayout({
        serverId: SERVER,
        devSessionId: s.id,
        region: "viewer",
        tree: { type: "leaf", paneId: v.id },
    });

    const moved = ws.moveSessionToGroup({ id: s.id, groupId: away.id });

    // SPEC 3.5's invariant is that a row shares its ancestors' server, so the
    // move carries the session AND its panes and layouts to the new server.
    // Leaving the old server_id behind listed the session under the target group
    // while reporting a foreign serverId on the row itself.
    assert.equal(moved.serverId, OTHER);
    assert.equal(ws.getViewerPane(v.id).serverId, OTHER);
    assert.equal(ws.getTerminalPane(t.id).serverId, OTHER);
    assert.equal(ws.getLayout({ devSessionId: s.id, region: "viewer" })?.serverId, OTHER);
    assert.deepEqual(ws.list(SERVER).map((x) => x.sessions.length), [0]);
    assert.deepEqual(ws.list(OTHER)[0].sessions.map((x) => x.name), ["S"]);

    // An unknown target group is rejected before any write, so the session stays
    // exactly where it was.
    assert.throws(() => ws.moveSessionToGroup({ id: s.id, groupId: "nope" }), /groups not found: nope/);
    assert.equal(ws.getSession(s.id).groupId, away.id);
    assert.equal(ws.getSession(s.id).serverId, OTHER);

    ws.close();
    await cleanup(dbPath);
});

test("migrate refuses a database written by a newer build instead of using it", async () => {
    const dbPath = await tmpDbPath();
    const first = openWorkspace(dbPath);
    // A future release's database, reopened by this build. Its migrations may
    // have renamed or dropped columns this build still selects, so opening it
    // must fail loudly rather than fail later, one confusing query at a time.
    first.db
        .prepare("UPDATE schema_version SET version = ? WHERE id = 1")
        .run(WORKSPACE_SCHEMA_VERSION + 1);
    first.close();

    assert.throws(() => openWorkspace(dbPath), /newer than this build supports/);

    await cleanup(dbPath);
});

// The four lookup columns that every listing joins or filters on. They are
// indexed by remote/sql/indexes.sql, which openWorkspace applies on EVERY open
// rather than through the migration runner: migrate() skips its DDL entirely
// once the stored schema_version has reached the current one, so an index
// delivered only by schema.sql would never appear on an existing database — and
// bumping the version to force it is not available, because that number is
// mirrored in C++ (WorkspaceDb::kSchemaVersion) and a one-sided bump breaks the
// client's compatibility gate.
const EXPECTED_INDEXES = [
    "idx_dev_sessions_group_id",
    "idx_groups_server_id",
    "idx_terminal_panes_dev_session_id",
    "idx_viewer_panes_dev_session_id",
];

function indexNames(ws: Workspace): string[] {
    const rows = ws.db
        .prepare("SELECT name FROM sqlite_master WHERE type = 'index' AND name LIKE 'idx_%' ORDER BY name")
        .all() as { name: string }[];
    return rows.map((r) => r.name);
}

test("opening an existing database without the lookup indexes creates them", async () => {
    const dbPath = await tmpDbPath();
    const first = openWorkspace(dbPath);
    assert.deepEqual(indexNames(first), EXPECTED_INDEXES);

    // Reproduce a database written by an older build: the rows and the CURRENT
    // schema version are there, the indexes are not. This is the case the
    // migration runner cannot fix, because it has nothing left to migrate.
    const { group } = seed(first);
    for (const name of EXPECTED_INDEXES) {
        first.db.exec(`DROP INDEX ${name}`);
    }
    assert.deepEqual(indexNames(first), []);
    const stored = first.db
        .prepare("SELECT version FROM schema_version WHERE id = 1")
        .get() as { version: number };
    assert.equal(stored.version, WORKSPACE_SCHEMA_VERSION);
    first.close();

    const second = openWorkspace(dbPath);
    assert.deepEqual(indexNames(second), EXPECTED_INDEXES);
    // The upgrade adds indexes and nothing else: the data and the version are
    // untouched, and a third open is a no-op rather than an error.
    assert.deepEqual(second.getGroup(group.id), group);
    const after = second.db
        .prepare("SELECT version FROM schema_version WHERE id = 1")
        .get() as { version: number };
    assert.equal(after.version, WORKSPACE_SCHEMA_VERSION);
    second.close();

    const third = openWorkspace(dbPath);
    assert.deepEqual(indexNames(third), EXPECTED_INDEXES);
    third.close();

    await cleanup(dbPath);
});

// An index nobody can use is worse than none: it costs every write and buys
// nothing. Ask SQLite's planner directly whether the session listing uses the
// group_id index, so a typo in a column name fails here rather than passing as
// "the index exists".
test("the session lookup index is actually chosen by the query planner", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const plan = ws.db
        .prepare("EXPLAIN QUERY PLAN SELECT * FROM dev_sessions WHERE group_id = ?")
        .all("g1") as { detail: string }[];
    assert.match(
        plan.map((r) => r.detail).join("\n"),
        /USING INDEX idx_dev_sessions_group_id/,
    );
    ws.close();
    await cleanup(dbPath);
});

// --- RW13: server repairs layout integrity on pane delete -------------------

test("RW13: deleting a pane in a 2-pane split promotes the sibling, layout row id/server_id unchanged", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const group = ws.createGroup({ serverId: SERVER, name: "Work" });
    const session = ws.createSession({
        serverId: SERVER,
        groupId: group.id,
        name: "s",
        repositoryRoot: "/r",
    });
    const a = ws.createViewerPane({ serverId: SERVER, devSessionId: session.id, url: "https://a" });
    const b = ws.createViewerPane({ serverId: SERVER, devSessionId: session.id, url: "https://b" });
    ws.setLayout({
        serverId: SERVER,
        devSessionId: session.id,
        region: "viewer",
        tree: {
            type: "split",
            orientation: "horizontal",
            children: [
                { type: "leaf", paneId: a.id },
                { type: "leaf", paneId: b.id },
            ],
            ratios: [0.5, 0.5],
        },
    });
    const before = ws.getLayout({ devSessionId: session.id, region: "viewer" });
    assert.ok(before);

    ws.deleteViewerPane({ id: a.id });

    const after = ws.getLayout({ devSessionId: session.id, region: "viewer" });
    assert.ok(after);
    // The surviving sibling is promoted in place; no leaf names the deleted pane.
    assert.deepEqual(after.tree, { type: "leaf", paneId: b.id });
    // The layout row keeps its identity and server.
    assert.equal(after.id, before.id);
    assert.equal(after.serverId, before.serverId);
    assert.equal(after.createdAt, before.createdAt);

    ws.close();
    await cleanup(dbPath);
});

test("RW13: deleting the last pane in a region leaves a single empty leaf", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const group = ws.createGroup({ serverId: SERVER, name: "Work" });
    const session = ws.createSession({
        serverId: SERVER,
        groupId: group.id,
        name: "s",
        repositoryRoot: "/r",
    });
    const t = ws.createTerminalPane({ serverId: SERVER, devSessionId: session.id, name: "shell" });
    ws.setLayout({
        serverId: SERVER,
        devSessionId: session.id,
        region: "terminal",
        tree: { type: "leaf", paneId: t.id },
    });
    const before = ws.getLayout({ devSessionId: session.id, region: "terminal" });
    assert.ok(before);

    ws.deleteTerminalPane({ id: t.id });

    const after = ws.getLayout({ devSessionId: session.id, region: "terminal" });
    assert.ok(after);
    assert.deepEqual(after.tree, { type: "leaf", paneId: "" });
    assert.equal(after.id, before.id);
    assert.equal(after.serverId, before.serverId);

    ws.close();
    await cleanup(dbPath);
});

test("RW13: deleting a pane with no stored layout row is a no-op", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const group = ws.createGroup({ serverId: SERVER, name: "Work" });
    const session = ws.createSession({
        serverId: SERVER,
        groupId: group.id,
        name: "s",
        repositoryRoot: "/r",
    });
    const v = ws.createViewerPane({ serverId: SERVER, devSessionId: session.id, url: "https://a" });

    assert.deepEqual(ws.deleteViewerPane({ id: v.id }), { ok: true });
    assert.equal(ws.getLayout({ devSessionId: session.id, region: "viewer" }), null);

    ws.close();
    await cleanup(dbPath);
});

// --- RW14: a corrupt tree must not fail the whole listing -------------------

test("RW14: a corrupt stored layout tree is self-healed to null, not thrown", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { session } = seed(ws);

    // Simulate corruption from outside this store (the store only ever writes
    // JSON.stringify output): overwrite the viewer tree with unparseable text.
    ws.db
        .prepare("UPDATE session_layouts SET tree = ? WHERE dev_session_id = ? AND region = ?")
        .run("{not valid json", session.id, "viewer");

    // getLayout reports the corrupt region as null instead of throwing.
    assert.equal(ws.getLayout({ devSessionId: session.id, region: "viewer" }), null);

    // The nested listing still returns; the good region survives, the corrupt
    // one reads as null.
    const groups = ws.list(SERVER);
    const listed = groups[0].sessions[0];
    assert.equal(listed.layouts.viewer, null);
    assert.notEqual(listed.layouts.terminal, null);

    ws.close();
    await cleanup(dbPath);
});

// --- RW15: every handler validates its params -------------------------------

test("RW15: createGroup handler rejects a params object missing name", () => {
    assert.throws(
        () => WORKSPACE_METHODS["workspace.createGroup"]({ serverId: SERVER }),
        /workspace\.createGroup: missing or invalid field 'name'/,
    );
});

test("RW15: moveSessionToGroup handler rejects a non-string id", () => {
    assert.throws(
        () => WORKSPACE_METHODS["workspace.moveSessionToGroup"]({ id: 123, groupId: "g" }),
        /workspace\.moveSessionToGroup: missing or invalid field 'id'/,
    );
});

test("RW15: a handler rejects a non-object params value", () => {
    assert.throws(
        () => WORKSPACE_METHODS["workspace.list"](null),
        /workspace\.list: missing or invalid params object/,
    );
});
