#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace ch::rpc {

// C1 — RPC method catalog (docs/PLAN.md). C++ mirror of the frozen TypeScript
// contract in remote/src/rpc-types.ts for the initial SPEC 8.3 editing file
// method set. Header-only: the method-name and error-code constants bound by
// the R-client workstream, plus the one inline reader of a wire shape that more
// than one consumer has to agree about (decodeFileContent, below). Distinct
// from the server-side implementation in remote/.
//
// Revision tokens (SPEC 8.4) are OPAQUE strings minted by the server. The
// client stores and echoes them verbatim as expectedRevision on writes and
// NEVER derives, parses, or synthesizes them. A write whose expectedRevision no
// longer matches is rejected (never silently overwritten — SPEC 8.6) with
// kRevisionMismatch.

// Application-level JSON-RPC error code for a writeFile whose expectedRevision
// no longer matches the file's current revision (SPEC 8.4 / 8.6).
inline constexpr int kRevisionMismatch = -32001;

// Application-level JSON-RPC error code for a workspace write that lost the
// race for the server's database write lock and gave up waiting. Mirrors
// RPC_DATABASE_BUSY in remote/src/rpc-types.ts. NOT an internal server error:
// nothing was applied and the user is simply asked to try again. No client code
// special-cases it and none should: the busy timeout on the server side makes
// this close to unreachable, and an automatic retry here would repeat a write
// whose effect is unknown. It reaches the user through the generic error path,
// carrying a message already written for a user to read.
inline constexpr int kDatabaseBusy = -32002;

// Application-level JSON-RPC error code for a request that is well-formed but
// whose ANSWER would exceed a server resource bound, so the server refused it
// outright and changed nothing. Mirrors RPC_RESOURCE_LIMIT in
// remote/src/rpc-types.ts, which raises it for FOUR conditions, all in
// remote/src/files.ts:
//   * file.listDirectory whose serialized listing would not fit in one
//     transport frame (MAX_DIRECTORY_LISTING_BYTES);
//   * file.watch past the live-subscription cap (MAX_WATCH_SUBSCRIPTIONS);
//   * file.readFile on a file whose raw size — or whose requested range — is
//     past MAX_FILE_READ_BYTES, which would have meant one allocation of the
//     whole file on the server;
//   * file.readFile whose ENCODED reply is past MAX_FILE_RESPONSE_BYTES, since
//     base64 expansion and JSON escaping can turn a within-limit file into an
//     over-cap frame.
//
// No client code special-cases it and none should: the server's message names
// the limit that bit and is written for a person, so the generic error path
// carries it to the user unchanged. That the refusal exists at all is the
// client's protection too — a listing serialized anyway put a line past
// CodeharbordClient's 16 MiB cap, and going over that cap does not fail one
// reply, it drops the whole transport.
inline constexpr int kResourceLimit = -32003;

// JSON-RPC 2.0's RESERVED code for an internal error (section 5.1). Mirrors
// RPC_INTERNAL_ERROR in remote/src/rpc-types.ts, and it is the code
// CodeharbordClient stamps on every failure it synthesizes itself — a dead,
// replaced or unwritable transport, a malformed response, a request it refuses
// to put on the wire — so a caller needs no separate code path for "the server
// said no" and "we never reached the server".
//
// It lives in this mirror header, beside the application codes above, because
// more than one place on the C++ side mints it and they must not drift: the
// value used to be spelled as a private constant per translation unit, with
// only a comment claiming the two agreed.
inline constexpr int kInternalError = -32603;

// Stable wire method names for the initial file set (SPEC 8.3). These mirror the
// values in RPC_METHODS in remote/src/rpc-types.ts.
inline constexpr auto kMethodStat = "file.stat";
inline constexpr auto kMethodReadFile = "file.readFile";
inline constexpr auto kMethodWriteFile = "file.writeFile";
inline constexpr auto kMethodResolvePath = "file.resolvePath";
inline constexpr auto kMethodWatch = "file.watch";
inline constexpr auto kMethodUnwatch = "file.unwatch";
inline constexpr auto kMethodListDirectory = "file.listDirectory";

// Decode the {encoding, content} pair of a file.readFile result (SPEC 8.3) into
// the text a consumer should display.
//
// The daemon decodes the file with a STRICT UTF-8 decoder and, when that
// refuses, answers `encoding: "base64"` carrying the file's exact bytes
// (remote/src/files.ts). A base64 reply is therefore not "some other kind of
// file": it is the same file, sent losslessly because the decoder would have
// had to guess. Every consumer must decode it before presenting it, or the user
// is shown a wall of base64 instead of their file.
//
// It lives here, with the reply shape it decodes, rather than beside its
// caller. TODAY every caller sits in ONE translation unit — the three
// file.readFile replies in ch::EditorController (open, reload, and the
// crash-recovery snapshot) — and that is a fact about right now, not evidence
// of a second user: ch::ViewerModel::settleTextRead read the same shape and
// decoded it the same way until the viewer's text-read path was removed in this
// same round, which is how the two came to disagree in the first place. Anything
// that reads a file.readFile reply next needs this, and it must not be
// rediscovered a third time. There is also nowhere else the two could have
// shared it: ch_editor deliberately does not link ch_viewers (see
// EditorController::kMaxEditableReadBytes) and both link ch_remote.
//
// Returns nullopt when the wire shape is missing a string `encoding` or
// `content`, names an unsupported encoding, or a base64 payload is not valid
// base64 — each is a server bug or a corrupted frame. Callers report that
// rather than rendering an empty file. Invalid UTF-8 INSIDE a correctly encoded
// payload is not an error here: it becomes U+FFFD, which is the honest read-only
// view of a file the strict decoder refused.
//
// Decoding NEVER makes a buffer writable. Writability is derived by the caller
// from the wire `encoding` (and `truncated`), never from the payload's shape,
// so a decoded base64 buffer is still refused by the save path.
inline std::optional<QString> decodeFileContent(const QJsonObject& readResult)
{
    const QJsonValue encodingValue =
        readResult.value(QStringLiteral("encoding"));
    const QJsonValue contentValue =
        readResult.value(QStringLiteral("content"));
    if (!encodingValue.isString() || !contentValue.isString())
        return std::nullopt;

    const QString encoding = encodingValue.toString();
    const QString content = contentValue.toString();
    if (encoding == QLatin1String("utf-8"))
        return content;
    if (encoding != QLatin1String("base64"))
        return std::nullopt;

    const auto decoded = QByteArray::fromBase64Encoding(
        content.toUtf8(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return std::nullopt;
    return QString::fromUtf8(*decoded);
}

// Server -> client notification method name for an active watch subscription
// (SPEC 8.7). A NOTIFICATION name (no id, no response), deliberately NOT part
// of the request methods above. Mirrors RPC_WATCH_EVENT_NOTIFICATION in
// remote/src/rpc-types.ts.
inline constexpr auto kWatchEventNotification = "file.watchEvent";

// Server -> client notification: watch notifications for one or more
// subscriptions were DROPPED by the daemon because the client's end of the SSH
// channel stalled long enough to fill the daemon's bounded notification queue.
// Params: { "subscriptionIds": [ ... ] }. The listed subscriptions' watched
// paths MUST be re-read; their cached contents may be stale. Mirrors
// RPC_WATCH_EVENTS_LOST_NOTIFICATION in remote/src/rpc-types.ts.
inline constexpr auto kWatchEventsLostNotification = "file.watchEventsLost";

// Server -> client notification carrying ONE viewer-pane command an agent asked
// for through codeharbord's control socket (remote/src/control.ts). Params:
// { commandId, devSessionId, terminalId, op, args }. A NOTIFICATION name (no
// id, no response), deliberately NOT part of the request methods. Mirrors
// RPC_VIEWER_COMMAND_NOTIFICATION in remote/src/rpc-types.ts.
//
// Consumed by ch::ViewerCommandService, which validates it and hands it to
// Main.qml. It is a notification rather than a server-initiated request because
// CodeharbordClient has no server-request dispatcher — a message carrying both
// `method` and `id` is treated as malformed on purpose — and the ANSWER travels
// back as an ordinary client request instead (kMethodViewerCommandResult).
inline constexpr auto kNotificationViewerCommand = "viewer.command";

// Client -> server request delivering the outcome of one relayed viewer
// command. Params: { commandId, ok, error?: {code, message}, data?: {...} }.
// Mirrors RPC_VIEWER_COMMAND_RESULT_METHOD in remote/src/rpc-types.ts, whose
// handler lives in codeharbord's static method map beside `ping` and
// `server.info` — it belongs to the transport's own control channel, not to the
// file/workspace/tmux domains, so it joins no group.
//
// Answering a commandId the daemon no longer holds is NOT an error: it may
// already have timed the command out, and turning a late answer into a fault
// would put a toast in front of the user for a command that merely finished
// slowly.
inline constexpr auto kMethodViewerCommandResult = "viewer.commandResult";

// --- Server introspection ---------------------------------------------------
//
// Mirrors the `server.info` handler in remote/src/codeharbord.ts.
inline constexpr auto kMethodServerInfo = "server.info";

// --- transport liveness ------------------------------------------------------
//
// Mirrors RPC_PING_METHOD in remote/src/rpc-types.ts, which keys the `ping`
// handler in remote/src/codeharbord.ts's static method map.
//
// Deliberately UNGROUPED, and deliberately not renamed to `server.ping`. This
// is not an application method: nothing about the workspace, the filesystem or
// tmux is being asked. It is the transport keepalive
// CodeharbordClient::enableHeartbeat() sends to answer one question — "is the
// peer still reading and writing?" — and its bare name is the wire name every
// already-deployed `codeharbord` answers. Renaming it into the `server.` group
// would break the probe against every server older than this change for no gain
// (the heartbeat is the one call that must work before we know anything about
// the peer), so it is pinned as a singleton alongside kMethodServerInfo in
// remote/test/rpc-mirror.test.ts instead of joining a group.
inline constexpr auto kMethodPing = "ping";

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
// Lookup-or-create for ONE layout slot, in one server-side transaction (SPEC
// 5.2). Deliberately not list + createTerminalPane on the client: two clients
// running that pair concurrently both see no row and both create one, which is
// two tmux sessions for a single terminal pane.
inline constexpr auto kMethodWorkspaceResolveTerminalPane =
    "workspace.resolveTerminalPane";
inline constexpr auto kMethodWorkspaceUpdateTerminalPane =
    "workspace.updateTerminalPane";
inline constexpr auto kMethodWorkspaceDeleteTerminalPane =
    "workspace.deleteTerminalPane";
inline constexpr auto kMethodWorkspaceGetLayout = "workspace.getLayout";
inline constexpr auto kMethodWorkspaceSetLayout = "workspace.setLayout";

} // namespace ch::rpc
