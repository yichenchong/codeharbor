import { test } from "node:test";
import assert from "node:assert/strict";
import { promises as fs } from "node:fs";
import os from "node:os";
import path from "node:path";
import { DatabaseSync } from "node:sqlite";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

import {
    applyConnectionPragmas,
    isDatabaseBusy,
    openWorkspace,
    WORKSPACE_SCHEMA_VERSION,
    WORKSPACE_METHODS,
} from "../src/workspace.ts";
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

    // Force a failure in the LAST copy step (the layouts), after the new
    // session and both panes have already been inserted, so a missing
    // transaction would leave a half-built duplicate behind. A trigger that
    // aborts every session_layouts insert is the injection: it is independent
    // of how duplicateSession happens to handle any particular bad input, so
    // the test keeps measuring atomicity and nothing else.
    ws.db.exec(
        "CREATE TRIGGER fail_layout_copy BEFORE INSERT ON session_layouts " +
            "BEGIN SELECT RAISE(ABORT, 'injected layout insert failure'); END",
    );

    assert.throws(() => ws.duplicateSession({ id: session.id }));

    ws.db.exec("DROP TRIGGER fail_layout_copy");

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

// A corrupt layout blob self-heals to "no layout" on every read path (RW14).
// Duplicating used to be the one exception: JSON.parse threw out of the copy
// loop and the whole "Duplicate Dev Session" action failed for a session whose
// panes were all intact. The duplicate must be created, with the readable
// region copied and the broken one simply absent.
test("duplicateSession skips an unparseable layout instead of failing the copy", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const { group, session } = seed(ws);

    ws.db
        .prepare("UPDATE session_layouts SET tree = ? WHERE dev_session_id = ? AND region = ?")
        .run("{ not valid json", session.id, "terminal");

    const copy = ws.duplicateSession({ id: session.id });

    assert.notEqual(copy.id, session.id);
    assert.equal(copy.viewerPanes.length, 1);
    assert.equal(copy.terminalPanes.length, 1);
    // The readable region came across; the corrupt one is reported as absent
    // rather than copied as garbage.
    assert.ok(copy.layouts.viewer, "the parseable viewer layout must be copied");
    assert.equal(copy.layouts.terminal, null);
    // The group now holds both sessions and the corrupt source blob is left
    // exactly as it was — duplicating reads, it never rewrites the original.
    const sessions = ws.list(SERVER)[0].sessions;
    assert.equal(sessions.length, 2);
    assert.equal(sessions[0].groupId, group.id);
    const sourceTree = ws.db
        .prepare("SELECT tree FROM session_layouts WHERE dev_session_id = ? AND region = 'terminal'")
        .get(session.id) as { tree: string };
    assert.equal(sourceTree.tree, "{ not valid json");

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

// --- tmux targets: one minting site, safe by construction, unique (v3) ------

// Rebuild terminal_panes the way schema v2 declared it — no UNIQUE on
// tmux_target — and rewind the stored version, so the next open has the v3
// migration's real job to do. SQLite cannot drop a constraint, so the table is
// re-created and the rows copied across, which is exactly what a v2 database
// looks like on disk.
function downgradeToV2(ws: Workspace): void {
    ws.db.exec(`
        ALTER TABLE terminal_panes RENAME TO terminal_panes_v2_tmp;
        CREATE TABLE terminal_panes (
            id                TEXT    NOT NULL PRIMARY KEY,
            server_id         TEXT    NOT NULL,
            dev_session_id    TEXT    NOT NULL,
            name              TEXT    NOT NULL,
            working_directory TEXT,
            tmux_target       TEXT,
            startup_command   TEXT,
            harness           TEXT,
            position          INTEGER NOT NULL DEFAULT 0,
            created_at        INTEGER NOT NULL,
            updated_at        INTEGER NOT NULL,
            FOREIGN KEY (dev_session_id) REFERENCES dev_sessions (id)
        );
        INSERT INTO terminal_panes SELECT * FROM terminal_panes_v2_tmp;
        DROP TABLE terminal_panes_v2_tmp;
    `);
    ws.db.prepare("UPDATE schema_version SET version = 2 WHERE id = 1").run();
}

// SQL identifier quoting, the same rule the source uses (quoteIdentifier in
// remote/src/workspace.ts): an embedded double quote is DOUBLED, never
// backslash-escaped. These helpers are handed index names straight out of the
// catalogue, so they have to survive a name that contains one.
function quotedIdentifier(name: string): string {
    return `"${name.replace(/"/g, '""')}"`;
}

// Every unique index over exactly (tmux_target), however it was declared: a
// table-level UNIQUE shows up as an sqlite_autoindex, the migration's as a
// named one.
function uniqueTmuxTargetIndexes(ws: Workspace): string[] {
    const names: string[] = [];
    const indexes = ws.db.prepare("PRAGMA index_list(terminal_panes)").all() as unknown as {
        name: string;
        unique: number;
    }[];
    for (const index of indexes) {
        if (index.unique !== 1) continue;
        const columns = ws.db
            .prepare(`PRAGMA index_info(${quotedIdentifier(index.name)})`)
            .all() as unknown as { name: string | null }[];
        if (columns.length === 1 && columns[0].name === "tmux_target") names.push(index.name);
    }
    return names;
}

// The whole point of moving identity to the server: the client no longer names
// a pane's tmux session, it reads the name off the row it created. A pane that
// came back with a null target would send the client straight back to inventing
// `ch_<devSessionId>_<layoutPaneId>` — the recycling bug this replaces.
test("createTerminalPane mints a tmux target from the row id when none is given", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });

    const first = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-1" });
    const second = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-2" });

    assert.equal(first.tmuxTarget, `ch_${s.id}_${first.id}`);
    assert.equal(second.tmuxTarget, `ch_${s.id}_${second.id}`);
    assert.notEqual(first.tmuxTarget, second.tmuxTarget);
    // A layout slot label recycles per Dev Session, and two clients each keep
    // their own counter — so `terminal-1` will be asked for again by somebody
    // who has never seen this row. That is now ALLOWED and makes a SEPARATE
    // row: closing a pane keeps its row and its remote shell alive, so the
    // label is free in the layout while the old shell is still running, and a
    // new pane asking for that label wants a new terminal, not the old one.
    // Identity is the row id the layout leaf carries, never the label. (v3
    // enforced UNIQUE (dev_session_id, name) here and rejected this create;
    // v4 removed it — see WORKSPACE_SCHEMA_VERSION.)
    const relabelled = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-1" });
    assert.notEqual(relabelled.id, first.id);
    assert.notEqual(relabelled.tmuxTarget, first.tmuxTarget);

    ws.close();
    await cleanup(dbPath);
});

// tmux reads `:` and `.` as the session/window/pane separators, and refuses to
// store them in a session name at all (it rewrites both to `_`). A target
// carrying one names a session that cannot exist, so `-t '=<target>'` misses it
// and an unanchored match can land on a different session entirely. The caller
// is told rather than quietly given a different target than it asked for.
test("createTerminalPane and updateTerminalPane reject a structurally unsafe tmux target", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });

    for (const bad of ["ch_a:0", "ch_a.1", "", "ch_a b", "=ch_a", "ch_*", "$3", "ch_a\n9"]) {
        assert.throws(
            () => ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "t", tmuxTarget: bad }),
            /tmuxTarget must be/,
            `expected ${JSON.stringify(bad)} to be refused`,
        );
    }
    // Refused before the INSERT: no half-made row is left behind.
    assert.deepEqual(ws.list(SERVER)[0].sessions[0].terminalPanes, []);

    const t = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "t" });
    assert.throws(() => ws.updateTerminalPane({ id: t.id, tmuxTarget: "ch_a:0" }), /tmuxTarget must be/);
    assert.equal(ws.getTerminalPane(t.id).tmuxTarget, t.tmuxTarget);
    // Explicitly clearing the binding is still allowed: a pane with no remote
    // session is a legitimate state, and SQLite permits many NULLs under UNIQUE.
    assert.equal(ws.updateTerminalPane({ id: t.id, tmuxTarget: null }).tmuxTarget, null);

    ws.close();
    await cleanup(dbPath);
});

// Two panes on one target attach the SAME remote shell: each sees the other's
// keystrokes and they fight over the terminal size. The database refuses it.
test("terminal_panes.tmux_target is UNIQUE, and many NULLs are still allowed", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });
    assert.equal(uniqueTmuxTargetIndexes(ws).length, 1);

    const a = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "a", tmuxTarget: "ch_shared" });
    assert.throws(
        () => ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "b", tmuxTarget: "ch_shared" }),
        /UNIQUE constraint failed: terminal_panes.tmux_target/,
    );
    // Retargeting an existing pane onto a taken target is the same collision
    // reached from the other direction, and is refused the same way.
    const b = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "b" });
    assert.throws(
        () => ws.updateTerminalPane({ id: b.id, tmuxTarget: "ch_shared" }),
        /UNIQUE constraint failed: terminal_panes.tmux_target/,
    );
    assert.equal(ws.getTerminalPane(a.id).tmuxTarget, "ch_shared");

    const n1 = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "n1" });
    const n2 = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "n2" });
    ws.updateTerminalPane({ id: n1.id, tmuxTarget: null });
    ws.updateTerminalPane({ id: n2.id, tmuxTarget: null });
    assert.equal(ws.getTerminalPane(n1.id).tmuxTarget, null);
    assert.equal(ws.getTerminalPane(n2.id).tmuxTarget, null);

    ws.close();
    await cleanup(dbPath);
});

test("the v3 migration repairs and de-duplicates tmux targets before constraining them", async () => {
    const dbPath = await tmpDbPath();
    const legacy = openWorkspace(dbPath);
    const g = legacy.createGroup({ serverId: SERVER, name: "G" });
    const s = legacy.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });
    const a = legacy.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "a" });
    const b = legacy.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "b" });
    const c = legacy.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "c" });
    const d = legacy.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "d" });
    downgradeToV2(legacy);
    // What a v2 database could hold and v3 cannot: two rows on one target (the
    // mirrored-keystrokes bug), a target tmux would have rewritten (`.`), and a
    // row deliberately left unbound.
    const set = legacy.db.prepare("UPDATE terminal_panes SET tmux_target = ? WHERE id = ?");
    set.run("ch_shared", a.id);
    set.run("ch_shared", b.id);
    set.run("ch_dotted.name", c.id);
    set.run(null, d.id);
    assert.deepEqual(uniqueTmuxTargetIndexes(legacy), []);
    legacy.close();

    const upgraded = openWorkspace(dbPath);
    const version = upgraded.db
        .prepare("SELECT version FROM schema_version WHERE id = 1")
        .get() as { version: number };
    assert.equal(version.version, WORKSPACE_SCHEMA_VERSION);
    assert.equal(uniqueTmuxTargetIndexes(upgraded).length, 1);

    // The OLDEST row keeps the contested target: it is the one whose shell has
    // been running longest, so the pane the user has been working in is the one
    // that stays attached to it.
    assert.equal(upgraded.getTerminalPane(a.id).tmuxTarget, "ch_shared");
    // The loser is re-minted onto its own canonical target rather than nulled,
    // so it opens a shell of its own on the next attach instead of having none.
    assert.equal(upgraded.getTerminalPane(b.id).tmuxTarget, `ch_${s.id}_${b.id}`);
    // `.` is rewritten to `_` — the name tmux ACTUALLY created the session
    // under, so this row stops missing its own long-running shell.
    assert.equal(upgraded.getTerminalPane(c.id).tmuxTarget, "ch_dotted_name");
    // An unbound pane stays unbound; nothing is invented for it.
    assert.equal(upgraded.getTerminalPane(d.id).tmuxTarget, null);
    // Every row survived, names and all.
    assert.deepEqual(
        upgraded.list(SERVER)[0].sessions[0].terminalPanes.map((p) => p.name),
        ["a", "b", "c", "d"],
    );
    // And the constraint is live from here on.
    assert.throws(
        () => upgraded.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "e", tmuxTarget: "ch_shared" }),
        /UNIQUE constraint failed: terminal_panes.tmux_target/,
    );
    upgraded.close();

    // Reopening is a no-op: no second index, no further rewriting.
    const again = openWorkspace(dbPath);
    assert.equal(uniqueTmuxTargetIndexes(again).length, 1);
    assert.equal(again.getTerminalPane(a.id).tmuxTarget, "ch_shared");
    again.close();

    await cleanup(dbPath);
});

// Same shape as uniqueTmuxTargetIndexes, for the pane's OTHER identity.
function uniqueAddressIndexes(ws: Workspace): string[] {
    const names: string[] = [];
    const indexes = ws.db.prepare("PRAGMA index_list(terminal_panes)").all() as unknown as {
        name: string;
        unique: number;
    }[];
    for (const index of indexes) {
        if (index.unique !== 1) continue;
        const columns = ws.db
            .prepare(`PRAGMA index_info(${quotedIdentifier(index.name)})`)
            .all() as unknown as { name: string | null }[];
        if (
            columns.length === 2 &&
            columns[0].name === "dev_session_id" &&
            columns[1].name === "name"
        ) {
            names.push(index.name);
        }
    }
    return names;
}

// The method the desktop client actually calls to learn which remote tmux
// session a terminal pane owns. Everything hangs off it being lookup-or-create
// rather than create: the pane asks the identical question on its first attach,
// on every reconnect and after every restart, and must get the same row.
test("resolveTerminalPane creates a slot's row once and answers with it for ever after", async () => {
    const dbPath = await tmpDbPath();
    let ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });

    const first = ws.resolveTerminalPane({
        serverId: SERVER,
        devSessionId: s.id,
        name: "terminal-1",
        workingDirectory: "/r",
    });
    assert.equal(first.name, "terminal-1");
    assert.equal(first.workingDirectory, "/r");
    assert.equal(first.tmuxTarget, `ch_${s.id}_${first.id}`);

    // Asked again: the SAME row, not a second one. This is the reconnect case.
    const again = ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-1" });
    assert.equal(again.id, first.id);
    assert.equal(again.tmuxTarget, first.tmuxTarget);

    // A different slot of the same Dev Session is a different terminal.
    const second = ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-2" });
    assert.notEqual(second.id, first.id);
    assert.notEqual(second.tmuxTarget, first.tmuxTarget);

    // The same slot NAME in another Dev Session is also a different terminal:
    // layout pane ids are minted per Dev Session, so "terminal-1" exists in all
    // of them and must never be treated as one shared terminal.
    const other = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S2", repositoryRoot: "/r" });
    const elsewhere = ws.resolveTerminalPane({ serverId: SERVER, devSessionId: other.id, name: "terminal-1" });
    assert.notEqual(elsewhere.tmuxTarget, first.tmuxTarget);

    // Across a restart the identity is still the row's, which is the whole
    // point: the tmux session survives the client, so the name for it must too.
    ws.close();
    ws = openWorkspace(dbPath);
    assert.equal(
        ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-1" }).tmuxTarget,
        first.tmuxTarget,
    );
    assert.equal(ws.list(SERVER)[0].sessions.find((x) => x.id === s.id)?.terminalPanes.length, 2);

    // A row somehow left with no target does not come back unusable: the client
    // is deliberately incapable of minting one, so the server fills it in.
    ws.updateTerminalPane({ id: first.id, tmuxTarget: null });
    const healed = ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-1" });
    assert.equal(healed.id, first.id);
    assert.equal(healed.tmuxTarget, `ch_${s.id}_${first.id}`);

    ws.close();
    await cleanup(dbPath);
});

// THE regression this whole scheme exists to remove, at the storage layer.
// Closing a terminal pane deliberately leaves its row and its remote tmux
// session alive, so the slot LABEL it wore is free in the layout while the old
// shell is still running. The next split — on this client or another one, whose
// label counter has never heard of the closed pane — legitimately asks for that
// same label and wants a NEW terminal. v3 refused that insert, because it read
// the label as an address. v4 removed the rule: identity is the row id the
// layout leaf carries, and duplicate labels within one Dev Session are normal.
test("two terminal panes of one Dev Session may share a slot label", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });
    assert.deepEqual(uniqueAddressIndexes(ws), []);

    const closed = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-2" });
    const fresh = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-2" });
    // Different rows, different shells. This is the acceptance criterion: a
    // pane created after another pane with the same label was closed must never
    // land on the closed pane's tmux session.
    assert.notEqual(fresh.id, closed.id);
    assert.notEqual(fresh.tmuxTarget, closed.tmuxTarget);
    // Both rows remain enumerable, so the closed pane's work is recoverable
    // rather than lost.
    const panes = ws.list(SERVER)[0].sessions[0].terminalPanes;
    assert.deepEqual(panes.map((p) => p.id).sort(), [closed.id, fresh.id].sort());

    // Addressing BY ROW ID keeps the two apart, which is the only addressing a
    // current client uses.
    assert.equal(
        ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, id: fresh.id }).tmuxTarget,
        fresh.tmuxTarget,
    );
    // The legacy by-label path is deterministic under duplicates: the OLDEST
    // row wins, because a leaf with no row id predates every later mint.
    assert.equal(
        ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-2" }).id,
        closed.id,
    );
    // Renaming a pane onto a taken label is allowed for the same reason.
    // Nothing in the client renames a terminal pane today.
    const other = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-9" });
    assert.equal(ws.updateTerminalPane({ id: other.id, name: "terminal-2" }).name, "terminal-2");

    ws.close();
    await cleanup(dbPath);
});

// Resolving by row id is a pure LOOKUP: it never creates. A leaf naming a row
// that is gone must be reported, not silently handed a different terminal, and
// a row belonging to another Dev Session is not this leaf's to attach.
test("resolveTerminalPane by id never creates and is scoped to the Dev Session", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });
    const other = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S2", repositoryRoot: "/r" });
    const t = ws.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-1" });

    assert.equal(ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, id: t.id }).id, t.id);
    assert.throws(
        () => ws.resolveTerminalPane({ serverId: SERVER, devSessionId: other.id, id: t.id }),
        /no terminal pane/,
    );
    assert.throws(
        () => ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, id: "does-not-exist" }),
        /no terminal pane/,
    );
    assert.equal(ws.list(SERVER)[0].sessions[0].terminalPanes.length, 1);

    // Neither addressing mode, or both, is a caller bug rather than a guess.
    assert.throws(
        () => ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id }),
        /exactly one of/,
    );
    assert.throws(
        () => ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, id: t.id, name: "terminal-1" }),
        /exactly one of/,
    );

    // A row found without a tmux target still gets one minted, on this path too.
    ws.updateTerminalPane({ id: t.id, tmuxTarget: null });
    assert.equal(
        ws.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, id: t.id }).tmuxTarget,
        `ch_${s.id}_${t.id}`,
    );

    ws.close();
    await cleanup(dbPath);
});

// v3 de-duplicated slot labels before constraining them, and v4 then dropped
// the constraint again, which makes the renames look like pure data damage in
// hindsight. They are not, and this is re-verified: phase 4 of v3 creates
// UNIQUE (dev_session_id, name), that statement fails outright on a database
// still holding two rows for one slot, and both steps run inside the migration
// runner's ONE transaction — so a v2 database with duplicate labels would roll
// the whole upgrade back and never open. The repair is what a v2 database gets,
// and it is pinned here even though the index it was preparing for no longer
// exists.
test("the v3 migration de-duplicates slot names, and v4 drops the index again", async () => {
    const dbPath = await tmpDbPath();
    const legacy = openWorkspace(dbPath);
    const g = legacy.createGroup({ serverId: SERVER, name: "G" });
    const s = legacy.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });
    const a = legacy.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "a" });
    const b = legacy.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "b" });
    const c = legacy.createTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "c" });
    downgradeToV2(legacy);
    // A v2 database could hold two rows for one layout slot, because nothing
    // stopped two clients from both creating one.
    legacy.db.prepare("UPDATE terminal_panes SET name = ? WHERE id = ?").run("terminal-1", a.id);
    legacy.db.prepare("UPDATE terminal_panes SET name = ? WHERE id = ?").run("terminal-1", b.id);
    legacy.db.prepare("UPDATE terminal_panes SET name = ? WHERE id = ?").run("terminal-1", c.id);
    assert.deepEqual(uniqueAddressIndexes(legacy), []);
    legacy.close();

    const upgraded = openWorkspace(dbPath);
    // v4 removed the address index; only the tmux_target rule is left.
    assert.deepEqual(uniqueAddressIndexes(upgraded), []);

    // The OLDEST row keeps the contested slot: it is the one whose shell has
    // been running longest, so it is the one the slot should keep resolving to.
    assert.equal(upgraded.getTerminalPane(a.id).name, "terminal-1");
    // The later rows are RENAMED, not deleted: each still owns a live tmux
    // session and the user's running processes, and deleting the row would
    // strand them under a name nothing could ever look up again.
    assert.equal(upgraded.getTerminalPane(b.id).name, `terminal-1-dup-${b.id.slice(0, 8)}`);
    assert.equal(upgraded.getTerminalPane(c.id).name, `terminal-1-dup-${c.id.slice(0, 8)}`);
    // Every row survived, and each kept its own tmux session.
    const panes = upgraded.list(SERVER)[0].sessions[0].terminalPanes;
    assert.equal(panes.length, 3);
    assert.equal(new Set(panes.map((p) => p.tmuxTarget)).size, 3);
    // The slot now resolves to exactly one row — the survivor.
    assert.equal(
        upgraded.resolveTerminalPane({ serverId: SERVER, devSessionId: s.id, name: "terminal-1" }).id,
        a.id,
    );
    upgraded.close();

    await cleanup(dbPath);
});

// A v3 database in the shape v4 has to REBUILD: the address rule arrived as a
// table-level UNIQUE, i.e. an sqlite_autoindex, which SQLite cannot drop.
// Rewind the stored version so the next open runs v4 for real.
function downgradeToV3Autoindex(ws: Workspace): void {
    ws.db.exec(`
        ALTER TABLE terminal_panes RENAME TO terminal_panes_v3_tmp;
        CREATE TABLE terminal_panes (
            id                TEXT    NOT NULL PRIMARY KEY,
            server_id         TEXT    NOT NULL,
            dev_session_id    TEXT    NOT NULL,
            name              TEXT    NOT NULL,
            working_directory TEXT,
            tmux_target       TEXT,
            startup_command   TEXT,
            harness           TEXT,
            position          INTEGER NOT NULL DEFAULT 0,
            created_at        INTEGER NOT NULL,
            updated_at        INTEGER NOT NULL,
            FOREIGN KEY (dev_session_id) REFERENCES dev_sessions (id),
            UNIQUE (tmux_target),
            UNIQUE (dev_session_id, name)
        );
        INSERT INTO terminal_panes SELECT * FROM terminal_panes_v3_tmp;
        DROP TABLE terminal_panes_v3_tmp;
    `);
    ws.db.prepare("UPDATE schema_version SET version = 3 WHERE id = 1").run();
}

// A real database can hold a terminal pane whose Dev Session is gone: SQLite
// checks a foreign key when the row is WRITTEN and never re-checks it, so
// however the orphan arrived (a delete by an older build, a hand-edit, a
// half-restored file), the daemon opens that database and serves it happily.
// v4 then has to REBUILD terminal_panes, and the copy re-checks every key —
// which used to fail the INSERT, roll the whole upgrade back and make the
// database permanently unopenable by this build. What is at stake is not the
// stale row: it is every OTHER row, because one orphaned pane took the user's
// groups, sessions, panes and layouts with it.
test("the v4 rebuild migrates a legacy database that contains an orphaned pane row", async () => {
    const dbPath = await tmpDbPath();
    const legacy = openWorkspace(dbPath);
    const { group, session, terminal } = seed(legacy);
    downgradeToV3Autoindex(legacy);
    // Plant the orphan the way a real one exists on disk: unchecked, because
    // enforcement only ever applied at the moment of the write.
    legacy.db.exec("PRAGMA foreign_keys = OFF");
    legacy.db
        .prepare(
            "INSERT INTO terminal_panes (id, server_id, dev_session_id, name, tmux_target, position, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        )
        .run("orphan-pane", SERVER, "dev-session-that-is-gone", "terminal-9", "ch_orphan", 0, 1, 1);
    legacy.db.exec("PRAGMA foreign_keys = ON");
    legacy.close();

    // The whole point: this open must SUCCEED.
    const upgraded = openWorkspace(dbPath);
    const version = upgraded.db
        .prepare("SELECT version FROM schema_version WHERE id = 1")
        .get() as { version: number };
    assert.equal(version.version, WORKSPACE_SCHEMA_VERSION);
    // v4 did its actual job on the way through: the address rule is gone and
    // the tmux_target rule was re-declared on the rebuilt table.
    assert.deepEqual(uniqueAddressIndexes(upgraded), []);
    assert.equal(uniqueTmuxTargetIndexes(upgraded).length, 1);
    // Nothing was deleted or re-parented on the user's behalf. The orphan comes
    // through the rebuild exactly as it came through every open before it.
    const orphan = upgraded.db
        .prepare("SELECT dev_session_id, name FROM terminal_panes WHERE id = 'orphan-pane'")
        .get() as { dev_session_id: string; name: string } | undefined;
    assert.equal(orphan?.dev_session_id, "dev-session-that-is-gone");
    assert.equal(orphan?.name, "terminal-9");
    // And the reachable workspace still reads back whole.
    const groups = upgraded.list(SERVER);
    assert.equal(groups.length, 1);
    assert.equal(groups[0].id, group.id);
    assert.equal(groups[0].sessions[0].id, session.id);
    assert.deepEqual(
        groups[0].sessions[0].terminalPanes.map((p) => p.id),
        [terminal.id],
    );
    // Enforcement is off for the upgrade and only for the upgrade: an ordinary
    // write that would create a NEW orphan is still rejected.
    assert.throws(
        () =>
            upgraded.db
                .prepare(
                    "INSERT INTO terminal_panes (id, server_id, dev_session_id, name, position, created_at, updated_at) VALUES ('x', 's', 'nope', 'n', 0, 1, 1)",
                )
                .run(),
        /FOREIGN KEY/,
    );
    upgraded.close();

    await cleanup(dbPath);
});

// v4 finds the index it must drop by NAME, read back out of the catalogue, and
// then interpolates that name into two further statements. A name is a SQL
// IDENTIFIER: an embedded double quote is doubled. It used to be JSON-escaped,
// which emits a backslash SQL has no idea what to do with — so a database
// carrying such an index did not merely mis-handle it, it failed to open.
test("the v4 migration drops an address index whose name contains a double quote", async () => {
    const dbPath = await tmpDbPath();
    const legacy = openWorkspace(dbPath);
    const { session, terminal } = seed(legacy);
    downgradeToV2(legacy);
    legacy.db.exec(
        'CREATE UNIQUE INDEX "odd""name_unique" ON terminal_panes (dev_session_id, name)',
    );
    // Stored at v3, so the next open runs v4 and nothing else.
    legacy.db.prepare("UPDATE schema_version SET version = 3 WHERE id = 1").run();
    assert.deepEqual(uniqueAddressIndexes(legacy), ['odd"name_unique']);
    legacy.close();

    const upgraded = openWorkspace(dbPath);
    assert.deepEqual(uniqueAddressIndexes(upgraded), []);
    // Dropped by name rather than by rebuilding the table, so the rows are
    // untouched.
    assert.deepEqual(
        upgraded.list(SERVER)[0].sessions[0].terminalPanes.map((p) => p.id),
        [terminal.id],
    );
    assert.equal(upgraded.getTerminalPane(terminal.id).devSessionId, session.id);
    upgraded.close();

    await cleanup(dbPath);
});

// THE race this method exists for, driven against two REAL codeharbord
// processes sharing one database file — which is exactly the production shape:
// each client's SSH session spawns its own daemon, and each daemon opens its
// own SQLite connection. Two clients opening the same Dev Session at the same
// moment used to each list, each see no row for "terminal-1", and each create
// one: two rows, two server-minted targets, two tmux sessions for one pane.
//
// The requests are written to both daemons before either answer is read, so the
// two are genuinely in flight together; transaction()'s BEGIN IMMEDIATE takes
// the write lock before the SELECT, so the loser reads after the winner's
// commit and finds the row instead of inserting a second one.
test("two concurrent daemons resolving one slot converge on one row", async () => {
    const dbPath = await tmpDbPath();
    const seed = openWorkspace(dbPath);
    const g = seed.createGroup({ serverId: SERVER, name: "G" });
    const s = seed.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });
    seed.close();

    const daemon = fileURLToPath(new URL("../src/codeharbord.ts", import.meta.url));
    const spawnDaemon = () =>
        spawn(process.execPath, [daemon, "rpc", "--stdio"], {
            env: { ...process.env, CODEHARBOR_DB: dbPath },
            stdio: ["pipe", "pipe", "inherit"],
        });

    // One request per daemon, both written before either reply is awaited.
    const request =
        JSON.stringify({
            jsonrpc: "2.0",
            id: 1,
            method: "workspace.resolveTerminalPane",
            params: { serverId: SERVER, devSessionId: s.id, name: "terminal-1" },
        }) + "\n";

    const daemons = [spawnDaemon(), spawnDaemon()];
    const replies = daemons.map(
        (proc) =>
            new Promise<Record<string, unknown>>((resolve, reject) => {
                let buffered = "";
                proc.stdout.setEncoding("utf8");
                proc.stdout.on("data", (chunk: string) => {
                    buffered += chunk;
                    const end = buffered.indexOf("\n");
                    if (end >= 0) resolve(JSON.parse(buffered.slice(0, end)));
                });
                proc.on("error", reject);
                proc.on("exit", () => reject(new Error("daemon exited before answering")));
            }),
    );
    for (const proc of daemons) proc.stdin.write(request);

    const answers = await Promise.all(replies);
    for (const proc of daemons) {
        proc.stdin.end();
        proc.kill();
    }

    // Neither client was told to retry, and neither got an error.
    for (const answer of answers) {
        assert.equal(answer.error, undefined, JSON.stringify(answer.error));
    }
    const panes = answers.map((a) => a.result as { id: string; tmuxTarget: string });
    // ONE row, ONE tmux session — both clients are pointed at the same terminal.
    assert.equal(panes[0].id, panes[1].id);
    assert.equal(panes[0].tmuxTarget, panes[1].tmuxTarget);

    // And the database agrees: the slot has exactly one row, not two.
    const after = openWorkspace(dbPath);
    const rows = after.list(SERVER)[0].sessions[0].terminalPanes;
    assert.deepEqual(
        rows.map((p) => p.name),
        ["terminal-1"],
    );
    assert.equal(rows[0].tmuxTarget, panes[0].tmuxTarget);
    after.close();

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

// "Create at position N" is a placement REQUEST, not a raw column value.
// Writing N straight into the new row's `position` left it TIED with whichever
// row already held N, and every listing orders by (position, id) — so the tie
// broke by UUID and the row the caller placed landed above or below its
// neighbour at random, and stayed tied for every later read. moveSession was
// fixed for exactly this; creation had the same hole, on all four ordered
// tables.
test("creating a row at an explicit position re-packs its scope instead of tying", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);

    const first = ws.createGroup({ serverId: SERVER, name: "A" });
    const last = ws.createGroup({ serverId: SERVER, name: "B" });
    const mid = ws.createGroup({ serverId: SERVER, name: "M", position: 1 });
    assert.deepEqual(
        ws.list(SERVER).map((g) => g.id),
        [first.id, mid.id, last.id],
    );
    // Packed 0..n-1, so no two rows share a position and nothing depends on the
    // id tie-break.
    assert.deepEqual(
        ws.list(SERVER).map((g) => g.position),
        [0, 1, 2],
    );
    // A position past the end appends — the only sensible reading of "create at
    // position 99" in a scope of three — rather than leaving a gap behind.
    const appended = ws.createGroup({ serverId: SERVER, name: "Z", position: 99 });
    assert.equal(ws.getGroup(appended.id).position, 3);

    // The nested scopes have their own position column and the same hole.
    const s1 = mkSession(ws, first.id, "S1");
    const s2 = mkSession(ws, first.id, "S2");
    const s0 = ws.createSession({
        serverId: SERVER,
        groupId: first.id,
        name: "S0",
        repositoryRoot: "/r",
        position: 0,
    });
    assert.deepEqual(
        ws.list(SERVER)[0].sessions.map((s) => s.id),
        [s0.id, s1.id, s2.id],
    );

    const v1 = ws.createViewerPane({ serverId: SERVER, devSessionId: s0.id, url: "u1" });
    const v0 = ws.createViewerPane({
        serverId: SERVER,
        devSessionId: s0.id,
        url: "u0",
        position: 0,
    });
    const t1 = ws.createTerminalPane({ serverId: SERVER, devSessionId: s0.id, name: "t1" });
    const t0 = ws.createTerminalPane({
        serverId: SERVER,
        devSessionId: s0.id,
        name: "t0",
        position: 0,
    });
    const node = ws.list(SERVER)[0].sessions[0];
    assert.deepEqual(
        node.viewerPanes.map((p) => p.id),
        [v0.id, v1.id],
    );
    assert.deepEqual(
        node.terminalPanes.map((p) => p.id),
        [t0.id, t1.id],
    );
    assert.deepEqual(
        node.terminalPanes.map((p) => p.position),
        [0, 1],
    );

    ws.close();
    await cleanup(dbPath);
});

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

// The region names a column with a CHECK constraint. An unlisted value used to
// travel all the way to SQLite and come back as a raw constraint-violation
// message that named neither the field nor the two values it accepts.
test("RW15: the layout handlers reject a region outside viewer/terminal", () => {
    assert.throws(
        () =>
            WORKSPACE_METHODS["workspace.setLayout"]({
                serverId: SERVER,
                devSessionId: "s",
                region: "sidebar",
                tree: { type: "leaf", paneId: "" },
            }),
        /workspace\.setLayout: field 'region' must be one of viewer, terminal/,
    );
    assert.throws(
        () => WORKSPACE_METHODS["workspace.getLayout"]({ devSessionId: "s", region: "" }),
        /workspace\.getLayout: field 'region' must be one of viewer, terminal/,
    );
});

// An omitted tree stringifies to `undefined`, which the SQLite driver rejects
// with an opaque type error naming no field at all.
test("RW15: setLayout rejects a params object with no tree", () => {
    assert.throws(
        () =>
            WORKSPACE_METHODS["workspace.setLayout"]({
                serverId: SERVER,
                devSessionId: "s",
                region: "viewer",
            }),
        /workspace\.setLayout: missing field 'tree'/,
    );
});

// `null` is not "leave it alone". The nullable fields genuinely mean "clear
// this" (updateSession taskDescription, update*Pane title/harness/…), but the
// columns that cannot be null — a group's name, a session's repositoryRoot, a
// viewer pane's url, a terminal pane's slot label — used to accept a null,
// discard it through the store's `?? current` fallback, and answer the caller
// with a success and an unchanged row. A caller that meant to rename something
// was told it had.
test("RW15: a null is rejected for the fields that cannot be cleared", () => {
    for (const [method, params, field] of [
        ["workspace.updateGroup", { id: "g", name: null }, "name"],
        ["workspace.updateSession", { id: "s", name: null }, "name"],
        ["workspace.updateSession", { id: "s", repositoryRoot: null }, "repositoryRoot"],
        ["workspace.updateViewerPane", { id: "v", url: null }, "url"],
        ["workspace.updateTerminalPane", { id: "t", name: null }, "name"],
    ] as const) {
        assert.throws(
            () => WORKSPACE_METHODS[method](params),
            new RegExp(`${method.replace(".", "\\.")}: missing or invalid field '${field}'`),
            `${method}.${field} accepted a null`,
        );
    }
});

// `position` on a create/update is written STRAIGHT into the ordering column
// that every other path (packOrder) keeps at contiguous integers 0..n-1.
// SQLite stores -5 and 2.5 verbatim in an INTEGER-affinity column, so one such
// call left the scope ordered around a negative or fractional slot for ever
// after, and nextPosition then handed the following row 3.5.
test("RW15: a fractional or negative position is rejected by every create/update", () => {
    const cases: Array<[keyof typeof WORKSPACE_METHODS, Record<string, unknown>]> = [
        ["workspace.createGroup", { serverId: SERVER, name: "G" }],
        ["workspace.updateGroup", { id: "g" }],
        ["workspace.createSession", { serverId: SERVER, groupId: "g", name: "S", repositoryRoot: "/r" }],
        ["workspace.updateSession", { id: "s" }],
        ["workspace.createViewerPane", { serverId: SERVER, devSessionId: "s", url: "u" }],
        ["workspace.updateViewerPane", { id: "v" }],
        ["workspace.createTerminalPane", { serverId: SERVER, devSessionId: "s", name: "t" }],
        ["workspace.updateTerminalPane", { id: "t" }],
    ];
    for (const [method, base] of cases) {
        for (const position of [2.5, -1, Number.NaN]) {
            assert.throws(
                () => WORKSPACE_METHODS[method]({ ...base, position }),
                /field 'position' must be a non-negative integer/,
                `${method} accepted position ${position}`,
            );
        }
    }
});

// Every create is a read-then-write (read the scope's highest position, then
// insert at one past it) and now runs inside a transaction, so two connections
// cannot both claim the same position and leave the ordering to be broken by
// UUID. What is observable from outside is the rollback contract: a failed
// create leaves no row AND leaves the connection usable, i.e. the wrapper
// really did roll its transaction back instead of leaving one open — a leaked
// open transaction would break every later write on the connection.
test("a failed create rolls back and leaves the connection usable", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const group = ws.createGroup({ serverId: SERVER, name: "G" });
    const session = ws.createSession({
        serverId: SERVER,
        groupId: group.id,
        name: "S",
        repositoryRoot: "/r",
    });

    ws.db.exec(
        "CREATE TRIGGER fail_viewer_insert BEFORE INSERT ON viewer_panes " +
            "BEGIN SELECT RAISE(ABORT, 'injected insert failure'); END",
    );
    assert.throws(() =>
        ws.createViewerPane({ serverId: SERVER, devSessionId: session.id, url: "a" }),
    );
    ws.db.exec("DROP TRIGGER fail_viewer_insert");

    // Nothing was written, and the very next write succeeds.
    assert.equal(ws.list(SERVER)[0].sessions[0].viewerPanes.length, 0);
    ws.createViewerPane({ serverId: SERVER, devSessionId: session.id, url: "a" });
    ws.createViewerPane({ serverId: SERVER, devSessionId: session.id, url: "b" });
    ws.createTerminalPane({ serverId: SERVER, devSessionId: session.id, name: "sh" });

    // Positions stay contiguous from 0 across the failure.
    const node = ws.list(SERVER)[0].sessions[0];
    assert.deepEqual(
        node.viewerPanes.map((p) => p.position),
        [0, 1],
    );
    assert.deepEqual(
        node.terminalPanes.map((p) => p.position),
        [0],
    );

    ws.close();
    await cleanup(dbPath);
});

// openWorkspace opens the connection BEFORE it migrates. A database written by
// a newer build is refused — and the connection it already opened must be
// closed, or it leaks a descriptor and keeps SQLite's lock on the file, so the
// next attempt fails for a second, unrelated reason.
test("openWorkspace closes the connection when it refuses a newer database", async () => {
    const dbPath = await tmpDbPath();
    const first = openWorkspace(dbPath);
    first.db.prepare("UPDATE schema_version SET version = ? WHERE id = 1").run(
        WORKSPACE_SCHEMA_VERSION + 1,
    );
    first.close();

    assert.throws(() => openWorkspace(dbPath), /newer than this build supports/);

    // Proof the refused connection was released: put the version back through a
    // fresh connection and reopen normally. A still-open handle from the failed
    // attempt would have left the file locked.
    const repair = new DatabaseSync(dbPath);
    // Every connection to a workspace database gets the same treatment, tests
    // included: without the busy timeout this one write would fail outright if
    // anything else held the lock.
    applyConnectionPragmas(repair);
    repair.prepare("UPDATE schema_version SET version = ? WHERE id = 1").run(
        WORKSPACE_SCHEMA_VERSION,
    );
    repair.close();
    const reopened = openWorkspace(dbPath);
    assert.equal(typeof reopened.serverId(), "string");
    reopened.close();

    await cleanup(dbPath);
});

// Contention that the BEGIN IMMEDIATE work assumed but nothing enforced: two
// daemon processes writing to one database file at the same time. SQLite's
// default busy timeout is ZERO, so before applyConnectionPragmas() the loser of
// every collision failed instantly and the client was told the SERVER broke.
//
// Each daemon is handed a whole batch of createGroup requests before either
// daemon's first reply is read, so the two are writing simultaneously for the
// whole run rather than trading one request each.
test("two daemons writing at once all succeed, and positions stay packed", async () => {
    const dbPath = await tmpDbPath();
    openWorkspace(dbPath).close();

    const daemonPath = fileURLToPath(new URL("../src/codeharbord.ts", import.meta.url));
    const PER_DAEMON = 25;

    // A daemon plus a reader that lets the test wait for the Nth reply. Both
    // daemons must be up before either starts writing: spawning is much slower
    // than a row insert, so an unsynchronized pair can finish its whole burst
    // before the other has opened the database and never contend at all.
    const daemons = [0, 1].map((n) => {
        const proc = spawn(process.execPath, [daemonPath, "rpc", "--stdio"], {
            env: { ...process.env, CODEHARBOR_DB: dbPath },
            stdio: ["pipe", "pipe", "inherit"],
        });
        const replies: Record<string, unknown>[] = [];
        const waiters: Array<{ want: number; settle: () => void }> = [];
        let failure: Error | undefined;
        let buffered = "";
        const wake = () => {
            for (let i = waiters.length - 1; i >= 0; i--) {
                if (failure || replies.length >= waiters[i].want) waiters.splice(i, 1)[0].settle();
            }
        };
        proc.stdout.setEncoding("utf8");
        proc.stdout.on("data", (chunk: string) => {
            buffered += chunk;
            for (let end = buffered.indexOf("\n"); end >= 0; end = buffered.indexOf("\n")) {
                replies.push(JSON.parse(buffered.slice(0, end)));
                buffered = buffered.slice(end + 1);
            }
            wake();
        });
        const fail = () => {
            failure = new Error(`daemon ${n} exited before answering`);
            wake();
        };
        proc.on("error", fail);
        proc.on("exit", fail);
        const until = (want: number) =>
            new Promise<void>((resolve, reject) => {
                waiters.push({ want, settle: () => (failure ? reject(failure) : resolve()) });
                wake();
            });
        return { proc, replies, until, tag: `d${n}` };
    });

    // The barrier: `ping` needs no database, so answering it proves only that
    // the process is running and reading its stdin — exactly what we need
    // before releasing the writes.
    for (const d of daemons) {
        d.proc.stdin.write(JSON.stringify({ jsonrpc: "2.0", id: 0, method: "ping" }) + "\n");
    }
    await Promise.all(daemons.map((d) => d.until(1)));

    // Both bursts released without awaiting anything in between, so each
    // daemon's whole batch is queued while the other is still writing.
    for (const d of daemons) {
        let batch = "";
        for (let i = 1; i <= PER_DAEMON; i++) {
            batch +=
                JSON.stringify({
                    jsonrpc: "2.0",
                    id: i,
                    method: "workspace.createGroup",
                    params: { serverId: SERVER, name: `${d.tag}-${i}` },
                }) + "\n";
        }
        d.proc.stdin.write(batch);
    }
    await Promise.all(daemons.map((d) => d.until(PER_DAEMON + 1)));

    const answers = daemons.flatMap((d) => d.replies.slice(1));
    for (const d of daemons) {
        d.proc.stdin.end();
        d.proc.kill();
    }

    // Not one of the fifty writes was refused. A "database is locked" here is
    // the whole defect: valid work rejected because the loser did not wait.
    for (const answer of answers) {
        assert.equal(answer.error, undefined, JSON.stringify(answer.error));
    }

    // Every write landed, and the read-then-write inside createGroup stayed
    // serialized: positions are exactly 0..49 with no duplicate and no hole, so
    // no two concurrent inserts read the same maximum. A duplicate position is
    // an ordering the user sees as groups shuffling between listings.
    const after = openWorkspace(dbPath);
    const groups = after.list(SERVER);
    assert.equal(groups.length, 2 * PER_DAEMON);
    const positions = after.db
        .prepare("SELECT position FROM groups WHERE server_id = ? ORDER BY position")
        .all(SERVER) as unknown as Array<{ position: number }>;
    assert.deepEqual(
        positions.map((row) => row.position),
        Array.from({ length: 2 * PER_DAEMON }, (_, i) => i),
    );
    // And the listing order agrees with those positions.
    assert.deepEqual(
        groups.map((g) => g.position),
        Array.from({ length: 2 * PER_DAEMON }, (_, i) => i),
    );
    after.close();

    await cleanup(dbPath);
});

// The two settings applyConnectionPragmas() exists for, asserted on the real
// thing. WAL is the reason concurrent readers and the writer stop blocking each
// other at all; the busy timeout is the fallback for the write lock they still
// share.
test("a file-backed workspace runs in WAL with a non-zero busy timeout", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const mode = ws.db.prepare("PRAGMA journal_mode").get() as unknown as {
        journal_mode: string;
    };
    assert.equal(mode.journal_mode, "wal");
    const busy = ws.db.prepare("PRAGMA busy_timeout").get() as unknown as { timeout: number };
    assert.ok(busy.timeout > 0, `busy_timeout is ${busy.timeout}; SQLite's zero default never waits`);
    ws.close();

    // The journal mode is recorded in the FILE, so reopening an existing
    // database must find it already set and everything written before the
    // switch must still be readable — a mode change is not a migration and must
    // not disturb the schema version or the rows.
    const seeded = openWorkspace(dbPath);
    const g = seeded.createGroup({ serverId: SERVER, name: "G" });
    seeded.close();
    const reopened = openWorkspace(dbPath);
    const remode = reopened.db.prepare("PRAGMA journal_mode").get() as unknown as {
        journal_mode: string;
    };
    assert.equal(remode.journal_mode, "wal");
    assert.equal(reopened.list(SERVER)[0].id, g.id);
    const version = reopened.db
        .prepare("SELECT version FROM schema_version WHERE id = 1")
        .get() as unknown as { version: number };
    assert.equal(version.version, WORKSPACE_SCHEMA_VERSION);
    reopened.close();

    await cleanup(dbPath);
});

// WAL does not exist for an in-memory database and SQLite quietly ignores the
// request there. Several tests (and schema.test.ts) open ":memory:", so this
// pins that adopting WAL cannot make them throw or drop into an unexpected
// mode — while the busy timeout, which IS meaningful everywhere, still applies.
test("an in-memory workspace is unaffected by the WAL pragma", () => {
    const ws = openWorkspace(":memory:");
    const mode = ws.db.prepare("PRAGMA journal_mode").get() as unknown as {
        journal_mode: string;
    };
    assert.equal(mode.journal_mode, "memory");
    const busy = ws.db.prepare("PRAGMA busy_timeout").get() as unknown as { timeout: number };
    assert.ok(busy.timeout > 0);
    // Still a working database, which is the point of the graceful path.
    assert.equal(ws.createGroup({ serverId: SERVER, name: "G" }).position, 0);
    ws.close();
});

// The predicate the dispatcher branches on to answer RPC_DATABASE_BUSY instead
// of -32603. It reads SQLite's numeric result code rather than matching on the
// message text, so this drives a REAL contention failure rather than a
// hand-built Error: a fabricated one could keep passing after node:sqlite
// changed how it reports the failure, which is exactly the drift that would put
// "internal error" back in front of users.
test("isDatabaseBusy recognizes a real lock failure and nothing else", async () => {
    const dbPath = await tmpDbPath();
    const holder = openWorkspace(dbPath);
    const loser = new DatabaseSync(dbPath);
    // No busy timeout on this one: it must fail instantly instead of waiting
    // out a lock the single-threaded test will never release in the meantime.
    loser.exec("PRAGMA busy_timeout = 0;");
    holder.db.exec("BEGIN IMMEDIATE");
    assert.throws(
        () => loser.exec("BEGIN IMMEDIATE"),
        (err: unknown) => {
            assert.ok(isDatabaseBusy(err), `not recognized as contention: ${String(err)}`);
            return true;
        },
    );
    holder.db.exec("ROLLBACK");
    loser.close();

    // A constraint violation is a genuine rejection of the request and must NOT
    // be dressed up as "retry, someone else is writing".
    const g = holder.createGroup({ serverId: SERVER, name: "G" });
    assert.throws(
        () =>
            holder.db
                .prepare("INSERT INTO groups (id, server_id, name, position, collapsed, created_at, updated_at) VALUES (?, ?, ?, ?, 0, 0, 0)")
                .run(g.id, SERVER, "dup", 1),
        (err: unknown) => isDatabaseBusy(err) === false,
    );
    assert.equal(isDatabaseBusy(new Error("database is locked")), false);
    holder.close();

    await cleanup(dbPath);
});

// A split tree is authored by the CLIENT, so its pane ids are arbitrary
// strings. duplicateSession looked each one up in a plain object literal, and
// a leaf whose paneId was "constructor" or "toString" therefore resolved to a
// FUNCTION inherited from Object.prototype — which JSON.stringify drops, so
// the copied leaf came out with no paneId at all and the region rendered
// empty. The lookup is a Map now, which has no inherited keys.
test("duplicateSession leaves a paneId named after an Object.prototype member alone", async () => {
    const dbPath = await tmpDbPath();
    const ws = openWorkspace(dbPath);
    const g = ws.createGroup({ serverId: SERVER, name: "G" });
    const s = ws.createSession({ serverId: SERVER, groupId: g.id, name: "S", repositoryRoot: "/r" });
    const real = ws.createViewerPane({ serverId: SERVER, devSessionId: s.id, url: "u" });
    ws.setLayout({
        serverId: SERVER,
        devSessionId: s.id,
        region: "viewer",
        tree: {
            type: "split",
            orientation: "horizontal",
            ratios: [0.5, 0.5],
            children: [
                { type: "leaf", paneId: "constructor" },
                { type: "leaf", paneId: real.id, terminalPaneId: "toString" },
            ],
        },
    });

    const copy = ws.duplicateSession({ id: s.id });
    const tree = copy.layouts.viewer as { children: Array<Record<string, unknown>> };
    // The unmapped id survives verbatim rather than vanishing...
    assert.equal(tree.children[0].paneId, "constructor");
    // ...the mapped one is still remapped to the copied pane...
    assert.equal(tree.children[1].paneId, copy.viewerPanes[0].id);
    assert.notEqual(copy.viewerPanes[0].id, real.id);
    // ...and an unmapped terminalPaneId is dropped, not turned into a function.
    assert.equal("terminalPaneId" in tree.children[1], false);

    ws.close();
    await cleanup(dbPath);
});

// schemaVersion() reads the stored version and answers 0 when the table is not
// there yet. It used to answer 0 for EVERY failure, so a database this build
// cannot read at all was mistaken for an unmigrated one: migrate() went on to
// BEGIN IMMEDIATE and the full schema.sql, and the operator was shown whichever
// of those failed second instead of the actual defect.
test("openWorkspace reports why an unreadable database failed, not a downstream error", async () => {
    const dbPath = await tmpDbPath();
    const broken = new DatabaseSync(dbPath);
    applyConnectionPragmas(broken);
    // A schema_version table whose shape this build does not understand: the
    // version SELECT fails on the column, not on the table.
    broken.exec("CREATE TABLE schema_version (id INTEGER NOT NULL PRIMARY KEY, v INTEGER NOT NULL)");
    broken.close();

    assert.throws(() => openWorkspace(dbPath), /no such column: version/);

    await cleanup(dbPath);
});