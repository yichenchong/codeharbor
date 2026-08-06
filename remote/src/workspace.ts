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
import type { StatementSync } from "node:sqlite";
import { randomUUID } from "node:crypto";
import { mkdirSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import os from "node:os";
import path from "node:path";

import { RPC_WORKSPACE_METHODS as M } from "./rpc-types.ts";
import type { RpcWorkspaceMethodName } from "./rpc-types.ts";
import {
    requireObject,
    requireString,
    optionalString,
    optionalPlainString,
    requireStringArray,
    optionalIndex,
    optionalNumber,
    optionalBoolean,
    requireOneOf,
    requireDefined,
    InvalidParamsError,
} from "./validate.ts";
import { isSafeTmuxTarget, tmuxSafeName, TMUX_TARGET_MAX_LENGTH } from "./tmux.ts";
// The shared resource-limit error (RPC_RESOURCE_LIMIT), not a second class
// saying the same thing: the dispatcher recognizes it structurally by its
// `code`, and file.readFile / file.listDirectory already raise this exact type
// for "valid request, too big to move". files.ts has no import-time side
// effects and does not import this module, so there is no cycle.
import { ResourceLimitError } from "./files.ts";

// Current schema version. Mirrors schema_version in remote/sql/schema.sql and
// WorkspaceDb::kSchemaVersion (bump all three together — see schema.sql header).
// Bumped 1 -> 2 for the server_identity singleton (SPEC 3.5).
// Bumped 2 -> 3 for UNIQUE (tmux_target) on terminal_panes: two panes sharing
// one target attach the SAME remote shell and mirror each other's keystrokes.
// Bumped 3 -> 4 to REMOVE the UNIQUE (dev_session_id, name) v3 had added: a
// layout slot label is not a terminal's identity (the row id in the layout leaf
// is), and a closed pane keeps its row and its label while a new pane
// legitimately takes the same label.
// Bumped 4 -> 5 to persist Dev Session pin state. The migration only adds the
// NOT NULL column with a false default, so existing sessions remain unpinned.
export const WORKSPACE_SCHEMA_VERSION = 5;

// The authoritative DDL (C2). Read relative to this module so it resolves the
// same whether invoked from src/ or from a built dist/ alongside sql/.
const schemaSql = readFileSync(
    fileURLToPath(new URL("../sql/schema.sql", import.meta.url)),
    "utf8",
);

// Lookup indexes, applied on EVERY open rather than by the migration runner.
// They carry no data and every statement is idempotent (`CREATE INDEX IF NOT
// EXISTS` / `DROP INDEX IF EXISTS`), so re-running them is a no-op; that is
// precisely what lets an existing database at the current schema version gain a
// newly added index — or shed a retired one — without a version bump (which
// would have to be mirrored in C++ — see indexes.sql for the full reasoning).
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
    pinned: boolean;
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

// The two split regions of the fixed layout. Declared as a runtime array so the
// RPC guards can reject an unlisted region by name (requireOneOf below) instead
// of letting SQLite's CHECK constraint answer for them; the TYPE is derived
// from the array, so the two can never drift apart.
export const REGIONS = ["viewer", "terminal"] as const;
export type Region = (typeof REGIONS)[number];

export interface SessionLayout {
    id: string;
    serverId: string;
    devSessionId: string;
    region: Region;
    tree: unknown;
    createdAt: number;
    updatedAt: number;
}

// Per-session split trees, one slot per region. `null` means the region has no
// stored layout (or its stored tree no longer parses — see getLayouts); any
// other value is the client-authored tree exactly as it was stored. The fields
// are `unknown` rather than `unknown | null`: TypeScript collapses that union
// back to `unknown`, so writing it only suggests a narrowing that is not there.
export interface SessionLayouts {
    viewer: unknown;
    terminal: unknown;
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
    pinned?: boolean;
}

export interface UpdateSessionParams {
    id: string;
    name?: string;
    repositoryRoot?: string;
    defaultWorkingDirectory?: string | null;
    taskDescription?: string | null;
    position?: number;
    archived?: boolean;
    pinned?: boolean;
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

// Find the `terminal_panes` row one layout leaf owns, creating it only on the
// legacy path below. Exactly ONE of `id` and `name` must be given.
//
//   * `id` — the row's own identity, carried in the layout leaf
//     (SplitNode::terminalPaneId). Pure lookup: the row was minted when the
//     leaf was created and is never invented here. This is the normal path.
//
//   * `name` — a layout slot LABEL, and lookup-or-CREATE. Only for leaves
//     stored before layouts carried a row id, where the label genuinely IS the
//     historical key. The label is not unique (see remote/sql/schema.sql), so
//     this resolves the OLDEST row bearing it: a legacy leaf predates every row
//     a later mint could have added under the same label. The client writes the
//     answer's id back into the leaf, so a given leaf takes this path once.
export interface ResolveTerminalPaneParams {
    serverId: string;
    devSessionId: string;
    id?: string;
    name?: string;
    // Only used when the row has to be CREATED: it is where a brand new tmux
    // session is rooted. An existing pane keeps the directory it was made with,
    // because `tmux new-session -c` applies only at creation anyway.
    workingDirectory?: string | null;
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
    pinned: number;
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
        pinned: r.pinned !== 0,
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

// Rewrite the pane references of a split tree (SPEC 4.5) through an old->new id
// map, leaving structure and any unmapped ids untouched. Used by
// duplicateSession so a copied layout references the copied panes, not the
// originals. Shape matches SplitNode::toJson in src/models/SplitTree.cpp:
// leaves are { type: "leaf", paneId, terminalPaneId? }, splits carry a
// children[] array.
//
// `terminalPaneId` is remapped for the same reason and it matters MORE than
// paneId does: it names the `terminal_panes` row whose tmux session the leaf
// attaches to. A copy that kept the original's row ids would put the duplicate
// and the original on the SAME remote shells — every keystroke mirrored — even
// though duplicateSession went to the trouble of inserting fresh rows with
// freshly minted targets for exactly the opposite outcome.
function remapPaneIds(node: unknown, idMap: ReadonlyMap<string, string>): unknown {
    if (node === null || typeof node !== "object") return node;
    const n = node as Record<string, unknown>;
    if (n.type === "leaf") {
        // Only a STRING paneId is a pane reference. A leaf carrying anything
        // else is malformed client data, and it is copied through untouched
        // rather than rewritten to "" — the copy is supposed to differ from the
        // original only in which panes it names.
        const leaf: Record<string, unknown> = { ...n };
        if (typeof n.paneId === "string") leaf.paneId = idMap.get(n.paneId) ?? n.paneId;
        if (typeof n.terminalPaneId === "string" && n.terminalPaneId !== "") {
            // An unmapped row id would name a row in ANOTHER Dev Session, which
            // resolveTerminalPane refuses. Dropping the field instead lets the
            // copied leaf mint a row of its own on first use.
            const mapped = idMap.get(n.terminalPaneId);
            if (mapped === undefined) delete leaf.terminalPaneId;
            else leaf.terminalPaneId = mapped;
        }
        return leaf;
    }
    if (n.type === "split" && Array.isArray(n.children)) {
        return { ...n, children: n.children.map((child) => remapPaneIds(child, idMap)) };
    }
    return node;
}

// Remove the leaf that references `paneId` from a split tree (SPEC 4.5),
// repairing structure so no surviving leaf points at a deleted pane. Matches
// either the leaf's own `paneId` or its `terminalPaneId`, because deleting a
// `terminal_panes` row strands a leaf naming it just as surely (the callers
// pass a row id, which for a terminal leaf is the field that holds one).
// Returns the rewritten node, or null when the whole subtree drained (the
// caller then substitutes a single empty leaf). A split left with exactly one
// child promotes that child in place; surviving children keep parallel,
// renormalized ratios — SplitNode::toJson / parseNode in
// src/models/SplitTree.cpp require one finite, positive ratio per child. Shape
// matches remapPaneIds above.
function removePaneFromTree(node: unknown, paneId: string): unknown | null {
    if (node === null || typeof node !== "object") return node;
    const n = node as Record<string, unknown>;
    if (n.type === "leaf") {
        return n.paneId === paneId || n.terminalPaneId === paneId ? null : node;
    }
    if (n.type === "split" && Array.isArray(n.children)) {
        const ratios = Array.isArray(n.ratios) ? n.ratios : [];
        const keptChildren: unknown[] = [];
        const keptRatios: number[] = [];
        n.children.forEach((child, i) => {
            const repaired = removePaneFromTree(child, paneId);
            if (repaired === null) return;
            keptChildren.push(repaired);
            const r = ratios[i];
            keptRatios.push(typeof r === "number" && Number.isFinite(r) && r > 0 ? r : 1);
        });
        if (keptChildren.length === 0) return null;
        if (keptChildren.length === 1) return keptChildren[0];
        const sum = keptRatios.reduce((a, b) => a + b, 0);
        const normalized = keptRatios.map((r) => r / sum);
        return { ...n, children: keptChildren, ratios: normalized };
    }
    return node;
}

// Does any leaf of this split tree reference `paneId`, by either of the two
// fields removePaneFromTree matches on? Asked BEFORE repairing a region, so a
// tree that has nothing to do with the deleted pane is left byte-for-byte as it
// was: removePaneFromTree rebuilds every split it walks (it renormalizes
// ratios), so its output is almost never equal to the stored text even when it
// dropped nothing at all.
function treeReferencesPane(node: unknown, paneId: string): boolean {
    if (node === null || typeof node !== "object") return false;
    const n = node as Record<string, unknown>;
    if (n.type === "leaf") return n.paneId === paneId || n.terminalPaneId === paneId;
    if (n.type === "split" && Array.isArray(n.children)) {
        return n.children.some((child) => treeReferencesPane(child, paneId));
    }
    return false;
}

// --- tmux targets (SPEC 5.2) ------------------------------------------------
//
// THE one place a terminal pane's tmux target is minted. It used to be two:
// this module minted one for every copied pane in duplicateSession, while the
// desktop client independently built `ch_<devSessionId>_<layoutPaneId>` of its
// own and attached to it without ever writing a row. Two minting sites meant
// the client's ids were the ones that actually reached tmux, and those recycled
// — a layout pane id is `terminal-1`, `terminal-2`, … per Dev Session, so a
// second client machine that had never seen `terminal-2` re-minted it and
// attached the FIRST machine's still-running shell (closing a pane deliberately
// leaves its shell alive). The client now looks its row up here and uses this
// value verbatim, so an identity is minted exactly once, from a row id that is
// never reused, and holds across machines.
//
// Normally both inputs are server-minted UUIDs, so `ch_<devSessionId>_<paneId>`
// is inside the safe set by construction. A database upgraded from an old build
// need not oblige: nothing ever constrained `dev_sessions.id` or
// `terminal_panes.id` to a UUID, and a legacy id may contain a character tmux
// reads as a session/window/pane separator. Throwing there made every terminal
// pane in such a Dev Session impossible to create OR to heal — the client got a
// bare internal error and no shell at all — which is exactly the outcome the v3
// migration's own fallback goes out of its way to avoid.
//
// So the identity degrades instead of failing: the pane row id alone, and
// failing that a fresh UUID. Both are unique and neither is ever reused, which
// is all a target has to be; the value is stored on the row, so it does not
// have to be recomputable from the ids. Only the human-readable link back to
// the Dev Session is lost, and only for a database that predates UUID ids.
function mintTmuxTarget(devSessionId: string, paneId: string): string {
    const target = `ch_${devSessionId}_${paneId}`;
    if (isSafeTmuxTarget(target)) return target;
    const fromPane = `ch_${paneId}`;
    if (isSafeTmuxTarget(fromPane)) return fromPane;
    return `ch_${randomUUID()}`;
}

// Gate for a tmux target supplied by a CLIENT (createTerminalPane /
// updateTerminalPane). Rejected, never rewritten: the client is still there to
// be told, and a silent rewrite would leave it attaching to the session it
// asked for while the row — and therefore every other client, and the kill
// command — named a different one. Tagged invalid-params so the dispatcher
// answers -32602: the payload is at fault, not the server.
function requireSafeTmuxTarget(target: string, method: string): string {
    if (!isSafeTmuxTarget(target)) {
        throw new InvalidParamsError(
            `${method}: tmuxTarget must be 1-${TMUX_TARGET_MAX_LENGTH} characters of [A-Za-z0-9_-]; ` +
                "tmux reads ':' and '.' as session/window/pane separators and rewrites them in " +
                `session names, so ${JSON.stringify(target)} could never name this pane's session`,
        );
    }
    return target;
}

// How long a statement waits for another connection's write lock before giving
// up. SQLite's default is ZERO — the loser of any contention fails instantly —
// and several codeharbord processes genuinely share one database file (see
// serverId()), so the default turns every real collision into a client-visible
// error.
//
// Five seconds is chosen against both ends:
//   * Long enough. Every write here is a handful of indexed row operations
//     inside one BEGIN IMMEDIATE — single-digit milliseconds. Five seconds
//     absorbs hundreds of queued transactions, far more than a handful of
//     daemons can produce, plus the one genuinely slow case (a first-open
//     migration rewriting terminal_panes).
//   * Short enough. A writer that is actually stuck (a crashed process holding
//     a lock, a hung network filesystem) must surface as an error rather than
//     hang the client's RPC forever. The daemon answers requests one at a time,
//     so a stall here also delays the client's heartbeat reply; at 5s the
//     daemon can miss at most one probe of the client's 15s/4-miss heartbeat
//     (src/remote/CodeharbordClient.h), so waiting can never be mistaken for a
//     dead peer. And the heartbeat is the backstop that tears down a truly
//     wedged transport, so this value does not have to guard against infinity.
const DB_BUSY_TIMEOUT_MS = 5000;

// Ceiling on the SERIALIZED bytes of one region's split tree, enforced by
// setLayout on the way IN. The stored tree is free-form client JSON and nothing
// bounded it, which is the one input to this store that a client can grow
// without limit.
//
// WHAT BREAKS ABOVE IT. `workspace.list` embeds every stored tree of every
// session in a single reply, and responseLine (remote/src/codeharbord.ts)
// refuses a reply past MAX_LINE_BYTES — 16 MiB — with RPC_RESOURCE_LIMIT rather
// than writing an over-cap frame that would drop the transport. So one
// oversized stored tree does not kill the connection, it does something quieter
// and worse: `workspace.list` fails from then on, for EVERY session on the
// server, and the workspace can never be loaded again without hand-editing the
// session_layouts row out of the database. The write is the only place this can
// still be refused while the user is there to be told, so it is refused there.
//
// WHY 256 KiB. The cap has to clear the largest tree a client can legitimately
// draw while staying small enough that a plausible WORKSPACE still fits one
// frame — and the second constraint is the binding one, because nothing bounds
// how many Dev Sessions a workspace holds (see below). Measured against
// SplitNode::tryToJson (src/models/SplitTree.cpp), whose leaf carries `type`, a
// `paneId`, an optional `url`, an optional `terminalPaneId` and an optional
// `customTitle` bounded by kMaxCustomTitleLength = 128 UTF-16 units:
//
//     4 panes, 120-char URLs, no titles                     1.3 KiB
//    16 panes, 256-char URLs, 40-char titles                8.5 KiB
//    32 panes, 256-char URLs, max titles   (realistic max) 19.8 KiB
//    32 panes, 2 KiB URLs,    max titles   (aggressive)    75.8 KiB
//  1024 panes, 2 KiB URLs, fully escaped max titles         3.0 MiB
//
// 256 KiB is 12.9x the realistic maximum and 3.4x the aggressive one, so no
// layout a user can produce — 32 tiles in one region is already a wall of
// panes — comes near it. The 1024-pane row is the pathological case, about
// eight pixels per pane at 4K; refusing it is the entire point.
//
// And the frame still holds a real workspace. A listed session costs ~1.8 KiB
// of rows (measured: the session plus two viewer and two terminal panes) on top
// of its two trees, so 200 sessions carrying REALISTIC worst-case layouts come
// to ~8.1 MiB, half the frame; ~396 such sessions is where it runs out, and
// that ceiling belongs to the session count, not to this constant. To brick
// `workspace.list` a workspace would need 31 Dev Sessions holding a 256 KiB
// layout in BOTH regions at once. A 4 MiB cap — the first number tried here —
// failed exactly this test: TWO such sessions already exceeded the frame.
//
// It still does NOT make `workspace.list` unconditionally safe, and cannot:
// neither this schema nor the client bounds the number of Dev Sessions, so a
// large enough workspace of ordinary trees can outgrow the frame no matter what
// this value is. Fixing that residual case would mean bounding the READ, i.e.
// omitting stored data from a listing, which is a worse contract than refusing
// absurd input on the way in.
export const MAX_LAYOUT_TREE_BYTES = 256 * 1024;

/**
 * Apply the connection-level settings every workspace connection needs, in the
 * order they need to be applied. Call this on EVERY connection to a workspace
 * database — the daemon's, and any raw DatabaseSync a test opens — immediately
 * after constructing it and before running any statement: DDL contends too, so
 * a second daemon starting while the first is migrating must wait rather than
 * fail, and that window opens before `migrate()` is ever called.
 */
export function applyConnectionPragmas(db: DatabaseSync): void {
    db.exec(`PRAGMA busy_timeout = ${DB_BUSY_TIMEOUT_MS};`);
    try {
        // Write-ahead logging, because this is a multi-PROCESS workload:
        // in WAL a reader never blocks the writer and the writer never blocks
        // readers, so most of the contention above is removed at the source
        // instead of merely waited out. The mode is recorded IN THE DATABASE
        // FILE, so a database created before this change is converted on its
        // first open by a build carrying this code and stays WAL afterwards —
        // including for older codeharbord builds, which read it fine (every
        // SQLite since 3.7.0 understands WAL) and simply do not benefit.
        //
        // Wrapped because WAL needs a shared-memory file (-shm) beside the
        // database, which some network filesystems cannot provide, and a
        // workspace can plausibly live on one. On failure node:sqlite THROWS an
        // ERR_SQLITE_ERROR (verified: "attempt to write a readonly database"
        // when the directory is not writable) and the journal mode is left at
        // whatever it was. Rollback journalling plus the busy timeout above is
        // exactly the pre-WAL behaviour, which is correct if slower under
        // contention — a daemon that refuses to start would be far worse.
        //
        // A no-op for ":memory:": SQLite reports journal_mode "memory" there and
        // silently ignores the request (verified) — no throw, no output, so the
        // in-memory tests are unaffected.
        db.exec("PRAGMA journal_mode = WAL;");
    } catch {
        // Deliberately not fatal, and deliberately not logged: stderr is the
        // daemon's log channel and this would print on every open of a
        // workspace whose filesystem simply cannot do WAL.
    }
}

// SQLITE_BUSY / SQLITE_LOCKED, i.e. the busy timeout above ran out and this
// write never touched the database. Reported as its own RPC code rather than
// "internal error": nothing malfunctioned, the caller merely lost a race and
// may retry. Extended result codes (SQLITE_BUSY_SNAPSHOT = 517, and friends)
// carry the primary code in their low byte, hence the mask.
export function isDatabaseBusy(err: unknown): boolean {
    if (!(err instanceof Error) || !("errcode" in err)) return false;
    const errcode: unknown = err.errcode;
    if (typeof errcode !== "number") return false;
    const primary = errcode & 0xff;
    return primary === 5 || primary === 6;
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
    try {
        // Before the foreign-key pragma, before migrate(), before the index
        // DDL: every one of those is a statement that can contend with another
        // daemon, so the busy timeout has to already be in force.
        applyConnectionPragmas(db);
        db.exec("PRAGMA foreign_keys = ON;");
        migrate(db);
        // After migrate(), never before: the indexed tables must exist first.
        db.exec(indexesSql);
    } catch (err) {
        // Setup failed (most commonly: a database written by a NEWER build, or
        // a corrupt file). The connection is already open at this point, so
        // leaving it dangling leaks a file descriptor and keeps SQLite's lock
        // on the file for the rest of the process — the next open attempt, and
        // any other process, then fails for a second, unrelated reason. Close
        // it and report the ORIGINAL error.
        try {
            db.close();
        } catch {
            // Nothing useful to do; the original failure is what matters.
        }
        throw err;
    }
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
    // v3 makes a terminal pane's identity enforceable: unique tmux_target and
    // unique (dev_session_id, name), repairing legacy collisions first.
    { version: 3, apply: (db) => migrateTerminalPaneIdentity(db) },
    // v4 removes the unique (dev_session_id, name) constraint again. A pane
    // label is not a terminal identity, so multiple rows may legitimately
    // reuse it after a pane is closed and a new one is created.
    { version: 4, apply: (db) => migrateDropTerminalPaneAddressUnique(db) },
    // v5 adds the workspace-owned Dev Session pin bit. ALTER TABLE is
    // intentionally explicit: CREATE TABLE IF NOT EXISTS cannot add a column
    // to a database that already has dev_sessions, and every old row must stay
    // visible as unpinned. Fresh databases already received the column from
    // schema.sql in v1, so this step checks before altering them.
    {
        version: 5,
        apply: (db) => {
            const columns = db
                .prepare("PRAGMA table_info(dev_sessions)")
                .all() as unknown as Array<{ name: string }>;
            if (!columns.some((column) => column.name === "pinned")) {
                db.exec(
                    "ALTER TABLE dev_sessions ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0",
                );
            }
        },
    },
];

// Quote a SQL IDENTIFIER: wrap it in double quotes and double any embedded
// double quote, which is the only escape SQL defines. Not JSON.stringify —
// that backslash-escapes a quote, and a backslash means nothing to SQLite, so
// an index named `we"ird` came out as the syntax error `"we\"ird"` rather than
// as a reference to that index. The names below are read back out of the
// database's own catalogue, so this is the layer that has to be right about
// them: a value that arrives from a query is not a literal we control.
function quoteIdentifier(name: string): string {
    return `"${name.replace(/"/g, '""')}"`;
}

// The unique index over exactly `columns` of `terminal_panes`, in order, or
// null when the table does not enforce that combination. A table-level UNIQUE
// shows up as an sqlite_autoindex, a migration-added one under its own name,
// and both answer PRAGMA index_list. Asking rather than using CREATE UNIQUE
// INDEX IF NOT EXISTS: that only matches on the index NAME, so on a FRESH
// database (where step v1 applied a schema.sql whose CREATE TABLE already
// carries the constraint) it would add a second index over the same columns
// and charge every write for it twice. The NAME, not just a yes/no, because v4
// has to remove one of these again and how it does that depends on which kind
// it is.
function terminalPaneUniqueIndexOver(db: DatabaseSync, columns: string[]): string | null {
    const indexes = db.prepare("PRAGMA index_list(terminal_panes)").all() as unknown as Array<{
        name: string;
        unique: number;
    }>;
    for (const index of indexes) {
        if (index.unique !== 1) continue;
        const found = db
            .prepare(`PRAGMA index_info(${quoteIdentifier(index.name)})`)
            .all() as unknown as Array<{ name: string | null }>;
        if (found.length !== columns.length) continue;
        if (found.every((c, i) => c.name === columns[i])) return index.name;
    }
    return null;
}

// Schema v3: make a terminal pane's identity unique, in both of the ways it is
// addressed.
//
//   * tmux_target. Two panes sharing one attach the SAME remote tmux session,
//     so every keystroke typed in one appears in the other and the two fight
//     over the terminal size.
//
//   * (dev_session_id, name). In v3, `name` was treated as the layout pane id,
//     and the client resolved a layout slot to its row through this pair. Two
//     rows for one slot meant two minted targets, hence two tmux sessions where
//     the user had one pane — which the tmux_target rule could not catch because
//     those two targets were not equal.
//
// Nothing stopped either before. Fresh databases get both from schema.sql's
// CREATE TABLE; this step is what an EXISTING v1/v2 database goes through, and
// SQLite has no ALTER TABLE ADD CONSTRAINT, so each constraint arrives as a
// unique INDEX instead (identical enforcement — a table-level UNIQUE is itself
// implemented as one).
//
// Four phases, in this order, because each can create work for the next:
//
//  1. REPAIR targets. A stored target containing `:` or `.` never named a real
//     session: tmux rewrites both to `_` when it creates one (see tmuxSafeName).
//     The stored value is corrected to the name tmux actually used, so the pane
//     finally points at its own long-running shell instead of missing it.
//
//  2. DE-DUPLICATE targets. Phase 1 can itself collide two rows (`a.b` and
//     `a_b`), and a pre-v3 database may simply contain duplicates. The FIRST row
//     by rowid keeps the contested target — it is the oldest, so it is the one
//     whose shell the longest-lived pane is attached to — and every later row is
//     re-minted onto its own canonical `ch_<dev_session_id>_<id>`. Re-minting
//     rather than nulling is deliberate: a null target is a pane with no shell
//     at all, whereas a fresh target is a pane that opens its own on the next
//     attach, which is what the user asked for when they made two panes. Should
//     even the canonical mint be taken (only reachable if some other row was
//     hand-written to exactly that string), the row falls back to NULL — SQLite
//     permits any number of NULLs under UNIQUE — and the pane mints a target on
//     its next attach rather than blocking the whole upgrade.
//
//  3. DE-DUPLICATE names, by the SAME rule and for the same reason: the oldest
//     row keeps the contested slot name, because it is the one whose shell has
//     been running longest and therefore the one a client resolving that slot
//     should keep getting. Each later row is RENAMED — to
//     "<name>-dup-<short row id>", which is unique because the row id is — and
//     kept, rather than deleted. It still owns a live tmux session and the
//     user's running processes; deleting it would strand them under a name
//     nothing can ever look up again.
//
//     Still load-bearing even though v4 immediately makes duplicate labels
//     legal again, and not merely as history: phase 4 below creates
//     UNIQUE (dev_session_id, name), and that statement FAILS on a database
//     that still holds two rows for one slot. Both steps run inside the
//     migration runner's single transaction, so dropping this repair would
//     roll the whole upgrade back and leave a v2 database unopenable. (The
//     one-shot legacy by-label lookup is deterministic without it — it takes
//     the oldest match by rowid — so it is phase 4, not that lookup, that
//     this repair is actually for.)
//
//  4. CONSTRAIN, skipping whichever constraint the table already carries.
function migrateTerminalPaneIdentity(db: DatabaseSync): void {
    const targeted = db
        .prepare(
            "SELECT rowid AS rowid, id, dev_session_id, tmux_target FROM terminal_panes WHERE tmux_target IS NOT NULL ORDER BY rowid",
        )
        .all() as unknown as Array<{
        rowid: number;
        id: string;
        dev_session_id: string;
        tmux_target: string;
    }>;
    const assignTarget = db.prepare("UPDATE terminal_panes SET tmux_target = ? WHERE rowid = ?");
    const takenTargets = new Set<string>();
    for (const row of targeted) {
        const repaired = tmuxSafeName(row.tmux_target);
        let next: string | null = repaired;
        if (takenTargets.has(repaired)) {
            // The canonical mint, spelled out here rather than delegated to
            // mintTmuxTarget, because this loop needs the one thing that
            // function cannot know: whether the name is already claimed by an
            // earlier row in this same upgrade. Nothing ever constrained a
            // legacy `terminal_panes.id` or `dev_session_id` to a UUID, so
            // `ch_<dev_session_id>_<id>` can itself be unusable as a tmux
            // session name. Falling back to NULL costs the pane only its stored
            // session name; it mints a fresh one on its next attach.
            const minted = `ch_${row.dev_session_id}_${row.id}`;
            next = isSafeTmuxTarget(minted) && !takenTargets.has(minted) ? minted : null;
        }
        if (next !== null) takenTargets.add(next);
        if (next !== row.tmux_target) assignTarget.run(next, row.rowid);
    }

    const named = db
        .prepare(
            "SELECT rowid AS rowid, id, dev_session_id, name FROM terminal_panes ORDER BY rowid",
        )
        .all() as unknown as Array<{
        rowid: number;
        id: string;
        dev_session_id: string;
        name: string;
    }>;
    const assignName = db.prepare("UPDATE terminal_panes SET name = ? WHERE rowid = ?");
    const takenNames = new Set<string>();
    for (const row of named) {
        const address = `${row.dev_session_id}/${row.name}`;
        if (!takenNames.has(address)) {
            takenNames.add(address);
            continue;
        }
        // The row id is a UUID, so its first segment is unique among these rows
        // in every practical sense; the loop below closes even that gap.
        let renamed = `${row.name}-dup-${row.id.slice(0, 8)}`;
        for (let n = 2; takenNames.has(`${row.dev_session_id}/${renamed}`); n++) {
            renamed = `${row.name}-dup-${row.id.slice(0, 8)}-${n}`;
        }
        takenNames.add(`${row.dev_session_id}/${renamed}`);
        assignName.run(renamed, row.rowid);
    }

    if (terminalPaneUniqueIndexOver(db, ["tmux_target"]) === null) {
        db.exec(
            "CREATE UNIQUE INDEX terminal_panes_tmux_target_unique ON terminal_panes (tmux_target)",
        );
    }
    if (terminalPaneUniqueIndexOver(db, ["dev_session_id", "name"]) === null) {
        db.exec(
            "CREATE UNIQUE INDEX terminal_panes_address_unique ON terminal_panes (dev_session_id, name)",
        );
    }
}

// Schema v4: undo v3's UNIQUE (dev_session_id, name).
//
// v3 read `name` as a pane's ADDRESS — the client resolved a layout slot to its
// row through (dev_session_id, name), so two rows for one slot were two tmux
// sessions for one pane. That premise is gone. A terminal is identified by its
// row id, which the layout leaf now carries (SplitNode::terminalPaneId), and
// `name` is only a slot LABEL. Labels are minted per client and are recycled:
// closing a pane deliberately keeps its row and its remote shell alive, so its
// label stays taken while the layout stops showing it, and the very next split
// legitimately mints a NEW row wanting the SAME label. Under v3 that insert
// failed, which is why the rule has to go rather than merely stop being relied
// on. Duplicated labels are now expected and harmless: nothing resolves by
// label except the one-shot legacy path, which takes the oldest match.
//
// Two shapes to remove, because v3 could arrive either way:
//
//   * A NAMED index, on a database upgraded from v2 — dropped directly.
//
//   * An sqlite_autoindex, on a database first created while schema.sql still
//     declared the table-level UNIQUE. SQLite cannot drop a table constraint,
//     so the table is rebuilt without it and the rows are copied across. The
//     UNIQUE (tmux_target) rule is re-declared on the new table: THAT one still
//     holds, and dropping the old table takes its index with it.
function migrateDropTerminalPaneAddressUnique(db: DatabaseSync): void {
    const index = terminalPaneUniqueIndexOver(db, ["dev_session_id", "name"]);
    if (index === null) return;
    if (!index.startsWith("sqlite_autoindex_")) {
        db.exec(`DROP INDEX ${quoteIdentifier(index)}`);
        return;
    }
    db.exec(`
        CREATE TABLE terminal_panes_v4 (
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
            UNIQUE (tmux_target)
        );
        INSERT INTO terminal_panes_v4
            SELECT id, server_id, dev_session_id, name, working_directory, tmux_target,
                   startup_command, harness, position, created_at, updated_at
            FROM terminal_panes;
        DROP TABLE terminal_panes;
        ALTER TABLE terminal_panes_v4 RENAME TO terminal_panes;
    `);
}

// Is `version` already at or past what this build targets — i.e. is there no
// upgrade work to do? A version ABOVE the target is a database written by a
// NEWER build than this one, and that is refused rather than used: the
// migrations this build is missing may have renamed or dropped columns it still
// selects, and the failures would surface much later as confusing per-statement
// SQL errors on a live workspace. Equality is the normal case and passes
// through silently.
function isSchemaUpToDate(version: number): boolean {
    if (version > WORKSPACE_SCHEMA_VERSION) {
        throw new Error(
            `workspace database schema version ${version} is newer than this build supports (${WORKSPACE_SCHEMA_VERSION})`,
        );
    }
    return version === WORKSPACE_SCHEMA_VERSION;
}

function migrate(db: DatabaseSync): void {
    // Cheap pre-check with no lock held: an already-current database is by far
    // the common case, and taking the write lock on every open would serialise
    // every daemon start against every other one.
    if (isSchemaUpToDate(schemaVersion(db))) return;
    // Foreign-key enforcement is turned OFF for the duration of the upgrade,
    // and only for the upgrade.
    //
    // Schema v4 has to REBUILD terminal_panes (SQLite cannot drop a table-level
    // UNIQUE), and SQLite's documented rebuild procedure requires it: copying
    // the rows into the replacement table re-checks every foreign key, whereas
    // an existing database is never re-checked — a constraint is evaluated only
    // when a row is written. So a legacy database holding a pane row whose
    // dev_session is gone opens fine today, and used to fail the rebuild's
    // INSERT, roll the whole upgrade back, and become permanently unopenable by
    // this build. Off, the rebuild is a pure copy: the orphan survives exactly
    // as it survives every open today. That is deliberately preferred over
    // deleting the row (loses the user's pane) or re-parenting it (invents a
    // parent nobody chose), and over reporting a clear diagnostic, which would
    // still leave the workspace unopenable.
    //
    // It must be set HERE, before BEGIN: `PRAGMA foreign_keys` is a documented
    // no-op inside a transaction, which is exactly why the rebuild step cannot
    // do it for itself. Nothing between here and COMMIT inserts a row that
    // needs checking — the steps only run DDL and repair rows already present —
    // and the previous setting is restored in `finally`, so every ordinary
    // write afterwards is enforced again.
    const foreignKeysWereOn =
        (db.prepare("PRAGMA foreign_keys").get() as { foreign_keys: number } | undefined)
            ?.foreign_keys === 1;
    db.exec("PRAGMA foreign_keys = OFF");
    try {
        // One transaction for the whole upgrade, so a step that throws half way
        // leaves the database exactly as it was rather than at a version nobody
        // wrote migrations for. (schema.sql's leading `PRAGMA foreign_keys = ON`
        // is a no-op inside a transaction, so it cannot re-enable enforcement
        // behind the line above either.)
        db.exec("BEGIN IMMEDIATE");
        try {
            // Re-read the stored version UNDER the write lock. The pre-check
            // above ran with no lock at all, so a second daemon may have
            // completed the entire upgrade in between; re-applying every step on
            // top of that only happens to be harmless because each step is
            // idempotent today, and the next one added need not be.
            const from = schemaVersion(db);
            if (!isSchemaUpToDate(from)) {
                for (const step of MIGRATIONS) {
                    if (step.version > from && step.version <= WORKSPACE_SCHEMA_VERSION) {
                        step.apply(db);
                    }
                }
                // schema.sql seeds the version row with INSERT OR IGNORE, which
                // cannot advance an already-present row. Record the target
                // version explicitly so a WORKSPACE_SCHEMA_VERSION bump is
                // persisted (and migrate stops re-running steps on every open)
                // rather than the stored version silently drifting from the
                // DDL's hard-coded literal.
                db.prepare("UPDATE schema_version SET version = ? WHERE id = 1").run(
                    WORKSPACE_SCHEMA_VERSION,
                );
            }
            db.exec("COMMIT");
        } catch (err) {
            try {
                db.exec("ROLLBACK");
            } catch {
                // Already rolled back by the failure itself; the original error
                // is the one worth reporting.
            }
            throw err;
        }
    } finally {
        if (foreignKeysWereOn) db.exec("PRAGMA foreign_keys = ON");
    }
}

function schemaVersion(db: DatabaseSync): number {
    try {
        const row = db
            .prepare("SELECT version FROM schema_version WHERE id = 1")
            .get() as { version: number } | undefined;
        return row?.version ?? 0;
    } catch (err) {
        // Exactly ONE failure means "unmigrated": the schema_version table is
        // not there yet. Every other failure — a file that is not a SQLite
        // database at all, a malformed image, a lock that outlasted the busy
        // timeout — used to be swallowed here and answered as version 0, which
        // sent migrate() on to BEGIN IMMEDIATE and the full schema.sql and
        // buried the real cause under whichever of those failed second. Report
        // it instead.
        if (err instanceof Error && /no such table/i.test(err.message)) return 0;
        throw err;
    }
}

// The compiled form of the nested listing (see Workspace.listStatements).
interface ListStatements {
    groups: StatementSync;
    groupsPinned: StatementSync;
    sessions: StatementSync;
    sessionsPinned: StatementSync;
    viewerPanes: StatementSync;
    terminalPanes: StatementSync;
    layouts: StatementSync;
}

// Every table whose rows carry a `position` within a scope, mapped to the
// column naming that scope. Literal types rather than plain strings because
// both halves are interpolated into SQL — an identifier cannot be a bound
// parameter, so the type is what keeps a caller-supplied name from ever
// reaching the query text.
//
// A MAP from table to column, not two independent unions: with independent
// unions `packOrder("groups", "dev_session_id", …)` type-checked happily and
// then failed at runtime with SQLite's "no such column: dev_session_id",
// mid-transaction, on a scope that has no such column at all. Now only the one
// column each table actually has is accepted.
interface OrderedScope {
    groups: "server_id";
    dev_sessions: "group_id";
    viewer_panes: "dev_session_id";
    terminal_panes: "dev_session_id";
}
type OrderedTable = keyof OrderedScope;

// A single workspace database connection with all CRUD operations (SPEC 4.2,
// 11.1). Booleans are stored as 0/1 integers; deletes cascade manually because
// the schema's foreign keys use the default NO ACTION (they reject, not
// cascade). Every mutation carries server_id per SPEC 3.5.
export class Workspace {
    readonly db: DatabaseSync;
    // Memoized server_identity row (see serverId()); the row never changes.
    private cachedServerId: string | undefined;
    // Whether this connection is already inside a BEGIN; see transaction(). A
    // flag rather than a counter: a nested call never opens a transaction of
    // its own, so the value only ever toggles between the two states and a
    // counter invited a reader to expect otherwise.
    private inTransaction = false;
    // ...and whether that BEGIN is list()'s DEFERRED read rather than a
    // BEGIN IMMEDIATE. The distinction matters because a deferred transaction
    // holds no write lock: under WAL, a write issued inside one has to upgrade,
    // and if another connection committed in the meantime SQLite answers
    // SQLITE_BUSY_SNAPSHOT, which the busy timeout cannot wait out and only a
    // full restart of the transaction can resolve. Nothing writes from the read
    // path today; transaction() checks this so that a future caller that does
    // fails immediately and legibly instead of intermittently in production.
    private inReadTransaction = false;
    // The nested listing's seven statements (all/pinned groups and sessions,
    // plus three pane/layout lookups), compiled once per connection.
    //
    // list() runs one query for the groups, one per group for its sessions and
    // three per session for its panes and layouts, and every one of them is the
    // same SQL every time — so re-preparing them per call charges the whole read
    // path a fresh SQL compilation per row of the sidebar. duplicateSession and
    // packOrder already hoist their statements out of their loops for exactly
    // this reason; this is the same fix on the side that runs far more often.
    //
    // Lazy rather than prepared in the constructor: a connection that only ever
    // writes should not pay for them, and prepare() needs the tables to exist,
    // which is a promise the constructor cannot make for a caller that did not
    // come through openWorkspace().
    private listStatements: ListStatements | undefined;

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
     *
     * The mint runs only when the READ found nothing. Minting unconditionally
     * and letting OR IGNORE absorb the duplicate is equally correct, but it
     * makes reading an identity that has existed for months a WRITE: it takes
     * the database's write lock on the first `server.info` of every daemon
     * start — serialising against a second daemon's migration for no reason —
     * and it fails outright on a workspace that is merely readable (a
     * read-only mount, a full disk), where the answer was sitting in the file
     * all along.
     */
    serverId(): string {
        if (this.cachedServerId !== undefined) return this.cachedServerId;
        const read = this.db.prepare("SELECT server_id FROM server_identity WHERE id = 1");
        let row = read.get() as { server_id: string } | undefined;
        if (!row) {
            this.db
                .prepare(
                    "INSERT OR IGNORE INTO server_identity (id, server_id, created_at) VALUES (1, ?, ?)",
                )
                .run(randomUUID(), Date.now());
            row = read.get() as { server_id: string } | undefined;
        }
        if (!row) throw new Error("server identity missing after mint");
        // Immutable once written, so caching it is safe for this connection's
        // lifetime; every later server.info answers without touching the DB.
        this.cachedServerId = row.server_id;
        return this.cachedServerId;
    }

    // --- Groups -------------------------------------------------------------

    // Wrapped in a transaction because it is a read-then-write: nextPosition
    // reads the scope's current maximum position and the INSERT then claims
    // "one past" it. Two connections doing that at the same time (several
    // codeharbord processes may share one database file — see serverId) both
    // read the same maximum and both insert at the same position. Every listing
    // query orders by `position, id`, so the tie is then broken by UUID, i.e.
    // at random: the group the user just created can appear anywhere. The
    // BEGIN IMMEDIATE inside transaction() takes the write lock before the
    // read, so the second connection waits (or fails loudly) instead.
    createGroup(params: CreateGroupParams): Group {
        return this.transaction(() => {
            const id = randomUUID();
            const ts = Date.now();
            // ALWAYS the append slot, never `params.position` itself. An
            // explicit position is a placement REQUEST, not a column value:
            // placeAt below truncates and clamps it and then re-packs the whole
            // scope, so whatever goes in here is overwritten a statement later
            // — but only if the INSERT gets that far. Binding the raw request
            // meant a non-finite in-process index reached SQLite as NULL and
            // failed the create with "NOT NULL constraint failed: groups.position",
            // the one input placeAt's clamp is written to absorb.
            const position = this.nextPosition("groups", "server_id", params.serverId);
            this.db
                .prepare(
                    "INSERT INTO groups (id, server_id, name, position, collapsed, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                )
                .run(id, params.serverId, params.name, position, params.collapsed ? 1 : 0, ts, ts);
            // See placeAt: this is what makes the new row really be the Nth,
            // with no two rows sharing a position.
            if (params.position !== undefined) {
                this.placeAt("groups", "server_id", params.serverId, id, params.position, ts);
            }
            return this.getGroup(id);
        });
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
            const collapsed = params.collapsed ?? current.collapsed;
            const ts = Date.now();
            this.db
                .prepare("UPDATE groups SET name = ?, collapsed = ?, updated_at = ? WHERE id = ?")
                .run(name, collapsed ? 1 : 0, ts, params.id);
            if (params.position !== undefined) {
                this.placeAt("groups", "server_id", current.serverId, params.id, params.position, ts);
            }
            return this.getGroup(params.id);
        });
    }

    // Delete a group and everything under it. Children go first, and deepest
    // first, because the schema's foreign keys REJECT (NO ACTION) rather than
    // cascade — a group with sessions still attached cannot be deleted.
    //
    // Set-based, not a loop over the group's sessions: the per-session version
    // compiled four fresh statements for EVERY session in the group, which is
    // the same cost duplicateSession and packOrder already hoist out of their
    // loops. Five statements now, whatever the group holds.
    //
    // Deleting a group that does not exist is a successful no-op, deliberately:
    // a client that retries a delete after a dropped connection must not be
    // told the second attempt failed.
    deleteGroup(params: { id: string }): { ok: true } {
        return this.transaction(() => {
            for (const table of ["viewer_panes", "terminal_panes", "session_layouts"] as const) {
                this.db
                    .prepare(
                        `DELETE FROM ${table} WHERE dev_session_id IN (SELECT id FROM dev_sessions WHERE group_id = ?)`,
                    )
                    .run(params.id);
            }
            this.db.prepare("DELETE FROM dev_sessions WHERE group_id = ?").run(params.id);
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

    // Read-then-write (parent lookup + nextPosition + INSERT), so it runs in a
    // transaction for the same reason createGroup does.
    createSession(params: CreateSessionParams): Session {
        return this.transaction(() => {
            const id = randomUUID();
            const ts = Date.now();
            // SPEC 3.5: a child's server_id is authoritative from its parent row,
            // never the client-supplied param — otherwise a mismatched serverId
            // could surface this session under a foreign server's group. The wire
            // param is still accepted (C1) but overridden here.
            const serverId = this.parentServerId("groups", params.groupId);
            // The append slot, never the raw request — see createGroup.
            const position = this.nextPosition("dev_sessions", "group_id", params.groupId);
            this.db
                .prepare(
                    "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, default_working_directory, task_description, position, archived, pinned, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
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
                    params.pinned ? 1 : 0,
                    ts,
                    ts,
                );
            if (params.position !== undefined) {
                this.placeAt("dev_sessions", "group_id", params.groupId, id, params.position, ts);
            }
            return this.getSession(id);
        });
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
            const archived = params.archived ?? current.archived;
            const pinned = params.pinned ?? current.pinned;
            const ts = Date.now();
            this.db
                .prepare(
                    "UPDATE dev_sessions SET name = ?, repository_root = ?, default_working_directory = ?, task_description = ?, archived = ?, pinned = ?, updated_at = ? WHERE id = ?",
                )
                .run(
                    name,
                    repositoryRoot,
                    defaultWorkingDirectory,
                    taskDescription,
                    archived ? 1 : 0,
                    pinned ? 1 : 0,
                    ts,
                    params.id,
                );
            if (params.position !== undefined) {
                this.placeAt("dev_sessions", "group_id", current.groupId, params.id, params.position, ts);
            }
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
            // Unconditionally, not only when the SESSION's own server_id
            // changed. The two are not the same test: a legacy or hand-edited
            // database can hold a session that already carries the target
            // server while its panes and layouts carry a different one, and
            // gating on the session made the move leave that drift in place —
            // the pane rows then stayed invisible to the very listing the move
            // was supposed to bring them into. The write is scoped by
            // dev_session_id and touches at most a handful of rows.
            for (const table of ["viewer_panes", "terminal_panes", "session_layouts"] as const) {
                this.db
                    .prepare(`UPDATE ${table} SET server_id = ? WHERE dev_session_id = ? AND server_id <> ?`)
                    .run(targetServerId, params.id, targetServerId);
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
            // The parent group is authoritative for the new row's server. A
            // legacy database may contain a session whose server_id drifted from
            // its group; copying that drift would make the duplicate invisible
            // to the group's server-scoped listing.
            const targetServerId = this.parentServerId("groups", source.group_id);
            const newSessionId = randomUUID();
            const ts = Date.now();
            const position = this.nextPosition("dev_sessions", "group_id", source.group_id);
            this.db
                .prepare(
                    "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, default_working_directory, task_description, position, archived, pinned, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                )
                .run(
                    newSessionId,
                    targetServerId,
                    source.group_id,
                    source.name,
                    source.repository_root,
                    source.default_working_directory,
                    source.task_description,
                    position,
                    source.archived,
                    source.pinned,
                    ts,
                    ts,
                );

            // One prepared statement per table, reused for every copied row: a
            // fresh prepare() inside the loop compiled the identical SQL again
            // for each pane and left a statement handle per iteration for the
            // garbage collector to finalize. The copies take the NEW session's
            // server_id (derived from its parent group) rather than each child
            // row's own, so a duplicate always satisfies SPEC 3.5's "a row
            // shares its ancestors' server" invariant even if the source data
            // predates that rule.
            // ONE map for both pane tables, not one per region. Every key is a
            // server-minted UUID, so viewer and terminal ids can never collide,
            // and a leaf is then remapped by what it actually REFERENCES rather
            // than by which region it happens to be stored in. Layout trees are
            // client-authored: a malformed client can file a viewer leaf under
            // the terminal region, and with a per-region map that leaf's paneId
            // was not found, was left alone, and the DUPLICATE's tree went on
            // pointing at the ORIGINAL session's pane row — a cross-session
            // reference this whole method exists to avoid.
            //
            // A Map, not a plain object: the keys are pane ids read out of a
            // CLIENT-authored split tree, and on an object literal a leaf whose
            // paneId is "constructor" or "toString" would look up a function
            // inherited from Object.prototype, which JSON.stringify then drops
            // — silently deleting the field from the copied leaf.
            const idMap = new Map<string, string>();
            const viewerRows = this.db
                .prepare("SELECT * FROM viewer_panes WHERE dev_session_id = ? ORDER BY position, id").all(params.id) as unknown as ViewerPaneRow[];
            const insertViewer = this.db.prepare(
                "INSERT INTO viewer_panes (id, server_id, dev_session_id, url, handler, title, position, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            );
            for (const v of viewerRows) {
                const newId = randomUUID();
                idMap.set(v.id, newId);
                insertViewer.run(
                    newId,
                    targetServerId,
                    newSessionId,
                    v.url,
                    v.handler,
                    v.title,
                    v.position,
                    ts,
                    ts,
                );
            }

            const terminalRows = this.db
                .prepare("SELECT * FROM terminal_panes WHERE dev_session_id = ? ORDER BY position, id").all(params.id) as unknown as TerminalPaneRow[];
            const insertTerminal = this.db.prepare(
                "INSERT INTO terminal_panes (id, server_id, dev_session_id, name, working_directory, tmux_target, startup_command, harness, position, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            );
            for (const t of terminalRows) {
                const newId = randomUUID();
                idMap.set(t.id, newId);
                const tmuxTarget = mintTmuxTarget(newSessionId, newId);
                insertTerminal.run(
                    newId,
                    targetServerId,
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
                .prepare("SELECT * FROM session_layouts WHERE dev_session_id = ? ORDER BY region").all(params.id) as unknown as SessionLayoutRow[];
            const insertLayout = this.db.prepare(
                "INSERT INTO session_layouts (id, server_id, dev_session_id, region, tree, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            );
            for (const l of layoutRows) {
                // A stored tree that no longer parses must not abort the whole
                // duplicate. Everywhere else a corrupt blob self-heals to "this
                // region has no layout" (getLayout / getLayouts, RW14); here it
                // used to throw out of JSON.parse, roll the transaction back,
                // and make "Duplicate Dev Session" fail outright for a session
                // whose panes are all perfectly fine. Skip the region instead:
                // the copy comes up with a default layout, exactly as the
                // original already renders.
                let parsed: unknown;
                try {
                    parsed = JSON.parse(l.tree);
                } catch {
                    console.error(
                        `workspace: skipping unparseable layout tree while duplicating dev_session_id=${params.id} region=${l.region}`,
                    );
                    continue;
                }
                const tree = JSON.stringify(remapPaneIds(parsed, idMap));
                insertLayout.run(randomUUID(), targetServerId, newSessionId, l.region, tree, ts, ts);
            }

            return this.sessionNode(newSessionId);
        });
    }

    // --- Viewer panes -------------------------------------------------------

    // Read-then-write (parent lookup + nextPosition + INSERT), so it runs in a
    // transaction for the same reason createGroup does.
    createViewerPane(params: CreateViewerPaneParams): ViewerPane {
        return this.transaction(() => {
            const id = randomUUID();
            const ts = Date.now();
            // SPEC 3.5: server_id is derived from the parent session, not trusted
            // from the param, so a mismatched serverId cannot detach this pane from
            // its session's server.
            const serverId = this.parentServerId("dev_sessions", params.devSessionId);
            // The append slot, never the raw request — see createGroup.
            const position = this.nextPosition("viewer_panes", "dev_session_id", params.devSessionId);
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
            if (params.position !== undefined) {
                this.placeAt(
                    "viewer_panes",
                    "dev_session_id",
                    params.devSessionId,
                    id,
                    params.position,
                    ts,
                );
            }
            return this.getViewerPane(id);
        });
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
            const ts = Date.now();
            this.db
                .prepare("UPDATE viewer_panes SET url = ?, handler = ?, title = ?, updated_at = ? WHERE id = ?")
                .run(url, handler, title, ts, params.id);
            if (params.position !== undefined) {
                this.placeAt(
                    "viewer_panes",
                    "dev_session_id",
                    current.devSessionId,
                    params.id,
                    params.position,
                    ts,
                );
            }
            return this.getViewerPane(params.id);
        });
    }

    // Delete a viewer pane, then repair every stored split layout for its
    // session so no leaf still references it. Layout trees are client-authored,
    // so a malformed client can put a viewer id in the terminal region; the
    // server's integrity guarantee must cover both regions.
    //
    // Deleting a pane that is not there is a successful no-op (nothing to
    // delete, and no session to repair layouts for), on purpose: a client
    // retrying after a dropped connection must not be told its second attempt
    // failed. Both regions are repaired against ONE timestamp, so a delete that
    // touches both leaves them agreeing on when it happened.
    deleteViewerPane(params: { id: string }): { ok: true } {
        return this.transaction(() => {
            const row = this.db
                .prepare("SELECT dev_session_id FROM viewer_panes WHERE id = ?")
                .get(params.id) as { dev_session_id: string } | undefined;
            this.db.prepare("DELETE FROM viewer_panes WHERE id = ?").run(params.id);
            if (row) this.repairLayoutsForPane(row.dev_session_id, params.id, Date.now());
            return { ok: true } as const;
        });
    }

    // --- Terminal panes -----------------------------------------------------

    // Read-then-write (parent lookup + nextPosition + INSERT), so it runs in a
    // transaction for the same reason createGroup does.
    //
    // A pane is born WITH a tmux target unless the caller names one. That is
    // what makes this row the pane's identity: the desktop client creates the
    // row and attaches to whatever `tmuxTarget` comes back, so it never has to
    // (and no longer does) invent a session name of its own. A caller-supplied
    // target is still honoured — adopting a tmux session that already exists on
    // the host is a real workflow (see the tmux.* method group) — but it is
    // checked, because it ends up in a `-t '=<target>'` argument.
    createTerminalPane(params: CreateTerminalPaneParams): TerminalPane {
        return this.transaction(() => {
            const id = randomUUID();
            const ts = Date.now();
            // SPEC 3.5: server_id derived from the parent session (see
            // createViewerPane); the client-sent serverId is overridden.
            const serverId = this.parentServerId("dev_sessions", params.devSessionId);
            // The append slot, never the raw request — see createGroup.
            const position = this.nextPosition("terminal_panes", "dev_session_id", params.devSessionId);
            const tmuxTarget =
                params.tmuxTarget === undefined || params.tmuxTarget === null
                    ? mintTmuxTarget(params.devSessionId, id)
                    : this.requireUnboundTmuxTarget(
                          requireSafeTmuxTarget(params.tmuxTarget, "workspace.createTerminalPane"),
                          null,
                          "workspace.createTerminalPane",
                      );
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
                    tmuxTarget,
                    params.startupCommand ?? null,
                    params.harness ?? null,
                    position,
                    ts,
                    ts,
                );
            if (params.position !== undefined) {
                this.placeAt(
                    "terminal_panes",
                    "dev_session_id",
                    params.devSessionId,
                    id,
                    params.position,
                    ts,
                );
            }
            return this.getTerminalPane(id);
        });
    }

    // Find the row one layout leaf owns, in ONE transaction (SPEC 5.2). This is
    // how a client learns which remote tmux session a terminal pane owns.
    //
    // BY ID — the normal path. The layout leaf carries the row id
    // (SplitNode::terminalPaneId), minted when the leaf was created, so this is
    // a pure lookup: a row id is never recycled and never invented here, which
    // is what makes it impossible for a brand new pane to name an older pane's
    // shell. A leaf naming a row that no longer exists is an error, not an
    // invitation to create one: the client would silently get a different
    // terminal than the one its layout says it owns.
    //
    // BY NAME — the legacy path, lookup-or-CREATE, for leaves written before
    // layouts carried a row id. It exists as a server method precisely because
    // the client cannot do it safely itself: `list` then `createTerminalPane`
    // is two round trips, and two clients opening the same Dev Session at the
    // same moment both see no row in the first and both insert in the second.
    // That is two rows, two minted targets and two tmux sessions for one pane —
    // a race UNIQUE (tmux_target) cannot catch, because the two targets differ.
    // transaction() issues BEGIN IMMEDIATE, so the write lock is taken BEFORE
    // the SELECT. Two codeharbord processes (one per client SSH session, both
    // on the same database file) therefore serialise here: the loser's SELECT
    // runs after the winner's INSERT is committed and finds the row.
    //
    // The label is no longer unique (schema v4), so the by-name lookup takes
    // the OLDEST row bearing it. A legacy leaf predates every row a later mint
    // could have added under the same label, so the oldest is the one that leaf
    // has always meant. `rowid` orders by insertion and, unlike created_at, can
    // never tie.
    //
    // A row found with no tmux target gets one minted now rather than being
    // handed back unusable — the pane would have nothing to attach, and the
    // client is deliberately incapable of minting one for itself.
    resolveTerminalPane(params: ResolveTerminalPaneParams): TerminalPane {
        const id = params.id ?? "";
        const name = params.name ?? "";
        // Exactly one addressing mode. Enforced here rather than only at the
        // dispatcher so an in-process caller gets the rule too: both would leave
        // the caller guessing which one answered, and neither would fall through
        // to a create with an empty name.
        if ((id === "") === (name === "")) {
            throw new InvalidParamsError(
                'workspace.resolveTerminalPane: give exactly one of "id" (the terminal pane row, ' +
                    'the normal case) or "name" (a layout slot label, legacy layouts only)',
            );
        }
        return this.transaction(() => {
            const existing =
                id !== ""
                    ? (this.db
                          .prepare(
                              "SELECT * FROM terminal_panes WHERE id = ? AND dev_session_id = ?",
                          )
                          .get(id, params.devSessionId) as TerminalPaneRow | undefined)
                    : (this.db
                          .prepare(
                              "SELECT * FROM terminal_panes WHERE dev_session_id = ? AND name = ? ORDER BY rowid LIMIT 1",
                          )
                          .get(params.devSessionId, name) as TerminalPaneRow | undefined);
            if (existing) {
                if (existing.tmux_target !== null) return toTerminalPane(existing);
                const minted = mintTmuxTarget(existing.dev_session_id, existing.id);
                this.db
                    .prepare(
                        "UPDATE terminal_panes SET tmux_target = ?, updated_at = ? WHERE id = ?",
                    )
                    .run(minted, Date.now(), existing.id);
                return this.getTerminalPane(existing.id);
            }
            if (id !== "") {
                throw new InvalidParamsError(
                    `workspace.resolveTerminalPane: no terminal pane ${JSON.stringify(id)} ` +
                        `in dev session ${JSON.stringify(params.devSessionId)}`,
                );
            }
            return this.createTerminalPane({
                serverId: params.serverId,
                devSessionId: params.devSessionId,
                name,
                workingDirectory: params.workingDirectory ?? null,
            });
        });
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
            const tmuxTarget =
                params.tmuxTarget !== undefined
                    ? params.tmuxTarget === null
                        ? null
                        : this.requireUnboundTmuxTarget(
                              requireSafeTmuxTarget(
                                  params.tmuxTarget,
                                  "workspace.updateTerminalPane",
                              ),
                              params.id,
                              "workspace.updateTerminalPane",
                          )
                    : current.tmuxTarget;
            const startupCommand =
                params.startupCommand !== undefined ? params.startupCommand : current.startupCommand;
            const harness = params.harness !== undefined ? params.harness : current.harness;
            const ts = Date.now();
            this.db
                .prepare(
                    "UPDATE terminal_panes SET name = ?, working_directory = ?, tmux_target = ?, startup_command = ?, harness = ?, updated_at = ? WHERE id = ?",
                )
                .run(name, workingDirectory, tmuxTarget, startupCommand, harness, ts, params.id);
            if (params.position !== undefined) {
                this.placeAt(
                    "terminal_panes",
                    "dev_session_id",
                    current.devSessionId,
                    params.id,
                    params.position,
                    ts,
                );
            }
            return this.getTerminalPane(params.id);
        });
    }

    // Delete a terminal pane, then repair every stored split layout for its
    // session so no leaf still references it. Layout trees are client-authored,
    // so a malformed client can put a terminal id in the viewer region; the
    // server's integrity guarantee must cover both regions. Deleting a pane
    // that is not there is a successful no-op, and both regions are repaired
    // against one timestamp — see deleteViewerPane for both.
    deleteTerminalPane(params: { id: string }): { ok: true } {
        return this.transaction(() => {
            const row = this.db
                .prepare("SELECT dev_session_id FROM terminal_panes WHERE id = ?")
                .get(params.id) as { dev_session_id: string } | undefined;
            this.db.prepare("DELETE FROM terminal_panes WHERE id = ?").run(params.id);
            if (row) this.repairLayoutsForPane(row.dev_session_id, params.id, Date.now());
            return { ok: true } as const;
        });
    }

    // --- Split layouts ------------------------------------------------------

    getLayout(params: GetLayoutParams): SessionLayout | null {
        const row = this.db
            .prepare("SELECT * FROM session_layouts WHERE dev_session_id = ? AND region = ?")
            .get(params.devSessionId, params.region) as SessionLayoutRow | undefined;
        if (!row) return null;
        // A stored tree that no longer parses must not fail the read: report the
        // region as having no layout (self-heal) rather than throw. RW13 makes
        // the server authoritative over layout integrity, so a corrupt blob can
        // only originate from outside this store (RW14).
        let tree: unknown;
        try {
            tree = JSON.parse(row.tree);
        } catch {
            console.error(
                `workspace: ignoring unparseable layout tree for dev_session_id=${row.dev_session_id} region=${row.region}`,
            );
            return null;
        }
        return {
            id: row.id,
            serverId: row.server_id,
            devSessionId: row.dev_session_id,
            region: row.region,
            tree,
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
        // One transaction around the parent lookup, the upsert, and the
        // read-back: without it a second connection writing the same region in
        // between makes this call RETURN that other tree as if it were the one
        // it just stored, and the client then renders a layout it never saved.
        // Serialized BEFORE the transaction opens, and checked: `tree` is
        // free-form client JSON, and JSON.stringify answers `undefined` for a
        // value that has no JSON form at all (undefined itself, a function, a
        // symbol) and THROWS for a circular structure or a BigInt. Both used to
        // reach node:sqlite, the first as a bind value the driver rejects with
        // an opaque type error naming no field, the second as an exception
        // rolling back a transaction that had already been opened. `tree` is
        // NOT NULL in the schema, so there is no column value to fall back to.
        let serializedTree: string;
        try {
            const json = JSON.stringify(params.tree);
            if (json === undefined) throw new Error("value has no JSON representation");
            serializedTree = json;
        } catch (err) {
            throw new InvalidParamsError(
                `workspace.setLayout: 'tree' must be a JSON-serializable split tree (${
                    err instanceof Error ? err.message : String(err)
                })`,
            );
        }
        // Bounded here, still before the transaction opens, so an over-cap tree
        // costs the database nothing at all: no BEGIN, no upsert, no rollback,
        // and whatever was already stored for the region is left untouched. See
        // MAX_LAYOUT_TREE_BYTES for the measurement behind the number and for
        // what an unbounded tree does to `workspace.list`. Raised as a
        // ResourceLimitError (-32003), not invalid-params: the tree is
        // well-formed, the server simply will not carry it at that size.
        const treeBytes = Buffer.byteLength(serializedTree);
        if (treeBytes > MAX_LAYOUT_TREE_BYTES) {
            throw new ResourceLimitError(
                `Cannot store the ${params.region} layout: its split tree serializes to ` +
                    `${treeBytes} bytes, above this server's ${MAX_LAYOUT_TREE_BYTES}-byte ` +
                    "per-region limit. Nothing was changed; the previously stored layout is " +
                    "still in place. Close some panes, or shorten their custom titles.",
            );
        }
        return this.transaction(() => {
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
                    serializedTree,
                    ts,
                    ts,
                );
            const stored = this.getLayout(params);
            // Present unless the tree we just wrote does not read back as JSON,
            // which nothing can produce through this method. Checked rather
            // than asserted away, so a future change cannot hand the client a
            // `null` typed as a SessionLayout.
            if (!stored) {
                throw new Error(
                    `layout could not be read back after write: ${params.devSessionId}/${params.region}`,
                );
            }
            return stored;
        });
    }

    // Repair a region's stored split layout after `paneId`'s row was deleted, so
    // no leaf still references it (RW13). A no-op when no layout row exists.
    // A drained tree collapses to a single empty leaf { type: "leaf", paneId: "" },
    // exactly how the client represents an empty region (see remapPaneIds). Only
    // the tree and updated_at change; the row's id, created_at, and server_id are
    // left untouched, matching setLayout's identity-preserving semantics. Callers
    // must already be inside a transaction (both delete methods are).
    private repairLayout(devSessionId: string, region: Region, paneId: string, ts: number): void {
        const row = this.db
            .prepare("SELECT * FROM session_layouts WHERE dev_session_id = ? AND region = ?")
            .get(devSessionId, region) as SessionLayoutRow | undefined;
        if (!row) return;
        let tree: unknown;
        try {
            tree = JSON.parse(row.tree);
        } catch {
            // Already-broken blob; the self-healing read path (getLayout/
            // getLayouts) reports it as null. Nothing to repair here.
            return;
        }
        // Nothing in this region names the deleted pane: leave the row exactly
        // as it is. removePaneFromTree rebuilds every split it walks (it
        // renormalizes ratios), so its output almost never compares equal to
        // the stored text even when no leaf was dropped — without this check,
        // deleting a pane in ONE region rewrote the OTHER region's tree and
        // bumped its updated_at for no reason, which the equality check below
        // was written to prevent and cannot.
        if (!treeReferencesPane(tree, paneId)) return;
        const repaired = removePaneFromTree(tree, paneId);
        const finalTree = repaired === null ? { type: "leaf", paneId: "" } : repaired;
        const serialized = JSON.stringify(finalTree);
        if (serialized === row.tree) return;
        this.db
            .prepare("UPDATE session_layouts SET tree = ?, updated_at = ? WHERE id = ?")
            .run(serialized, ts, row.id);
    }

    private repairLayoutsForPane(devSessionId: string, paneId: string, ts: number): void {
        for (const region of REGIONS) {
            this.repairLayout(devSessionId, region, paneId, ts);
        }
    }

    // --- Nested read --------------------------------------------------------

    // The nested tree groups -> sessions -> {viewerPanes, terminalPanes,
    // layouts}. Ordering is deterministic (position, then id) so a reopen of
    // the same database yields byte-identical output.
    //
    // The nested listing's result may be narrowed to pinned sessions for a
    // client-local sidebar filter. The predicate is read-only; `pinned` itself
    // remains in the server-owned session row. A deferred read transaction keeps
    // the multiple group/session/pane queries on one SQLite snapshot, so another
    // daemon cannot make the returned tree describe a mixture of states.
    list(serverId: string, pinnedOnly = false): GroupNode[] {
        const read = (): GroupNode[] => {
            const listing = this.listing();
            const rows = (pinnedOnly ? listing.groupsPinned : listing.groups).all(serverId) as unknown as GroupRow[];
            return rows.map((g) => ({
                ...toGroup(g),
                sessions: this.listSessions(g.id, pinnedOnly, serverId),
            }));
        };
        if (this.inTransaction) return read();
        this.db.exec("BEGIN");
        // Kept in step with the actual state of the connection: the flag means
        // "this connection is inside a BEGIN", so anything the read path may
        // ever call that wants atomicity of its own joins this transaction
        // instead of issuing a nested BEGIN, which SQLite rejects outright.
        this.inTransaction = true;
        this.inReadTransaction = true;
        try {
            const result = read();
            this.inTransaction = false;
            this.inReadTransaction = false;
            this.db.exec("COMMIT");
            return result;
        } catch (err) {
            this.inTransaction = false;
            this.inReadTransaction = false;
            try {
                this.db.exec("ROLLBACK");
            } catch {
                // Preserve the original read or commit failure.
            }
            throw err;
        }
    }

    // Compile the listing's statements on first use and keep them; see
    // listStatements.
    private listing(): ListStatements {
        return (this.listStatements ??= {
            groups: this.db.prepare(
                "SELECT * FROM groups WHERE server_id = ? ORDER BY position, id",
            ),
            groupsPinned: this.db.prepare(
                "SELECT * FROM groups WHERE server_id = ? AND EXISTS (SELECT 1 FROM dev_sessions WHERE dev_sessions.group_id = groups.id AND dev_sessions.server_id = groups.server_id AND dev_sessions.pinned <> 0) ORDER BY position, id",
            ),
            sessions: this.db.prepare(
                "SELECT * FROM dev_sessions WHERE group_id = ? AND server_id = ? ORDER BY position, id",
            ),
            sessionsPinned: this.db.prepare(
                "SELECT * FROM dev_sessions WHERE group_id = ? AND server_id = ? AND pinned <> 0 ORDER BY position, id",
            ),
            viewerPanes: this.db.prepare(
                "SELECT * FROM viewer_panes WHERE dev_session_id = ? AND server_id = ? ORDER BY position, id",
            ),
            terminalPanes: this.db.prepare(
                "SELECT * FROM terminal_panes WHERE dev_session_id = ? AND server_id = ? ORDER BY position, id",
            ),
            layouts: this.db.prepare(
                "SELECT region, tree FROM session_layouts WHERE dev_session_id = ? AND server_id = ?",
            ),
        });
    }

    private listSessions(groupId: string, pinnedOnly: boolean, serverId: string): SessionNode[] {
        const statement = pinnedOnly ? this.listing().sessionsPinned : this.listing().sessions;
        const rows = statement.all(groupId, serverId) as unknown as SessionRow[];
        return rows.map((s) => this.sessionNode(s.id, s, serverId));
    }
    private sessionNode(sessionId: string, row?: SessionRow, serverId?: string): SessionNode {
        const session = row
            ? toSession(row)
            : this.getSession(sessionId);
        const scopeServerId = serverId ?? session.serverId;
        return {
            ...session,
            viewerPanes: this.listViewerPanes(sessionId, scopeServerId),
            terminalPanes: this.listTerminalPanes(sessionId, scopeServerId),
            layouts: this.getLayouts(sessionId, scopeServerId),
        };
    }
    private listViewerPanes(sessionId: string, serverId: string): ViewerPane[] {
        const rows = this.listing().viewerPanes.all(sessionId, serverId) as unknown as ViewerPaneRow[];
        return rows.map(toViewerPane);
    }

    private listTerminalPanes(sessionId: string, serverId: string): TerminalPane[] {
        const rows = this.listing().terminalPanes.all(sessionId, serverId) as unknown as TerminalPaneRow[];
        return rows.map(toTerminalPane);
    }

    private getLayouts(sessionId: string, serverId: string): SessionLayouts {
        const rows = this.listing().layouts.all(sessionId, serverId) as unknown as Array<{
            region: Region;
            tree: string;
        }>;
        const layouts: SessionLayouts = { viewer: null, terminal: null };
        // A single corrupt tree must not fail the whole nested listing: skip it
        // (leaving that region null / self-healed) rather than throw (RW14).
        for (const r of rows) {
            try {
                layouts[r.region] = JSON.parse(r.tree);
            } catch {
                console.error(
                    `workspace: ignoring unparseable layout tree for dev_session_id=${sessionId} region=${r.region}`,
                );
            }
        }
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

    // Ids of one ordered scope, in listing order. See OrderedScope on why the
    // identifiers are typed the way they are.
    private orderedIds<T extends OrderedTable>(
        table: T,
        scopeColumn: OrderedScope[T],
        scopeValue: string,
    ): string[] {
        const rows = this.db
            .prepare(`SELECT id FROM ${table} WHERE ${scopeColumn} = ? ORDER BY position, id`).all(scopeValue) as unknown as Array<{ id: string }>;
        return rows.map((r) => r.id);
    }

    // Put a just-inserted row at index `index` of its scope and renumber the
    // scope around it. Callers must already be inside a transaction.
    //
    // This is what makes "create at position N" mean something. Writing N into
    // the new row's `position` column and stopping leaves the row TIED with
    // whichever row already held N, and every listing query orders by
    // `position, id`, so the tie is broken by UUID — the row the user just
    // created at position 1 appears above or below its neighbour at random, and
    // the two stay tied for every later read. moveSession was fixed for exactly
    // this; creation has the same hole. An index past the end simply appends,
    // which is the only sensible reading of "create at position 9" in a scope
    // of three.
    //
    // The index is truncated and clamped at BOTH ends before use. Array.splice
    // reads a NEGATIVE index as an offset from the END, so `position: -1` used
    // to insert the new row second-to-LAST — the opposite of the "before
    // everything" a negative index reads as — and a fractional index is not a
    // slot at all. The RPC guards already reject both (optionalIndex), so this
    // is the in-process API keeping the same promise on its own.
    private placeAt<T extends OrderedTable>(
        table: T,
        scopeColumn: OrderedScope[T],
        scopeValue: string,
        id: string,
        index: number,
        ts: number,
    ): void {
        const others = this.orderedIds(table, scopeColumn, scopeValue).filter((x) => x !== id);
        const at = Number.isFinite(index)
            ? Math.min(Math.max(Math.trunc(index), 0), others.length)
            : others.length;
        others.splice(at, 0, id);
        this.packOrder(table, scopeColumn, scopeValue, others, ts);
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
    //
    // A row already sitting on the position it is about to be given is left
    // untouched. The UPDATE is not free — it rewrites the row and every index
    // over `position` — but the reason is correctness, not speed: `updated_at`
    // is a change marker, and rewriting it for every row of a scope told the
    // client that ALL of a server's groups had changed because one group was
    // created at the end of the list. Only rows that actually moved are stamped.
    private packOrder<T extends OrderedTable>(
        table: T,
        scopeColumn: OrderedScope[T],
        scopeValue: string,
        orderedIds: readonly string[],
        ts: number,
    ): void {
        const rows = this.db
            .prepare(
                `SELECT id, position FROM ${table} WHERE ${scopeColumn} = ? ORDER BY position, id`,
            )
            .all(scopeValue) as unknown as Array<{ id: string; position: number }>;
        const current = rows.map((r) => r.id);
        const positions = new Map(rows.map((r) => [r.id, r.position]));
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
            if (positions.get(id) === index) return;
            stmt.run(index, ts, id, scopeValue);
        });
    }

    // Next free position in a scope: one past the current maximum, so the first
    // row of an empty scope lands on 0. See OrderedScope on why the identifiers
    // are typed the way they are.
    private nextPosition<T extends OrderedTable>(
        table: T,
        scopeColumn: OrderedScope[T],
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

    // A client-supplied tmux target must not already belong to a DIFFERENT
    // pane. `UNIQUE (tmux_target)` says the same thing, but it says it as a raw
    // SQLite constraint failure, which the dispatcher can only report as an
    // internal error — telling the user the SERVER broke when in fact their
    // request asked two panes to drive one remote shell. Adopting an existing
    // tmux session is a real workflow (the tmux.* method group), so this is a
    // request a client genuinely makes and genuinely gets wrong; it deserves
    // the same -32602 as a structurally unsafe target.
    //
    // Callers are inside transaction()'s BEGIN IMMEDIATE, so no other
    // connection can claim the target between this check and the write. The
    // constraint stays as the backstop for anything that bypasses this path.
    //
    // `exceptPaneId` is the row being updated: re-writing a pane's own target
    // to the value it already holds is a no-op, not a collision.
    private requireUnboundTmuxTarget(
        target: string,
        exceptPaneId: string | null,
        method: string,
    ): string {
        const holder = this.db
            .prepare("SELECT id FROM terminal_panes WHERE tmux_target = ?")
            .get(target) as { id: string } | undefined;
        if (holder !== undefined && holder.id !== exceptPaneId) {
            throw new InvalidParamsError(
                `${method}: tmuxTarget ${JSON.stringify(target)} is already bound to terminal ` +
                    `pane ${JSON.stringify(holder.id)}; two panes on one tmux session share a ` +
                    "single remote shell and mirror each other's keystrokes",
            );
        }
        return target;
    }

    // All-or-nothing wrapper around a group of writes.
    //
    // BEGIN IMMEDIATE, not the default deferred BEGIN: every caller writes, and
    // taking the write lock up front makes two connections that read-modify-write
    // the same rows serialize (the loser fails loudly with SQLITE_BUSY) instead of
    // one silently overwriting fields the other had just changed.
    //
    // Re-entrant, because SQLite rejects a nested BEGIN: a helper that needs
    // atomicity of its own (updateSession, packOrder's callers) can therefore
    // be called both directly and from inside a larger operation. A nested
    // failure still aborts the whole outermost transaction, which is the
    // semantics every caller here wants.
    //
    // The one nesting it refuses is a write inside list()'s DEFERRED read
    // transaction, which holds no write lock — see inReadTransaction. No such
    // caller exists; the check is here so that adding one is a loud programming
    // error rather than an occasional SQLITE_BUSY_SNAPSHOT under real
    // concurrency, which is unreproducible and which no retry inside the
    // transaction can clear.
    private transaction<T>(fn: () => T): T {
        if (this.inReadTransaction) {
            throw new Error(
                "workspace: refusing to write inside the read-only listing transaction",
            );
        }
        if (this.inTransaction) return fn();
        this.db.exec("BEGIN IMMEDIATE");
        this.inTransaction = true;
        try {
            const result = fn();
            this.inTransaction = false;
            this.db.exec("COMMIT");
            return result;
        } catch (err) {
            this.inTransaction = false;
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
        // An override that is present but BLANK counts as absent. `??` only
        // catches an unset variable, so `CODEHARBOR_DB=` (a trivially easy
        // typo in a systemd unit or a wrapper script) used to reach SQLite as
        // the empty filename — which SQLite accepts and answers with a PRIVATE
        // TEMPORARY database that is deleted when the connection closes. The
        // daemon came up looking perfectly healthy with an empty workspace, and
        // everything the user did in it vanished at shutdown. Fall back to the
        // real location instead.
        const override = process.env.CODEHARBOR_DB;
        const dbPath =
            override !== undefined && override.trim() !== ""
                ? override
                : path.join(os.homedir(), ".local", "share", "codeharbor", "codeharbor.sqlite");
        defaultWorkspace = openWorkspace(dbPath);
    }
    return defaultWorkspace;
}

/**
 * Release the default connection on an orderly daemon shutdown.
 *
 * Only closing checkpoints the write-ahead log and removes the `-wal`/`-shm`
 * files beside the database, so a daemon that exits without this leaves them
 * behind for the next process to recover. Nothing is LOST either way — WAL is
 * crash-safe, which it has to be for SIGKILL — so this is tidiness on the paths
 * that can afford it, never a correctness requirement.
 *
 * Safe to call unconditionally, and that is the point: it is a no-op when no
 * connection was ever opened (a daemon can answer a whole session without
 * touching the database), it is idempotent, and it never throws. A shutdown
 * path must not have to reason about any of that.
 *
 * Call it only where the event loop has drained and no `workspace.*` handler
 * can still be running — closing a connection out from under a statement in
 * flight would turn a clean exit into an error. A forced/timed-out shutdown
 * should skip it and let SQLite recover, exactly as it does after a crash.
 */
export function closeDefaultWorkspace(): void {
    if (defaultWorkspace === undefined) return;
    const open = defaultWorkspace;
    // Cleared BEFORE the close, so a close that throws cannot leave the
    // singleton pointing at a half-closed handle that the next workspace()
    // would hand straight back out.
    defaultWorkspace = undefined;
    try {
        open.close();
    } catch {
        // Nothing useful to do on the way out, and nothing at risk: see above.
    }
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
    // `position` is guarded with optionalIndex, not optionalNumber: it is an
    // insertion index for create/update operations and a stored integer for
    // every scope that packOrder maintains. optionalNumber accepts -5 and 2.5,
    // while an invalid position would otherwise leave a scope ordered around a
    // negative or fractional slot.
    //
    // The non-nullable text fields (a group's name, a session's repositoryRoot,
    // a viewer pane's url) are guarded with optionalPlainString rather than
    // optionalString, which admits an explicit null. A null reached the store
    // and was discarded by its `?? current` fallback: the caller was answered
    // with a success and an unchanged row.
    [M.list]: (p) => {
        const o = requireObject(p, M.list);
        const serverId = requireString(o, "serverId", M.list);
        // The guard's RETURN value, not a second unchecked cast of the same
        // field: reading `o.pinnedOnly` again meant the validated value was
        // thrown away and the cast could drift from what was actually checked.
        const pinnedOnly = optionalBoolean(o, "pinnedOnly", M.list) ?? false;
        return workspace().list(serverId, pinnedOnly);
    },
    [M.createGroup]: (p) => {
        const o = requireObject(p, M.createGroup);
        requireString(o, "serverId", M.createGroup);
        requireString(o, "name", M.createGroup);
        optionalIndex(o, "position", M.createGroup);
        optionalBoolean(o, "collapsed", M.createGroup);
        return workspace().createGroup(p as CreateGroupParams);
    },
    [M.updateGroup]: (p) => {
        const o = requireObject(p, M.updateGroup);
        requireString(o, "id", M.updateGroup);
        optionalPlainString(o, "name", M.updateGroup);
        optionalIndex(o, "position", M.updateGroup);
        optionalBoolean(o, "collapsed", M.updateGroup);
        return workspace().updateGroup(p as UpdateGroupParams);
    },
    [M.deleteGroup]: (p) => {
        const o = requireObject(p, M.deleteGroup);
        requireString(o, "id", M.deleteGroup);
        return workspace().deleteGroup(p as { id: string });
    },
    [M.reorderGroups]: (p) => {
        const o = requireObject(p, M.reorderGroups);
        requireString(o, "serverId", M.reorderGroups);
        requireStringArray(o, "orderedIds", M.reorderGroups);
        return workspace().reorderGroups(p as { serverId: string; orderedIds: string[] });
    },
    [M.createSession]: (p) => {
        const o = requireObject(p, M.createSession);
        requireString(o, "serverId", M.createSession);
        requireString(o, "groupId", M.createSession);
        requireString(o, "name", M.createSession);
        requireString(o, "repositoryRoot", M.createSession);
        optionalString(o, "defaultWorkingDirectory", M.createSession);
        optionalString(o, "taskDescription", M.createSession);
        optionalIndex(o, "position", M.createSession);
        optionalBoolean(o, "archived", M.createSession);
        optionalBoolean(o, "pinned", M.createSession);
        return workspace().createSession(p as CreateSessionParams);
    },
    [M.updateSession]: (p) => {
        const o = requireObject(p, M.updateSession);
        requireString(o, "id", M.updateSession);
        optionalPlainString(o, "name", M.updateSession);
        optionalPlainString(o, "repositoryRoot", M.updateSession);
        optionalString(o, "defaultWorkingDirectory", M.updateSession);
        optionalString(o, "taskDescription", M.updateSession);
        optionalIndex(o, "position", M.updateSession);
        optionalBoolean(o, "archived", M.updateSession);
        optionalBoolean(o, "pinned", M.updateSession);
        return workspace().updateSession(p as UpdateSessionParams);
    },
    [M.deleteSession]: (p) => {
        const o = requireObject(p, M.deleteSession);
        requireString(o, "id", M.deleteSession);
        return workspace().deleteSession(p as { id: string });
    },
    [M.reorderSessions]: (p) => {
        const o = requireObject(p, M.reorderSessions);
        requireString(o, "groupId", M.reorderSessions);
        requireStringArray(o, "orderedIds", M.reorderSessions);
        return workspace().reorderSessions(p as { groupId: string; orderedIds: string[] });
    },
    [M.moveSessionToGroup]: (p) => {
        const o = requireObject(p, M.moveSessionToGroup);
        requireString(o, "id", M.moveSessionToGroup);
        requireString(o, "groupId", M.moveSessionToGroup);
        // optionalNumber, deliberately unlike the create/update pair above:
        // here `position` is a drag INDEX rather than a stored position, and
        // moveSessionToGroup truncates and clamps it into the target group's
        // range before renumbering, so an out-of-range drag is a legitimate
        // request with a defined answer instead of a rejected one.
        optionalNumber(o, "position", M.moveSessionToGroup);
        return workspace().moveSessionToGroup(p as MoveSessionParams);
    },
    [M.duplicateSession]: (p) => {
        const o = requireObject(p, M.duplicateSession);
        requireString(o, "id", M.duplicateSession);
        return workspace().duplicateSession(p as { id: string });
    },
    [M.createViewerPane]: (p) => {
        const o = requireObject(p, M.createViewerPane);
        requireString(o, "serverId", M.createViewerPane);
        requireString(o, "devSessionId", M.createViewerPane);
        requireString(o, "url", M.createViewerPane);
        optionalString(o, "handler", M.createViewerPane);
        optionalString(o, "title", M.createViewerPane);
        optionalIndex(o, "position", M.createViewerPane);
        return workspace().createViewerPane(p as CreateViewerPaneParams);
    },
    [M.updateViewerPane]: (p) => {
        const o = requireObject(p, M.updateViewerPane);
        requireString(o, "id", M.updateViewerPane);
        optionalPlainString(o, "url", M.updateViewerPane);
        optionalString(o, "handler", M.updateViewerPane);
        optionalString(o, "title", M.updateViewerPane);
        optionalIndex(o, "position", M.updateViewerPane);
        return workspace().updateViewerPane(p as UpdateViewerPaneParams);
    },
    [M.deleteViewerPane]: (p) => {
        const o = requireObject(p, M.deleteViewerPane);
        requireString(o, "id", M.deleteViewerPane);
        return workspace().deleteViewerPane(p as { id: string });
    },
    [M.createTerminalPane]: (p) => {
        const o = requireObject(p, M.createTerminalPane);
        requireString(o, "serverId", M.createTerminalPane);
        requireString(o, "devSessionId", M.createTerminalPane);
        requireString(o, "name", M.createTerminalPane);
        optionalString(o, "workingDirectory", M.createTerminalPane);
        optionalString(o, "tmuxTarget", M.createTerminalPane);
        optionalString(o, "startupCommand", M.createTerminalPane);
        optionalString(o, "harness", M.createTerminalPane);
        optionalIndex(o, "position", M.createTerminalPane);
        return workspace().createTerminalPane(p as CreateTerminalPaneParams);
    },
    [M.resolveTerminalPane]: (p) => {
        const o = requireObject(p, M.resolveTerminalPane);
        requireString(o, "serverId", M.resolveTerminalPane);
        requireString(o, "devSessionId", M.resolveTerminalPane);
        // Types only. Which of `id`/`name` must be present is the method's own
        // rule (it is a public in-process API too), enforced there.
        optionalString(o, "id", M.resolveTerminalPane);
        optionalString(o, "name", M.resolveTerminalPane);
        optionalString(o, "workingDirectory", M.resolveTerminalPane);
        return workspace().resolveTerminalPane(p as ResolveTerminalPaneParams);
    },
    [M.updateTerminalPane]: (p) => {
        const o = requireObject(p, M.updateTerminalPane);
        requireString(o, "id", M.updateTerminalPane);
        optionalPlainString(o, "name", M.updateTerminalPane);
        optionalString(o, "workingDirectory", M.updateTerminalPane);
        optionalString(o, "tmuxTarget", M.updateTerminalPane);
        optionalString(o, "startupCommand", M.updateTerminalPane);
        optionalString(o, "harness", M.updateTerminalPane);
        optionalIndex(o, "position", M.updateTerminalPane);
        return workspace().updateTerminalPane(p as UpdateTerminalPaneParams);
    },
    [M.deleteTerminalPane]: (p) => {
        const o = requireObject(p, M.deleteTerminalPane);
        requireString(o, "id", M.deleteTerminalPane);
        return workspace().deleteTerminalPane(p as { id: string });
    },
    [M.getLayout]: (p) => {
        const o = requireObject(p, M.getLayout);
        const devSessionId = requireString(o, "devSessionId", M.getLayout);
        const region = requireOneOf(o, "region", M.getLayout, REGIONS);
        return workspace().getLayout({ devSessionId, region });
    },
    [M.setLayout]: (p) => {
        const o = requireObject(p, M.setLayout);
        const serverId = requireString(o, "serverId", M.setLayout);
        const devSessionId = requireString(o, "devSessionId", M.setLayout);
        // A closed set, not merely "a string": the region names a column with a
        // CHECK constraint, so an unlisted value used to reach SQLite and come
        // back as a raw constraint-violation message that named neither the
        // field nor the two values it accepts.
        const region = requireOneOf(o, "region", M.setLayout, REGIONS);
        // `tree` must be PRESENT. Omitted, it stringifies to `undefined`, which
        // the SQLite driver rejects with an opaque type error naming no field.
        const tree = requireDefined(o, "tree", M.setLayout);
        return workspace().setLayout({ serverId, devSessionId, region, tree });
    },
};
