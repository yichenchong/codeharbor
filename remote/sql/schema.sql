-- CodeHarbor workspace database schema (SPEC 11.1).
--
-- Authoritative SQLite DDL for the server-side workspace database. The schema
-- version defined here is mirrored by `WorkspaceDb::kSchemaVersion` in the C++
-- client and `WORKSPACE_SCHEMA_VERSION` in remote/src/workspace.ts; keep all
-- three values aligned.
-- Additive changes use an ordered migration there so existing databases keep
-- every row.
--
-- Conventions:
--   * TEXT UUID primary keys for every domain row.
--   * INTEGER for positions, booleans (0/1), and timestamps (unix epoch ms).
--   * Every domain table carries `server_id` (SPEC 3.5) so rows remain scoped
--     when multiple server workspaces share one database.
--
-- Indexes are NOT here: they live in indexes.sql, which openWorkspace() applies
-- on every open. This file is applied only by the migration runner, so an index
-- added here would never reach a database that is already at the current schema
-- version. See indexes.sql for the full reasoning.

-- Foreign keys are OFF by default in SQLite and the setting is per CONNECTION,
-- not stored in the file, so this line only covers connections that execute this
-- script (the schema tests). It is also a no-op inside a transaction, and the
-- migration runner applies this DDL in one — openWorkspace() in
-- remote/src/workspace.ts therefore sets the same pragma itself, before
-- migrating, and that is what every runtime connection actually relies on.
PRAGMA foreign_keys = ON;

-- busy_timeout and journal_mode are deliberately NOT here. Both must be in
-- force BEFORE the first statement of this script runs (a second daemon
-- migrating concurrently has to wait, not fail), and journal_mode cannot be
-- changed inside a transaction at all — which is exactly where the migration
-- runner applies this file. applyConnectionPragmas() in
-- remote/src/workspace.ts owns both, on every connection.

-- Schema versioning ---------------------------------------------------------
CREATE TABLE IF NOT EXISTS schema_version (
    id      INTEGER NOT NULL PRIMARY KEY CHECK (id = 1),
    version INTEGER NOT NULL
);
INSERT OR IGNORE INTO schema_version (id, version) VALUES (1, 5);

-- Server identity (SPEC 3.5) ------------------------------------------------
--
-- The STABLE, SERVER-OWNED id that every domain row's `server_id` refers to.
-- Minted once by codeharbord on first use and never rewritten, so a client may
-- key its remote workspace by it: re-adding a connection profile or connecting
-- from a second machine still resolves to the same rows. It identifies THIS
-- DATABASE, not the route to it — it is independent of hostname, port, user,
-- and repository path, and is never derived from anything a client supplies.
--
-- Deliberately its own singleton table rather than a row in `server_settings`
-- or `app_settings`: both of those are keyed BY server_id (UNIQUE (server_id,
-- key) / PRIMARY KEY (server_id, key)), so the server's own id cannot live
-- there without a sentinel server_id, and a per-server key/value pair offers no
-- conflict target for two processes minting at once. The `id = 1` primary key
-- does: concurrent INSERT OR IGNOREs converge on exactly one winner.
CREATE TABLE IF NOT EXISTS server_identity (
    id         INTEGER NOT NULL PRIMARY KEY CHECK (id = 1),
    server_id  TEXT    NOT NULL,
    created_at INTEGER NOT NULL
);

-- Sidebar groups (SPEC 4.x) -------------------------------------------------
CREATE TABLE IF NOT EXISTS groups (
    id         TEXT    NOT NULL PRIMARY KEY,
    server_id  TEXT    NOT NULL,
    name       TEXT    NOT NULL,
    position   INTEGER NOT NULL DEFAULT 0,
    collapsed  INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

-- Dev Sessions (SPEC 4.x) ---------------------------------------------------
CREATE TABLE IF NOT EXISTS dev_sessions (
    id                        TEXT    NOT NULL PRIMARY KEY,
    server_id                 TEXT    NOT NULL,
    group_id                  TEXT    NOT NULL,
    name                      TEXT    NOT NULL,
    repository_root           TEXT    NOT NULL,
    default_working_directory TEXT,
    task_description          TEXT,
    position                  INTEGER NOT NULL DEFAULT 0,
    archived                  INTEGER NOT NULL DEFAULT 0,
    pinned                    INTEGER NOT NULL DEFAULT 0,
    created_at                INTEGER NOT NULL,
    updated_at                INTEGER NOT NULL,
    FOREIGN KEY (group_id) REFERENCES groups (id)
);

-- Panes (SPEC 4.5 / 7.x / 5.x) ----------------------------------------------
CREATE TABLE IF NOT EXISTS viewer_panes (
    id             TEXT    NOT NULL PRIMARY KEY,
    server_id      TEXT    NOT NULL,
    dev_session_id TEXT    NOT NULL,
    url            TEXT    NOT NULL,
    handler        TEXT,
    title          TEXT,
    position       INTEGER NOT NULL DEFAULT 0,
    created_at     INTEGER NOT NULL,
    updated_at     INTEGER NOT NULL,
    FOREIGN KEY (dev_session_id) REFERENCES dev_sessions (id)
);

-- ONE uniqueness rule, data integrity rather than optimization: `tmux_target`
-- is the name of a remote tmux session, so two panes holding one target attach
-- the SAME shell and mirror each other's keystrokes. NULL is still allowed, and
-- SQLite permits any number of NULLs under UNIQUE — a pane with no session
-- bound to it yet is a legitimate state.
--
-- (`dev_session_id`, `name`) is deliberately NOT unique, and schema v4 removed
-- the rule that briefly made it so. `name` holds a layout slot LABEL
-- ("terminal-1", "terminal-2", …) minted per client, and it is not the pane's
-- identity — the row's own `id` is, carried in the layout leaf itself
-- (SplitNode::terminalPaneId). Closing a pane deliberately keeps its row and
-- its tmux session alive, so its label stays taken while the layout no longer
-- shows it, and the next split on any client legitimately hands that same label
-- to a brand new pane needing a brand new row. Enforcing uniqueness here would
-- reject that perfectly correct pair of rows; keying identity on the label is
-- what let a new pane adopt a closed pane's shell in the first place.
--
-- Being declared here covers FRESH databases only. An existing one gains the
-- tmux_target rule as a unique index in the schema v3 migration (SQLite has no
-- ALTER TABLE ADD CONSTRAINT); that step also repairs the duplicates such a
-- database may already contain, and v4 undoes its address index. See
-- migrateTerminalPaneIdentity and migrateDropTerminalPaneAddressUnique in
-- remote/src/workspace.ts.
CREATE TABLE IF NOT EXISTS terminal_panes (
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

-- Split layouts per region (SPEC 4.5) ---------------------------------------
CREATE TABLE IF NOT EXISTS session_layouts (
    id             TEXT    NOT NULL PRIMARY KEY,
    server_id      TEXT    NOT NULL,
    dev_session_id TEXT    NOT NULL,
    region         TEXT    NOT NULL CHECK (region IN ('viewer', 'terminal')),
    tree           TEXT    NOT NULL, /* JSON split tree */
    created_at     INTEGER NOT NULL,
    updated_at     INTEGER NOT NULL,
    FOREIGN KEY (dev_session_id) REFERENCES dev_sessions (id),
    UNIQUE (dev_session_id, region)
);

-- Server configuration (SPEC 3.5 / 11.1) ------------------------------------
CREATE TABLE IF NOT EXISTS server_profiles (
    id         TEXT    NOT NULL PRIMARY KEY,
    server_id  TEXT    NOT NULL,
    name       TEXT    NOT NULL,
    host       TEXT    NOT NULL,
    port       INTEGER NOT NULL DEFAULT 22,
    username   TEXT    NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS server_settings (
    id        TEXT NOT NULL PRIMARY KEY,
    server_id TEXT NOT NULL,
    key       TEXT NOT NULL,
    value     TEXT,
    UNIQUE (server_id, key)
);

-- Application settings (per-server key/value; server_id per SPEC 3.5) ---------
CREATE TABLE IF NOT EXISTS app_settings (
    server_id TEXT NOT NULL,
    key       TEXT NOT NULL,
    value     TEXT,
    PRIMARY KEY (server_id, key)
);
