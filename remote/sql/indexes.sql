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
-- EVERY open. That is what actually makes an existing database gain them, and it
-- costs five no-op statements per connection once they exist.
--
-- Each index covers a column that only ever appears as an equality lookup or a
-- join key in Workspace's queries (listing a group's sessions, a session's
-- panes, a server's groups). Without them every listing is a full table scan.

CREATE INDEX IF NOT EXISTS idx_groups_server_id
    ON groups (server_id);

CREATE INDEX IF NOT EXISTS idx_dev_sessions_group_id
    ON dev_sessions (group_id);
-- The sidebar's optional pinned-only listing filters by both group and pinned
-- state. Keep the group-only index above for the unfiltered listing and use
-- this composite index for the filtered path; the latter is client-local UI
-- state, but the server still avoids scanning every session when asked for the
-- current view.
CREATE INDEX IF NOT EXISTS idx_dev_sessions_group_pinned
    ON dev_sessions (group_id, pinned);

CREATE INDEX IF NOT EXISTS idx_viewer_panes_dev_session_id
    ON viewer_panes (dev_session_id);

CREATE INDEX IF NOT EXISTS idx_terminal_panes_dev_session_id
    ON terminal_panes (dev_session_id);
