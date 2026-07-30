#pragma once

#include <QString>

namespace ch::rpc {

// C1 — RPC method catalog (docs/PLAN.md). C++ mirror of the frozen TypeScript
// contract in remote/src/rpc-types.ts for the initial SPEC 8.3 editing file
// method set. Header-only: the method-name and error-code constants bound by
// the R-client workstream. Distinct from the server-side implementation in
// remote/.
//
// Revision tokens (SPEC 8.4) are OPAQUE strings minted by the server. The
// client stores and echoes them verbatim as expectedRevision on writes and
// NEVER derives, parses, or synthesizes them. A write whose expectedRevision no
// longer matches is rejected (never silently overwritten — SPEC 8.6) with
// kRevisionMismatch.

// Application-level JSON-RPC error code for a writeFile whose expectedRevision
// no longer matches the file's current revision (SPEC 8.4 / 8.6).
inline constexpr int kRevisionMismatch = -32001;

// Stable wire method names for the initial file set (SPEC 8.3). These mirror the
// values in RPC_METHODS in remote/src/rpc-types.ts.
inline constexpr auto kMethodStat = "file.stat";
inline constexpr auto kMethodReadFile = "file.readFile";
inline constexpr auto kMethodWriteFile = "file.writeFile";
inline constexpr auto kMethodResolvePath = "file.resolvePath";
inline constexpr auto kMethodWatch = "file.watch";
inline constexpr auto kMethodUnwatch = "file.unwatch";
inline constexpr auto kMethodListDirectory = "file.listDirectory";

// Server -> client notification method name for an active watch subscription
// (SPEC 8.7). A NOTIFICATION name (no id, no response), deliberately NOT part
// of the request methods above. Mirrors RPC_WATCH_EVENT_NOTIFICATION in
// remote/src/rpc-types.ts.
inline constexpr auto kWatchEventNotification = "file.watchEvent";

// --- Server introspection ---------------------------------------------------
//
// Mirrors the `server.info` handler in remote/src/codeharbord.ts.
inline constexpr auto kMethodServerInfo = "server.info";

// --- tmux session discovery (SPEC 10.2) -------------------------------------
//
// Mirrors the `tmux.*` group in remote/src/rpc-types.ts. It lets the client
// list and ADOPT tmux sessions that already exist on the host instead of
// assuming its own naming scheme. Absence is not failure: a host with no tmux
// binary, or with no server running, returns an empty/false RESULT rather than
// a JSON-RPC error, so the client must not treat emptiness as a fault.

// Stable wire method names, mirroring RPC_TMUX_METHODS.
inline constexpr auto kMethodListSessions = "tmux.listSessions";
inline constexpr auto kMethodSessionExists = "tmux.sessionExists";
inline constexpr auto kMethodKillSession = "tmux.killSession";

// --- workspace persistence (SPEC 4.2, 11.1) ---------------------------------
//
// Mirrors the `workspace.*` group in remote/src/rpc-types.ts. This is the
// client's CRUD surface over the server-owned workspace database; the data
// shapes live in src/persistence/WorkspaceDb.h, only the wire names belong to
// the contract.
//
// Stable wire method names, mirroring RPC_WORKSPACE_METHODS.
// remote/test/rpc-mirror.test.ts scrapes every `inline constexpr auto k... =
// "...";` definition out of this header, groups them by wire-name prefix, and
// compares the SETS in both directions — so a name added, removed, or renamed on
// either side fails that test. Declaration order here is free (the test sorts
// both sides); it is kept aligned with the TypeScript table for readability only.
inline constexpr auto kMethodWorkspaceList = "workspace.list";
inline constexpr auto kMethodWorkspaceCreateGroup = "workspace.createGroup";
inline constexpr auto kMethodWorkspaceUpdateGroup = "workspace.updateGroup";
inline constexpr auto kMethodWorkspaceDeleteGroup = "workspace.deleteGroup";
inline constexpr auto kMethodWorkspaceReorderGroups = "workspace.reorderGroups";
inline constexpr auto kMethodWorkspaceCreateSession = "workspace.createSession";
inline constexpr auto kMethodWorkspaceUpdateSession = "workspace.updateSession";
inline constexpr auto kMethodWorkspaceDeleteSession = "workspace.deleteSession";
inline constexpr auto kMethodWorkspaceReorderSessions =
    "workspace.reorderSessions";
inline constexpr auto kMethodWorkspaceMoveSessionToGroup =
    "workspace.moveSessionToGroup";
inline constexpr auto kMethodWorkspaceDuplicateSession =
    "workspace.duplicateSession";
inline constexpr auto kMethodWorkspaceCreateViewerPane =
    "workspace.createViewerPane";
inline constexpr auto kMethodWorkspaceUpdateViewerPane =
    "workspace.updateViewerPane";
inline constexpr auto kMethodWorkspaceDeleteViewerPane =
    "workspace.deleteViewerPane";
inline constexpr auto kMethodWorkspaceCreateTerminalPane =
    "workspace.createTerminalPane";
inline constexpr auto kMethodWorkspaceUpdateTerminalPane =
    "workspace.updateTerminalPane";
inline constexpr auto kMethodWorkspaceDeleteTerminalPane =
    "workspace.deleteTerminalPane";
inline constexpr auto kMethodWorkspaceGetLayout = "workspace.getLayout";
inline constexpr auto kMethodWorkspaceSetLayout = "workspace.setLayout";

} // namespace ch::rpc
