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
