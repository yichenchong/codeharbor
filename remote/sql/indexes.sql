-- CodeHarbor workspace lookup indexes (SPEC 11.1).
--
-- Split out of schema.sql on purpose. schema.sql is applied only by the
-- migration runner, i.e. only when the stored schema_version is BELOW the
-- version this build targets (migrate() in remote/src/workspace.ts). An index
-- added to schema.sql alone would therefore never reach a database that is
-- already at the current version — and it cannot be delivered by bumping the
-- version either, because that number is mirrored in C++ by
-- WorkspaceDb::kSchemaVersion (src/persistence/WorkspaceDb.h) and pinned to
-- this side by tests in both languages: a one-sided bump breaks the client.
--
-- Indexes need no version gate anyway. They hold no data, change no row, and
-- every statement below is idempotent, so openWorkspace() executes this file on
-- EVERY open. That is what actually makes an existing database gain them, and
-- what lets one be RETIRED here too: a plain DROP INDEX IF EXISTS reaches every
-- database on its next open, which schema.sql could never do.
--
-- Each index covers a column that only ever appears as an equality lookup or a
-- join key in Workspace's queries (listing a group's sessions, a session's
-- panes, a server's groups). Without them every listing is a full table scan.
--
-- Two lookup columns are deliberately absent because a constraint already
-- indexes them, and adding a second b-tree over the same column would charge
-- every write twice for nothing:
--   * session_layouts.dev_session_id — leftmost column of the implicit index
--     behind UNIQUE (dev_session_id, region), which serves both the per-region
--     read and the per-session delete.
--   * terminal_panes.tmux_target — the implicit index behind UNIQUE
--     (tmux_target), which serves the "is this target already bound?" lookup.
-- Removing either constraint therefore turns those reads into full table scans;
-- whoever does that has to add the index here.

CREATE INDEX IF NOT EXISTS idx_groups_server_id
    ON groups (server_id);

-- Sessions are looked up by group, sometimes narrowed to the pinned ones for
-- the sidebar's optional filtered view. ONE index serves both: `group_id` is
-- its leftmost column, so SQLite uses it for the unfiltered listing exactly as
-- it would a group-only index.
--
-- Which is why the group-only index that used to sit here is DROPPED rather
-- than kept. Its column list was a strict prefix of this one, so the planner
-- never chose it (verified with EXPLAIN QUERY PLAN on every dev_sessions query
-- this module issues, filtered and unfiltered); it only charged every session
-- insert, update and delete for a second b-tree write. Foreign-key enforcement
-- on dev_sessions.group_id uses this index for the same prefix reason, so
-- deleting a group is unaffected.
DROP INDEX IF EXISTS idx_dev_sessions_group_id;
CREATE INDEX IF NOT EXISTS idx_dev_sessions_group_pinned
    ON dev_sessions (group_id, pinned);

CREATE INDEX IF NOT EXISTS idx_viewer_panes_dev_session_id
    ON viewer_panes (dev_session_id);

CREATE INDEX IF NOT EXISTS idx_terminal_panes_dev_session_id
    ON terminal_panes (dev_session_id);
