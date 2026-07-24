import { test } from "node:test";
import assert from "node:assert/strict";
import { promises as fs } from "node:fs";
import os from "node:os";
import path from "node:path";

import { openWorkspace, WORKSPACE_SCHEMA_VERSION } from "../src/workspace.ts";
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
