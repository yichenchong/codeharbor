-- CodeHarbor workspace database schema (SPEC 11.1).
--
-- Authoritative SQLite DDL for the server-side workspace database. The schema
-- version defined here is mirrored by `WorkspaceDb::kSchemaVersion` in
-- src/persistence/WorkspaceDb.h; bump both together (PLAN.md workstream P / C2).
--
-- Conventions:
--   * TEXT UUID primary keys for every domain row.
--   * INTEGER for positions, booleans (0/1), and timestamps (unix epoch ms).
--   * Every domain table carries `server_id` (SPEC 3.5) so multi-server support
--     can be added later without a schema break.

PRAGMA foreign_keys = ON;

-- Schema versioning ---------------------------------------------------------
CREATE TABLE IF NOT EXISTS schema_version (
    id      INTEGER NOT NULL PRIMARY KEY CHECK (id = 1),
    version INTEGER NOT NULL
);
INSERT OR IGNORE INTO schema_version (id, version) VALUES (1, 2);

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
    FOREIGN KEY (dev_session_id) REFERENCES dev_sessions (id)
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
