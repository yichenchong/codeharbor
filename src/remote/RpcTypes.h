#pragma once

#include <QString>
#include <QVector>
#include <optional>

namespace ch::rpc {

// C1 — RPC method catalog (docs/PLAN.md). C++ mirror of the frozen TypeScript
// contract in remote/src/rpc-types.ts for the initial SPEC 8.3 editing file
// method set. Header-only: pure data shapes and method-name constants bound by
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

enum class Kind { File, Directory, Symlink, Other };
enum class Encoding { Utf8, Base64 };
enum class WatchEventKind { Created, Modified, Deleted, Renamed };

struct StatParams {
    QString path;
};

struct StatResult {
    QString path;
    Kind kind;
    qint64 size;
    qint64 mtimeMs;
    qint64 mode;
    QString revision;
};

struct ReadFileParams {
    QString path;
    std::optional<qint64> offset;
    std::optional<qint64> length;
};

struct ReadFileResult {
    QString path;
    Encoding encoding;
    QString content;
    QString revision;
    bool truncated;
};

struct WriteFileParams {
    QString path;
    QString content;
    std::optional<Encoding> encoding;
    QString expectedRevision;
};

struct WriteFileResult {
    QString path;
    QString revision;
};

struct ResolvePathParams {
    QString path;
    std::optional<QString> base;
};

struct ResolvePathResult {
    QString path;
    bool insideRepositoryRoot;
};

struct WatchParams {
    QString path;
};

struct WatchResult {
    QString subscriptionId;
};

struct UnwatchParams {
    QString subscriptionId;
};

struct UnwatchResult {
    bool ok;
};

// Server -> client notification for an active watch subscription. `revision` is
// populated when a new revision is known for the affected path.
struct WatchEvent {
    QString subscriptionId;
    QString path;
    WatchEventKind event;
    std::optional<QString> revision;
};

struct ListDirectoryParams {
    QString path;
};

// One entry in a directory listing (SPEC 7.5). Mirrors DirectoryEntry in
// remote/src/rpc-types.ts; server order is unspecified, so the client sorts.
struct DirectoryEntry {
    QString name;
    Kind kind;
};

struct ListDirectoryResult {
    QString path;
    QVector<DirectoryEntry> entries;
};

} // namespace ch::rpc
