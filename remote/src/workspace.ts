// Workstream P — server-side workspace persistence (SPEC 4.2, 11.1). Owns the
// authoritative SQLite workspace database on the codeharbord host: opens the
// DB, runs the idempotent schema.sql migration (C2), and provides CRUD over
// groups, dev sessions, viewer/terminal panes, and per-region split layouts. It
// also implements duplicate-session copy semantics (SPEC 4.2): every copied row
// gets a fresh UUID, and every copied terminal pane gets a fresh tmux target.
//
// WORKSPACE_METHODS exposes the `workspace.*` RPC group. This is P's OWN method
// group — deliberately NOT part of the frozen six-method C1 file catalog
// (RPC_METHODS in rpc-types.ts, which stays exactly the file.* methods). Its
// wire names live in RPC_WORKSPACE_METHODS in rpc-types.ts, mirrored in C++ at
// src/remote/RpcTypes.h.

import { DatabaseSync } from "node:sqlite";
import { randomUUID } from "node:crypto";
import { mkdirSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import os from "node:os";
import path from "node:path";

import { RPC_WORKSPACE_METHODS as M } from "./rpc-types.ts";
import type { RpcWorkspaceMethodName } from "./rpc-types.ts";

// Current schema version. Mirrors schema_version in remote/sql/schema.sql and
// WorkspaceDb::kSchemaVersion (bump all three together — see schema.sql header).
// Bumped 1 -> 2 for the server_identity singleton (SPEC 3.5).
export const WORKSPACE_SCHEMA_VERSION = 2;

// The authoritative DDL (C2). Read relative to this module so it resolves the
// same whether invoked from src/ or from a built dist/ alongside sql/.
const schemaSql = readFileSync(
    fileURLToPath(new URL("../sql/schema.sql", import.meta.url)),
    "utf8",
);

// Lookup indexes, applied on EVERY open rather than by the migration runner.
// They carry no data and every statement is `CREATE INDEX IF NOT EXISTS`, so
// re-running them is a no-op; that is precisely what lets an existing database
// at the current schema version gain a newly added index without a version bump
// (which would have to be mirrored in C++ — see indexes.sql for the full
// reasoning).
const indexesSql = readFileSync(
    fileURLToPath(new URL("../sql/indexes.sql", import.meta.url)),
    "utf8",
);

// --- Public row shapes (camelCase mirror of the snake_case schema columns) ---

export interface Group {
    id: string;
    serverId: string;
    name: string;
    position: number;
    collapsed: boolean;
    createdAt: number;
    updatedAt: number;
}

export interface Session {
    id: string;
    serverId: string;
    groupId: string;
    name: string;
    repositoryRoot: string;
    defaultWorkingDirectory: string | null;
    taskDescription: string | null;
    position: number;
    archived: boolean;
    createdAt: number;
    updatedAt: number;
}

export interface ViewerPane {
    id: string;
    serverId: string;
    devSessionId: string;
    url: string;
    handler: string | null;
    title: string | null;
    position: number;
    createdAt: number;
    updatedAt: number;
}

export interface TerminalPane {
    id: string;
    serverId: string;
    devSessionId: string;
    name: string;
    workingDirectory: string | null;
    tmuxTarget: string | null;
    startupCommand: string | null;
    harness: string | null;
    position: number;
    createdAt: number;
    updatedAt: number;
}

export type Region = "viewer" | "terminal";

export interface SessionLayout {
    id: string;
    serverId: string;
    devSessionId: string;
    region: Region;
    tree: unknown;
    createdAt: number;
    updatedAt: number;
}

// Per-session split trees, one slot per region. A missing region is null.
export interface SessionLayouts {
    viewer: unknown | null;
    terminal: unknown | null;
}

export interface SessionNode extends Session {
    viewerPanes: ViewerPane[];
    terminalPanes: TerminalPane[];
    layouts: SessionLayouts;
}

export interface GroupNode extends Group {
    sessions: SessionNode[];
}

// --- Method parameter shapes ------------------------------------------------

export interface CreateGroupParams {
    serverId: string;
    name: string;
    position?: number;
    collapsed?: boolean;
}

export interface UpdateGroupParams {
    id: string;
    name?: string;
    position?: number;
    collapsed?: boolean;
}

export interface CreateSessionParams {
    serverId: string;
    groupId: string;
    name: string;
    repositoryRoot: string;
    defaultWorkingDirectory?: string | null;
    taskDescription?: string | null;
    position?: number;
    archived?: boolean;
}

export interface UpdateSessionParams {
    id: string;
    name?: string;
    repositoryRoot?: string;
    defaultWorkingDirectory?: string | null;
    taskDescription?: string | null;
    position?: number;
    archived?: boolean;
}

export interface MoveSessionParams {
    id: string;
    groupId: string;
    position?: number;
}

export interface CreateViewerPaneParams {
    serverId: string;
    devSessionId: string;
    url: string;
    handler?: string | null;
    title?: string | null;
    position?: number;
}

export interface UpdateViewerPaneParams {
    id: string;
    url?: string;
    handler?: string | null;
    title?: string | null;
    position?: number;
}

export interface CreateTerminalPaneParams {
    serverId: string;
    devSessionId: string;
    name: string;
    workingDirectory?: string | null;
    tmuxTarget?: string | null;
    startupCommand?: string | null;
    harness?: string | null;
    position?: number;
}

export interface UpdateTerminalPaneParams {
    id: string;
    name?: string;
    workingDirectory?: string | null;
    tmuxTarget?: string | null;
    startupCommand?: string | null;
    harness?: string | null;
    position?: number;
}

export interface SetLayoutParams {
    serverId: string;
    devSessionId: string;
    region: Region;
    tree: unknown;
}

export interface GetLayoutParams {
    devSessionId: string;
    region: Region;
}

// --- Internal row shapes (raw column names as returned by node:sqlite) -------

interface GroupRow {
    id: string;
    server_id: string;
    name: string;
    position: number;
    collapsed: number;
    created_at: number;
    updated_at: number;
}

interface SessionRow {
    id: string;
    server_id: string;
    group_id: string;
    name: string;
    repository_root: string;
    default_working_directory: string | null;
    task_description: string | null;
    position: number;
    archived: number;
    created_at: number;
    updated_at: number;
}

interface ViewerPaneRow {
    id: string;
    server_id: string;
    dev_session_id: string;
    url: string;
    handler: string | null;
    title: string | null;
    position: number;
    created_at: number;
    updated_at: number;
}

interface TerminalPaneRow {
    id: string;
    server_id: string;
    dev_session_id: string;
    name: string;
    working_directory: string | null;
    tmux_target: string | null;
    startup_command: string | null;
    harness: string | null;
    position: number;
    created_at: number;
    updated_at: number;
}

interface SessionLayoutRow {
    id: string;
    server_id: string;
    dev_session_id: string;
    region: Region;
    tree: string;
    created_at: number;
    updated_at: number;
}

function toGroup(r: GroupRow): Group {
    return {
        id: r.id,
        serverId: r.server_id,
        name: r.name,
        position: r.position,
        collapsed: r.collapsed !== 0,
        createdAt: r.created_at,
        updatedAt: r.updated_at,
    };
}

function toSession(r: SessionRow): Session {
    return {
        id: r.id,
        serverId: r.server_id,
        groupId: r.group_id,
        name: r.name,
        repositoryRoot: r.repository_root,
        defaultWorkingDirectory: r.default_working_directory,
        taskDescription: r.task_description,
        position: r.position,
        archived: r.archived !== 0,
        createdAt: r.created_at,
        updatedAt: r.updated_at,
    };
}

function toViewerPane(r: ViewerPaneRow): ViewerPane {
    return {
        id: r.id,
        serverId: r.server_id,
        devSessionId: r.dev_session_id,
        url: r.url,
        handler: r.handler,
        title: r.title,
        position: r.position,
        createdAt: r.created_at,
        updatedAt: r.updated_at,
    };
}

function toTerminalPane(r: TerminalPaneRow): TerminalPane {
    return {
        id: r.id,
        serverId: r.server_id,
        devSessionId: r.dev_session_id,
        name: r.name,
        workingDirectory: r.working_directory,
        tmuxTarget: r.tmux_target,
        startupCommand: r.startup_command,
        harness: r.harness,
        position: r.position,
        createdAt: r.created_at,
        updatedAt: r.updated_at,
    };
}

// Rewrite the leaf paneIds of a split tree (SPEC 4.5) through an old->new id
// map, leaving structure and any unmapped ids untouched. Used by
// duplicateSession so a copied layout references the copied panes, not the
// originals. Shape matches SplitNode::toJson in src/models/SplitTree.cpp:
// leaves are { type: "leaf", paneId }, splits carry a children[] array.
function remapPaneIds(node: unknown, idMap: Record<string, string>): unknown {
    if (node === null || typeof node !== "object") return node;
    const n = node as Record<string, unknown>;
    if (n.type === "leaf") {
        const paneId = typeof n.paneId === "string" ? n.paneId : "";
        return { ...n, paneId: idMap[paneId] ?? paneId };
    }
    if (n.type === "split" && Array.isArray(n.children)) {
        return { ...n, children: n.children.map((child) => remapPaneIds(child, idMap)) };
    }
    return node;
}

/**
 * Open (creating if needed) the workspace database at `dbPath` and bring it up
 * to WORKSPACE_SCHEMA_VERSION. The migration runner applies schema.sql only
 * when the stored schema_version is absent or older; schema.sql is idempotent,
 * so re-application is harmless. A database from a NEWER build than this one is
 * rejected with a thrown error rather than used blind. Pass ":memory:" for an
 * ephemeral database.
 *
 * The lookup indexes are applied AFTER migration and unconditionally, so a
 * database already at the current version still gains an index added later (see
 * remote/sql/indexes.sql).
 */
export function openWorkspace(dbPath: string): Workspace {
    if (dbPath !== ":memory:") {
        mkdirSync(path.dirname(dbPath), { recursive: true });
    }
    const db = new DatabaseSync(dbPath);
    db.exec("PRAGMA foreign_keys = ON;");
    migrate(db);
    // After migrate(), never before: the indexed tables must exist first.
    db.exec(indexesSql);
    return new Workspace(db);
}

// Ordered schema migrations, one entry per version. migrate() applies every
// step whose version is above the stored version and at or below the target,
// in order, then records the target. Shipping a future structural change means
// appending a new entry here (and bumping WORKSPACE_SCHEMA_VERSION) rather than
// only advancing the version literal while tables stay un-migrated. v1 is the
// baseline: it applies the idempotent schema.sql DDL (C2), so re-running it on
// an already-current database is harmless.
const MIGRATIONS: ReadonlyArray<{
    version: number;
    apply: (db: DatabaseSync) => void;
}> = [
    { version: 1, apply: (db) => db.exec(schemaSql) },
    // v2 adds the server_identity singleton and nothing else. The whole change
    // is one CREATE TABLE IF NOT EXISTS, so re-running the authoritative DDL IS
    // the migration — an existing v1 database gains the table and keeps every
    // row. (A future step that alters an existing table cannot do this: CREATE
    // TABLE IF NOT EXISTS never adds a column, so it must spell out its ALTERs.)
    { version: 2, apply: (db) => db.exec(schemaSql) },
];

function migrate(db: DatabaseSync): void {
    const from = schemaVersion(db);
    if (from >= WORKSPACE_SCHEMA_VERSION) {
        // A database written by a NEWER build than this one. Refuse it instead
        // of using it: the missing migrations may have renamed or dropped
        // columns this build still selects, and the failures would surface much
        // later as confusing per-statement SQL errors on a live workspace.
        // Equality is the normal case and passes through silently.
        if (from > WORKSPACE_SCHEMA_VERSION) {
            throw new Error(
                `workspace database schema version ${from} is newer than this build supports (${WORKSPACE_SCHEMA_VERSION})`,
            );
        }
        return;
    }
    // One transaction for the whole upgrade, so a step that throws half way
    // leaves the database exactly as it was rather than at a version nobody
    // wrote migrations for. (schema.sql's leading `PRAGMA foreign_keys = ON` is
    // a no-op inside a transaction; openWorkspace already set it on this
    // connection, so nothing is lost.)
    db.exec("BEGIN IMMEDIATE");
    try {
        for (const step of MIGRATIONS) {
            if (step.version > from && step.version <= WORKSPACE_SCHEMA_VERSION) {
                step.apply(db);
            }
        }
        // schema.sql seeds the version row with INSERT OR IGNORE, which cannot
        // advance an already-present row. Record the target version explicitly
        // so a WORKSPACE_SCHEMA_VERSION bump is persisted (and migrate stops
        // re-running steps on every open) rather than the stored version
        // silently drifting from the DDL's hard-coded literal.
        db.prepare("UPDATE schema_version SET version = ? WHERE id = 1").run(
            WORKSPACE_SCHEMA_VERSION,
        );
        db.exec("COMMIT");
    } catch (err) {
        try {
            db.exec("ROLLBACK");
        } catch {
            // Already rolled back by the failure itself; the original error is
            // the one worth reporting.
        }
        throw err;
    }
}

function schemaVersion(db: DatabaseSync): number {
    try {
        const row = db
            .prepare("SELECT version FROM schema_version WHERE id = 1")
            .get() as { version: number } | undefined;
        return row?.version ?? 0;
    } catch {
        // schema_version table absent -> database is unmigrated (version 0).
        return 0;
    }
}

// A single workspace database connection with all CRUD operations (SPEC 4.2,
// 11.1). Booleans are stored as 0/1 integers; deletes cascade manually because
// the schema's foreign keys use the default NO ACTION (they reject, not
// cascade). Every mutation carries server_id per SPEC 3.5.
export class Workspace {
    readonly db: DatabaseSync;
    // Memoized server_identity row (see serverId()); the row never changes.
    private cachedServerId: string | undefined;
    // Nesting depth of transaction() on this connection; see transaction().
    private txDepth = 0;

    constructor(db: DatabaseSync) {
        this.db = db;
    }

    close(): void {
        this.db.close();
    }

    // --- Server identity ----------------------------------------------------

    /**
     * The stable, server-owned id of THIS database (SPEC 3.5), minted on first
     * call and returned unchanged forever after — including across restarts and
     * from other processes sharing the file. It is what `server.info` reports as
     * `serverId` and what every domain row's `server_id` refers to, so a client
     * can key its view of the remote workspace by it: dropping and re-adding a
     * connection profile, or connecting from a second machine, still resolves to
     * the same rows instead of an empty workspace beside orphaned data.
     *
     * Deliberately NOT derived from the hostname, port, user, repository path,
     * or anything else a client supplies: those describe the ROUTE to the data,
     * and all of them can change while the data stays put.
     *
     * Race-safe without a transaction: several codeharbord processes may open
     * the same file at once (tst_liveshell starts a second server), so minting
     * is INSERT OR IGNORE against the `id = 1` primary key followed by a read.
     * Whoever inserts first wins; every loser's insert is a no-op and every
     * caller then reads the one committed row. Both statements run in SQLite's
     * implicit per-statement transaction, and the row is never updated or
     * deleted, so there is no read-modify-write window to protect.
     */
    serverId(): string {
        if (this.cachedServerId !== undefined) return this.cachedServerId;
        this.db
            .prepare(
                "INSERT OR IGNORE INTO server_identity (id, server_id, created_at) VALUES (1, ?, ?)",
            )
            .run(randomUUID(), Date.now());
        const row = this.db
            .prepare("SELECT server_id FROM server_identity WHERE id = 1")
            .get() as { server_id: string } | undefined;
        if (!row) throw new Error("server identity missing after mint");
        // Immutable once written, so caching it is safe for this connection's
        // lifetime; every later server.info answers without touching the DB.
        this.cachedServerId = row.server_id;
        return this.cachedServerId;
    }

    // --- Groups -------------------------------------------------------------

    createGroup(params: CreateGroupParams): Group {
        const id = randomUUID();
        const ts = Date.now();
        const position = params.position ?? this.nextPosition("groups", "server_id", params.serverId);
        this.db
            .prepare(
                "INSERT INTO groups (id, server_id, name, position, collapsed, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            )
            .run(id, params.serverId, params.name, position, params.collapsed ? 1 : 0, ts, ts);
        return this.getGroup(id);
    }

    getGroup(id: string): Group {
        const row = this.db.prepare("SELECT * FROM groups WHERE id = ?").get(id) as GroupRow | undefined;
        if (!row) throw new Error(`group not found: ${id}`);
        return toGroup(row);
    }

    // Read-modify-write: every column is rewritten from the row this call read,
    // so it runs inside a transaction. Without one, two connections editing
    // different fields of the same group (a rename and a collapse toggle) each
    // write back their own stale snapshot of the other's column, and one edit
    // silently disappears.
    updateGroup(params: UpdateGroupParams): Group {
        return this.transaction(() => {
            const current = this.getGroup(params.id);
            const name = params.name ?? current.name;
            const position = params.position ?? current.position;
            const collapsed = params.collapsed ?? current.collapsed;
            this.db
                .prepare("UPDATE groups SET name = ?, position = ?, collapsed = ?, updated_at = ? WHERE id = ?")
                .run(name, position, collapsed ? 1 : 0, Date.now(), params.id);
            return this.getGroup(params.id);
        });
    }

    deleteGroup(params: { id: string }): { ok: true } {
        return this.transaction(() => {
            const sessions = this.db
                .prepare("SELECT id FROM dev_sessions WHERE group_id = ?").all(params.id) as unknown as Array<{ id: string }>;
            for (const s of sessions) this.deleteSessionRows(s.id);
            this.db.prepare("DELETE FROM groups WHERE id = ?").run(params.id);
            return { ok: true } as const;
        });
    }

    reorderGroups(params: { serverId: string; orderedIds: string[] }): { ok: true } {
        return this.transaction(() => {
            this.packOrder("groups", "server_id", params.serverId, params.orderedIds, Date.now());
            return { ok: true } as const;
        });
    }

    // --- Sessions -----------------------------------------------------------

    createSession(params: CreateSessionParams): Session {
        const id = randomUUID();
        const ts = Date.now();
        // SPEC 3.5: a child's server_id is authoritative from its parent row,
        // never the client-supplied param — otherwise a mismatched serverId
        // could surface this session under a foreign server's group. The wire
        // param is still accepted (C1) but overridden here.
        const serverId = this.parentServerId("groups", params.groupId);
        const position = params.position ?? this.nextPosition("dev_sessions", "group_id", params.groupId);
        this.db
            .prepare(
                "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, default_working_directory, task_description, position, archived, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            )
            .run(
                id,
                serverId,
                params.groupId,
                params.name,
                params.repositoryRoot,
                params.defaultWorkingDirectory ?? null,
                params.taskDescription ?? null,
                position,
                params.archived ? 1 : 0,
                ts,
                ts,
            );
        return this.getSession(id);
    }

    getSession(id: string): Session {
        const row = this.db.prepare("SELECT * FROM dev_sessions WHERE id = ?").get(id) as SessionRow | undefined;
        if (!row) throw new Error(`session not found: ${id}`);
        return toSession(row);
    }

    // Read-modify-write of every column (see updateGroup), so it is wrapped in a
    // transaction to keep two concurrent partial updates from clobbering each
    // other's fields.
    updateSession(params: UpdateSessionParams): Session {
        return this.transaction(() => {
            const current = this.getSession(params.id);
            const name = params.name ?? current.name;
            const repositoryRoot = params.repositoryRoot ?? current.repositoryRoot;
            const defaultWorkingDirectory =
                params.defaultWorkingDirectory !== undefined
                    ? params.defaultWorkingDirectory
                    : current.defaultWorkingDirectory;
            const taskDescription =
                params.taskDescription !== undefined ? params.taskDescription : current.taskDescription;
            const position = params.position ?? current.position;
            const archived = params.archived ?? current.archived;
            this.db
                .prepare(
                    "UPDATE dev_sessions SET name = ?, repository_root = ?, default_working_directory = ?, task_description = ?, position = ?, archived = ?, updated_at = ? WHERE id = ?",
                )
                .run(
                    name,
                    repositoryRoot,
                    defaultWorkingDirectory,
                    taskDescription,
                    position,
                    archived ? 1 : 0,
                    Date.now(),
                    params.id,
                );
            return this.getSession(params.id);
        });
    }

    deleteSession(params: { id: string }): { ok: true } {
        return this.transaction(() => {
            this.deleteSessionRows(params.id);
            return { ok: true } as const;
        });
    }

    reorderSessions(params: { groupId: string; orderedIds: string[] }): { ok: true } {
        return this.transaction(() => {
            this.packOrder("dev_sessions", "group_id", params.groupId, params.orderedIds, Date.now());
            return { ok: true } as const;
        });
    }

    // Move a session into `groupId` at `position`, re-packing BOTH affected
    // groups to contiguous 0..n-1. Storing the requested position verbatim used
    // to leave the target group with two rows sharing it, and every listing
    // query orders by `position, id` — so a tie was broken by UUID, i.e. at
    // random (a drag to the top could land second). The target order is
    // therefore rebuilt explicitly: read the group's current order, drop the
    // moved row if it is already there, splice it back in at the clamped index,
    // and renumber. The source group is renumbered from its own fresh read so
    // the vacated slot leaves no hole. All of it inside one transaction, the
    // same idiom reorderSessions/reorderGroups use.
    //
    // Moving between groups that belong to DIFFERENT servers also re-homes the
    // session's server_id and that of its panes and layouts, because SPEC 3.5's
    // invariant is that a row shares its ancestors' server: leaving the old
    // server_id behind would list the session under the target group while
    // `list()` of the target server reported a different serverId on the row.
    moveSessionToGroup(params: MoveSessionParams): Session {
        return this.transaction(() => {
            const session = this.getSession(params.id); // also rejects an unknown session
            const sourceGroupId = session.groupId;
            // Rejects an unknown target group up front, before any write, with a
            // clearer message than the foreign key's.
            const targetServerId = this.parentServerId("groups", params.groupId);
            const ts = Date.now();

            // Target order as it will be AFTER the move, moved row excluded so a
            // same-group move is a pure reorder rather than a duplicate entry.
            const ordered = this.orderedIds("dev_sessions", "group_id", params.groupId).filter(
                (id) => id !== params.id,
            );
            const requested = params.position;
            const index =
                requested === undefined || !Number.isFinite(requested)
                    ? ordered.length
                    : Math.min(Math.max(Math.trunc(requested), 0), ordered.length);
            ordered.splice(index, 0, params.id);

            this.db
                .prepare("UPDATE dev_sessions SET group_id = ?, server_id = ?, updated_at = ? WHERE id = ?")
                .run(params.groupId, targetServerId, ts, params.id);
            if (targetServerId !== session.serverId) {
                for (const table of ["viewer_panes", "terminal_panes", "session_layouts"] as const) {
                    this.db
                        .prepare(`UPDATE ${table} SET server_id = ? WHERE dev_session_id = ?`)
                        .run(targetServerId, params.id);
                }
            }

            this.packOrder("dev_sessions", "group_id", params.groupId, ordered, ts);
            if (sourceGroupId !== params.groupId) {
                // Fresh read: the moved row already belongs to the target group.
                this.packOrder(
                    "dev_sessions",
                    "group_id",
                    sourceGroupId,
                    this.orderedIds("dev_sessions", "group_id", sourceGroupId),
                    ts,
                );
            }
            return this.getSession(params.id);
        });
    }

    // Copy a Dev Session's viewer + terminal pane definitions, split layouts,
    // repository root, and task metadata into a NEW session with fresh UUIDs,
    // minting a fresh tmux target `ch_<newSessionId>_<newTerminalId>` for every
    // copied terminal pane (SPEC 4.2). Layout leaf paneIds are remapped to the
    // copied panes so the duplicate's split trees stay self-consistent.
    duplicateSession(params: { id: string }): SessionNode {
        return this.transaction(() => {
            const source = this.db
                .prepare("SELECT * FROM dev_sessions WHERE id = ?")
                .get(params.id) as SessionRow | undefined;
            if (!source) throw new Error(`session not found: ${params.id}`);

            const newSessionId = randomUUID();
            const ts = Date.now();
            const position = this.nextPosition("dev_sessions", "group_id", source.group_id);
            this.db
                .prepare(
                    "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, default_working_directory, task_description, position, archived, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                )
                .run(
                    newSessionId,
                    source.server_id,
                    source.group_id,
                    source.name,
                    source.repository_root,
                    source.default_working_directory,
                    source.task_description,
                    position,
                    source.archived,
                    ts,
                    ts,
                );

            // One prepared statement per table, reused for every copied row: a
            // fresh prepare() inside the loop compiled the identical SQL again
            // for each pane and left a statement handle per iteration for the
            // garbage collector to finalize. The copies take the NEW session's
            // server_id (which is the source session's) rather than each child
            // row's own, so a duplicate always satisfies SPEC 3.5's "a row
            // shares its ancestors' server" invariant even if the source data
            // predates that rule.
            const viewerIdMap: Record<string, string> = {};
            const viewerRows = this.db
                .prepare("SELECT * FROM viewer_panes WHERE dev_session_id = ? ORDER BY position, id").all(params.id) as unknown as ViewerPaneRow[];
            const insertViewer = this.db.prepare(
                "INSERT INTO viewer_panes (id, server_id, dev_session_id, url, handler, title, position, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            );
            for (const v of viewerRows) {
                const newId = randomUUID();
                viewerIdMap[v.id] = newId;
                insertViewer.run(
                    newId,
                    source.server_id,
                    newSessionId,
                    v.url,
                    v.handler,
                    v.title,
                    v.position,
                    ts,
                    ts,
                );
            }

            const terminalIdMap: Record<string, string> = {};
            const terminalRows = this.db
                .prepare("SELECT * FROM terminal_panes WHERE dev_session_id = ? ORDER BY position, id").all(params.id) as unknown as TerminalPaneRow[];
            const insertTerminal = this.db.prepare(
                "INSERT INTO terminal_panes (id, server_id, dev_session_id, name, working_directory, tmux_target, startup_command, harness, position, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            );
            for (const t of terminalRows) {
                const newId = randomUUID();
                terminalIdMap[t.id] = newId;
                const tmuxTarget = `ch_${newSessionId}_${newId}`;
                insertTerminal.run(
                    newId,
                    source.server_id,
                    newSessionId,
                    t.name,
                    t.working_directory,
                    tmuxTarget,
                    t.startup_command,
                    t.harness,
                    t.position,
                    ts,
                    ts,
                );
            }

            const layoutRows = this.db
                .prepare("SELECT * FROM session_layouts WHERE dev_session_id = ?").all(params.id) as unknown as SessionLayoutRow[];
            const insertLayout = this.db.prepare(
                "INSERT INTO session_layouts (id, server_id, dev_session_id, region, tree, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            );
            for (const l of layoutRows) {
                const idMap = l.region === "viewer" ? viewerIdMap : terminalIdMap;
                const tree = JSON.stringify(remapPaneIds(JSON.parse(l.tree), idMap));
                insertLayout.run(randomUUID(), source.server_id, newSessionId, l.region, tree, ts, ts);
            }

            return this.sessionNode(newSessionId);
        });
    }

    // --- Viewer panes -------------------------------------------------------

    createViewerPane(params: CreateViewerPaneParams): ViewerPane {
        const id = randomUUID();
        const ts = Date.now();
        // SPEC 3.5: server_id is derived from the parent session, not trusted
        // from the param, so a mismatched serverId cannot detach this pane from
        // its session's server.
        const serverId = this.parentServerId("dev_sessions", params.devSessionId);
        const position =
            params.position ?? this.nextPosition("viewer_panes", "dev_session_id", params.devSessionId);
        this.db
            .prepare(
                "INSERT INTO viewer_panes (id, server_id, dev_session_id, url, handler, title, position, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            )
            .run(
                id,
                serverId,
                params.devSessionId,
                params.url,
                params.handler ?? null,
                params.title ?? null,
                position,
                ts,
                ts,
            );
        return this.getViewerPane(id);
    }

    getViewerPane(id: string): ViewerPane {
        const row = this.db.prepare("SELECT * FROM viewer_panes WHERE id = ?").get(id) as
            | ViewerPaneRow
            | undefined;
        if (!row) throw new Error(`viewer pane not found: ${id}`);
        return toViewerPane(row);
    }

    // Read-modify-write of every column (see updateGroup), hence the transaction.
    updateViewerPane(params: UpdateViewerPaneParams): ViewerPane {
        return this.transaction(() => {
            const current = this.getViewerPane(params.id);
            const url = params.url ?? current.url;
            const handler = params.handler !== undefined ? params.handler : current.handler;
            const title = params.title !== undefined ? params.title : current.title;
            const position = params.position ?? current.position;
            this.db
                .prepare("UPDATE viewer_panes SET url = ?, handler = ?, title = ?, position = ?, updated_at = ? WHERE id = ?")
                .run(url, handler, title, position, Date.now(), params.id);
            return this.getViewerPane(params.id);
        });
    }

    deleteViewerPane(params: { id: string }): { ok: true } {
        this.db.prepare("DELETE FROM viewer_panes WHERE id = ?").run(params.id);
        return { ok: true };
    }

    // --- Terminal panes -----------------------------------------------------

    createTerminalPane(params: CreateTerminalPaneParams): TerminalPane {
        const id = randomUUID();
        const ts = Date.now();
        // SPEC 3.5: server_id derived from the parent session (see
        // createViewerPane); the client-sent serverId is overridden.
        const serverId = this.parentServerId("dev_sessions", params.devSessionId);
        const position =
            params.position ?? this.nextPosition("terminal_panes", "dev_session_id", params.devSessionId);
        this.db
            .prepare(
                "INSERT INTO terminal_panes (id, server_id, dev_session_id, name, working_directory, tmux_target, startup_command, harness, position, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            )
            .run(
                id,
                serverId,
                params.devSessionId,
                params.name,
                params.workingDirectory ?? null,
                params.tmuxTarget ?? null,
                params.startupCommand ?? null,
                params.harness ?? null,
                position,
                ts,
                ts,
            );
        return this.getTerminalPane(id);
    }

    getTerminalPane(id: string): TerminalPane {
        const row = this.db.prepare("SELECT * FROM terminal_panes WHERE id = ?").get(id) as
            | TerminalPaneRow
            | undefined;
        if (!row) throw new Error(`terminal pane not found: ${id}`);
        return toTerminalPane(row);
    }

    // Read-modify-write of every column (see updateGroup), hence the transaction.
    updateTerminalPane(params: UpdateTerminalPaneParams): TerminalPane {
        return this.transaction(() => {
            const current = this.getTerminalPane(params.id);
            const name = params.name ?? current.name;
            const workingDirectory =
                params.workingDirectory !== undefined ? params.workingDirectory : current.workingDirectory;
            const tmuxTarget = params.tmuxTarget !== undefined ? params.tmuxTarget : current.tmuxTarget;
            const startupCommand =
                params.startupCommand !== undefined ? params.startupCommand : current.startupCommand;
            const harness = params.harness !== undefined ? params.harness : current.harness;
            const position = params.position ?? current.position;
            this.db
                .prepare(
                    "UPDATE terminal_panes SET name = ?, working_directory = ?, tmux_target = ?, startup_command = ?, harness = ?, position = ?, updated_at = ? WHERE id = ?",
                )
                .run(name, workingDirectory, tmuxTarget, startupCommand, harness, position, Date.now(), params.id);
            return this.getTerminalPane(params.id);
        });
    }

    deleteTerminalPane(params: { id: string }): { ok: true } {
        this.db.prepare("DELETE FROM terminal_panes WHERE id = ?").run(params.id);
        return { ok: true };
    }

    // --- Split layouts ------------------------------------------------------

    getLayout(params: GetLayoutParams): SessionLayout | null {
        const row = this.db
            .prepare("SELECT * FROM session_layouts WHERE dev_session_id = ? AND region = ?")
            .get(params.devSessionId, params.region) as SessionLayoutRow | undefined;
        if (!row) return null;
        return {
            id: row.id,
            serverId: row.server_id,
            devSessionId: row.dev_session_id,
            region: row.region,
            tree: JSON.parse(row.tree),
            createdAt: row.created_at,
            updatedAt: row.updated_at,
        };
    }

    // Store the region's split tree, replacing any tree already there. A single
    // upsert keyed on the schema's UNIQUE (dev_session_id, region) does it: the
    // previous SELECT-then-INSERT-or-UPDATE could have two connections both see
    // no row and both insert, and the loser then failed on the unique index. The
    // update branch keeps the original id and created_at, so the row's identity
    // survives every rewrite.
    //
    // SPEC 3.5: server_id comes from the parent Dev Session, not from the
    // client-supplied param (same rule as createViewerPane/createTerminalPane),
    // which also rejects an unknown devSessionId with a clear error instead of a
    // bare foreign-key failure. The wire param is still accepted for symmetry
    // with the other create calls.
    setLayout(params: SetLayoutParams): SessionLayout {
        const serverId = this.parentServerId("dev_sessions", params.devSessionId);
        const ts = Date.now();
        this.db
            .prepare(
                "INSERT INTO session_layouts (id, server_id, dev_session_id, region, tree, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?) " +
                    "ON CONFLICT (dev_session_id, region) DO UPDATE SET server_id = excluded.server_id, tree = excluded.tree, updated_at = excluded.updated_at",
            )
            .run(
                randomUUID(),
                serverId,
                params.devSessionId,
                params.region,
                JSON.stringify(params.tree),
                ts,
                ts,
            );
        // Guaranteed present after the upsert above.
        return this.getLayout(params) as SessionLayout;
    }

    // --- Nested read --------------------------------------------------------

    // The nested tree groups -> sessions -> {viewerPanes, terminalPanes,
    // layouts}. Ordering is deterministic (position, then id) so a reopen of
    // the same database yields byte-identical output.
    list(serverId: string): GroupNode[] {
        const rows = this.db
            .prepare("SELECT * FROM groups WHERE server_id = ? ORDER BY position, id").all(serverId) as unknown as GroupRow[];
        return rows.map((g) => ({ ...toGroup(g), sessions: this.listSessions(g.id) }));
    }

    private listSessions(groupId: string): SessionNode[] {
        const rows = this.db
            .prepare("SELECT * FROM dev_sessions WHERE group_id = ? ORDER BY position, id").all(groupId) as unknown as SessionRow[];
        return rows.map((s) => this.sessionNode(s.id, s));
    }

    private sessionNode(sessionId: string, row?: SessionRow): SessionNode {
        const session = row
            ? toSession(row)
            : this.getSession(sessionId);
        return {
            ...session,
            viewerPanes: this.listViewerPanes(sessionId),
            terminalPanes: this.listTerminalPanes(sessionId),
            layouts: this.getLayouts(sessionId),
        };
    }

    private listViewerPanes(sessionId: string): ViewerPane[] {
        const rows = this.db
            .prepare("SELECT * FROM viewer_panes WHERE dev_session_id = ? ORDER BY position, id").all(sessionId) as unknown as ViewerPaneRow[];
        return rows.map(toViewerPane);
    }

    private listTerminalPanes(sessionId: string): TerminalPane[] {
        const rows = this.db
            .prepare("SELECT * FROM terminal_panes WHERE dev_session_id = ? ORDER BY position, id").all(sessionId) as unknown as TerminalPaneRow[];
        return rows.map(toTerminalPane);
    }

    private getLayouts(sessionId: string): SessionLayouts {
        const rows = this.db
            .prepare("SELECT region, tree FROM session_layouts WHERE dev_session_id = ?").all(sessionId) as unknown as Array<{ region: Region; tree: string }>;
        const layouts: SessionLayouts = { viewer: null, terminal: null };
        for (const r of rows) layouts[r.region] = JSON.parse(r.tree);
        return layouts;
    }

    // --- Internal helpers ---------------------------------------------------

    // Delete a session and its dependent panes/layouts. Children go first
    // because the schema's foreign keys reject (NO ACTION), not cascade.
    private deleteSessionRows(id: string): void {
        this.db.prepare("DELETE FROM viewer_panes WHERE dev_session_id = ?").run(id);
        this.db.prepare("DELETE FROM terminal_panes WHERE dev_session_id = ?").run(id);
        this.db.prepare("DELETE FROM session_layouts WHERE dev_session_id = ?").run(id);
        this.db.prepare("DELETE FROM dev_sessions WHERE id = ?").run(id);
    }

    // Scope of an ordered, re-packable table: a server's groups, or a group's
    // sessions. The table/column pairs are literal unions rather than plain
    // strings because they are interpolated into SQL — an identifier cannot be a
    // bound parameter, so the type is what keeps a caller-supplied name from
    // ever reaching the query text.
    private orderedIds(
        table: "groups" | "dev_sessions",
        scopeColumn: "server_id" | "group_id",
        scopeValue: string,
    ): string[] {
        const rows = this.db
            .prepare(`SELECT id FROM ${table} WHERE ${scopeColumn} = ? ORDER BY position, id`).all(scopeValue) as unknown as Array<{ id: string }>;
        return rows.map((r) => r.id);
    }

    // Renumber one scope to contiguous positions 0..n-1 in the caller's order.
    // Callers must already be inside a transaction.
    //
    // `orderedIds` is a REQUEST, not the final truth: ids that are not in the
    // scope (stale, or belonging to another group/server) are dropped, repeats
    // collapse to their first occurrence, and rows the caller left out keep their
    // current relative order at the end. That reconciliation is load-bearing —
    // renumbering only the listed rows would leave every unlisted row on its old
    // position, so two rows in one scope could end up sharing a position, and
    // since every listing query orders by `position, id`, the tie would be broken
    // by UUID, i.e. at random. A client that sends a filtered or stale list gets
    // its requested prefix and a still-packed scope instead of a shuffle.
    private packOrder(
        table: "groups" | "dev_sessions",
        scopeColumn: "server_id" | "group_id",
        scopeValue: string,
        orderedIds: readonly string[],
        ts: number,
    ): void {
        const current = this.orderedIds(table, scopeColumn, scopeValue);
        const inScope = new Set(current);
        const wanted = new Set<string>();
        for (const id of orderedIds) {
            if (inScope.has(id)) wanted.add(id);
        }
        const final = [...wanted, ...current.filter((id) => !wanted.has(id))];
        const stmt = this.db.prepare(
            `UPDATE ${table} SET position = ?, updated_at = ? WHERE id = ? AND ${scopeColumn} = ?`,
        );
        final.forEach((id, index) => {
            stmt.run(index, ts, id, scopeValue);
        });
    }

    // Next free position in a scope: one past the current maximum, so the first
    // row of an empty scope lands on 0. See orderedIds on why the identifiers are
    // literal unions.
    private nextPosition(
        table: "groups" | "dev_sessions" | "viewer_panes" | "terminal_panes",
        scopeColumn: "server_id" | "group_id" | "dev_session_id",
        scopeValue: string,
    ): number {
        const row = this.db
            .prepare(`SELECT COALESCE(MAX(position), -1) AS m FROM ${table} WHERE ${scopeColumn} = ?`)
            .get(scopeValue) as { m: number };
        return row.m + 1;
    }

    // Authoritative server_id of a parent row (SPEC 3.5). Children inherit their
    // parent's server_id rather than trusting the client-supplied param, keeping
    // the multi-server invariant that a row and its ancestors share one server.
    // Throws if the parent is missing (the FK would reject the insert anyway,
    // but this yields a clearer error and runs before the child id is minted).
    private parentServerId(table: "groups" | "dev_sessions", id: string): string {
        const row = this.db
            .prepare(`SELECT server_id FROM ${table} WHERE id = ?`)
            .get(id) as { server_id: string } | undefined;
        if (!row) throw new Error(`${table} not found: ${id}`);
        return row.server_id;
    }

    // All-or-nothing wrapper around a group of writes.
    //
    // BEGIN IMMEDIATE, not the default deferred BEGIN: every caller writes, and
    // taking the write lock up front makes two connections that read-modify-write
    // the same rows serialize (the loser fails loudly with SQLITE_BUSY) instead of
    // one silently overwriting fields the other had just changed.
    //
    // Re-entrant via a depth counter, because SQLite rejects a nested BEGIN: a
    // helper that needs atomicity of its own (updateSession, packOrder's callers)
    // can therefore be called both directly and from inside a larger operation.
    // A nested failure still aborts the whole outermost transaction, which is the
    // semantics every caller here wants.
    private transaction<T>(fn: () => T): T {
        if (this.txDepth > 0) return fn();
        this.db.exec("BEGIN IMMEDIATE");
        this.txDepth = 1;
        try {
            const result = fn();
            this.txDepth = 0;
            this.db.exec("COMMIT");
            return result;
        } catch (err) {
            this.txDepth = 0;
            try {
                this.db.exec("ROLLBACK");
            } catch {
                // Already rolled back by the failure itself (SQLite aborts the
                // transaction on some errors); the original error is the one
                // worth propagating, so swallow this one.
            }
            throw err;
        }
    }
}

// Lazily opened default connection backing the RPC handlers. Opened on first
// use (not at import) so importing the module has no filesystem side effects
// beyond reading the schema. The location follows SPEC 11.1, overridable via
// CODEHARBOR_DB for tests and alternate deployments.
let defaultWorkspace: Workspace | undefined;

function workspace(): Workspace {
    if (defaultWorkspace === undefined) {
        const dbPath =
            process.env.CODEHARBOR_DB ??
            path.join(os.homedir(), ".local", "share", "codeharbor", "codeharbor.sqlite");
        defaultWorkspace = openWorkspace(dbPath);
    }
    return defaultWorkspace;
}

// This server's stable identity, from the same default database the
// `workspace.*` handlers write to — `server.info` reports it as `serverId`.
// Any failure to open or mint propagates: answering with a placeholder would
// invite the client to key its workspace by an id the rows do not carry.
export function serverIdentity(): string {
    return workspace().serverId();
}

// RPC handler table for the `workspace.*` method group (P's own group; NOT part
// of the frozen C1 file catalog). codeharbord spreads these into its method map
// and awaits any returned value; a thrown DB error becomes a JSON-RPC error.
// The keys come from the shared RPC_WORKSPACE_METHODS contract, so a wire-name
// change is a one-place edit here and in its C++ mirror — never a retyped
// literal that can drift apart silently.
export const WORKSPACE_METHODS: Record<RpcWorkspaceMethodName, (params: unknown) => unknown> = {
    [M.list]: (p) => {
        if (typeof p !== "object" || p === null || !("serverId" in p) || typeof p.serverId !== "string") {
            throw new Error(`${M.list} requires a string serverId`);
        }
        return workspace().list(p.serverId);
    },
    [M.createGroup]: (p) => workspace().createGroup(p as CreateGroupParams),
    [M.updateGroup]: (p) => workspace().updateGroup(p as UpdateGroupParams),
    [M.deleteGroup]: (p) => workspace().deleteGroup(p as { id: string }),
    [M.reorderGroups]: (p) => workspace().reorderGroups(p as { serverId: string; orderedIds: string[] }),
    [M.createSession]: (p) => workspace().createSession(p as CreateSessionParams),
    [M.updateSession]: (p) => workspace().updateSession(p as UpdateSessionParams),
    [M.deleteSession]: (p) => workspace().deleteSession(p as { id: string }),
    [M.reorderSessions]: (p) => workspace().reorderSessions(p as { groupId: string; orderedIds: string[] }),
    [M.moveSessionToGroup]: (p) => workspace().moveSessionToGroup(p as MoveSessionParams),
    [M.duplicateSession]: (p) => workspace().duplicateSession(p as { id: string }),
    [M.createViewerPane]: (p) => workspace().createViewerPane(p as CreateViewerPaneParams),
    [M.updateViewerPane]: (p) => workspace().updateViewerPane(p as UpdateViewerPaneParams),
    [M.deleteViewerPane]: (p) => workspace().deleteViewerPane(p as { id: string }),
    [M.createTerminalPane]: (p) => workspace().createTerminalPane(p as CreateTerminalPaneParams),
    [M.updateTerminalPane]: (p) => workspace().updateTerminalPane(p as UpdateTerminalPaneParams),
    [M.deleteTerminalPane]: (p) => workspace().deleteTerminalPane(p as { id: string }),
    [M.getLayout]: (p) => workspace().getLayout(p as GetLayoutParams),
    [M.setLayout]: (p) => workspace().setLayout(p as SetLayoutParams),
};
