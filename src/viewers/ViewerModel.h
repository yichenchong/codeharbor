#pragma once

#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <optional>

// Full definition (not a forward declaration): m_client is a QPointer, whose
// QObject static_cast needs the complete CodeharbordClient (a QObject) type.
#include "CodeharbordClient.h"

QT_BEGIN_NAMESPACE
class QQuickWebEngineProfile;
QT_END_NAMESPACE

namespace ch {


class InternalUrlMap;
class ViewerProfiles;

// QML-facing facade over the viewer subsystem (exposed as the `viewers` context
// property). It is the single object QML talks to: it classifies URLs (handler
// registry), maps file <-> internal URLs, hands out the two WebEngine security
// profiles, reads remote text files, and lists remote directories. It holds no
// view state.
class ViewerModel : public QObject {
    Q_OBJECT
public:
    // `client` performs remote file.* calls; `map` mints/resolves internal URLs
    // (defaults to InternalUrlMap::shared(), shared with the scheme handler).
    // Neither is owned.
    explicit ViewerModel(CodeharbordClient *client = nullptr,
                         InternalUrlMap *map = nullptr,
                         QObject *parent = nullptr);

    // Inject the WebEngine profiles. When unset, ViewerModel lazily creates its
    // own ViewerProfiles from `client`, so QML always has working profiles.
    void setProfiles(ViewerProfiles *profiles);

    // Map a URL to the QML view kind that should render it:
    //   "web" | "markdown" | "text" | "image" | "pdf" | "directory" | "binary".
    Q_INVOKABLE QString viewKind(const QUrl &url) const;

    // Opaque internal URL for a remote file URL, and its inverse.
    Q_INVOKABLE QString internalUrlFor(const QUrl &fileUrl);
    Q_INVOKABLE QUrl fileUrlFor(const QString &internalUrl) const;

    // The sandboxed external (http/https) profile and the privileged internal
    // profile. QML binds these to WebEngineView.profile. Never null once a
    // client is set.
    Q_INVOKABLE QQuickWebEngineProfile *externalProfile();
    Q_INVOKABLE QQuickWebEngineProfile *internalProfile();

    // Asynchronously read a remote text file (SPEC 8.3). The read is capped at
    // InternalUrlSchemeHandler::kMaxInlineReadBytes, the same bound the internal
    // scheme handler applies; a larger file fails instead of being shown
    // truncated. On success emits textFileRead(token, path, content); on
    // failure, on a file over the cap, or without a client, textFileError.
    //
    // Returns the token identifying THIS read. Both reply signals carry it back,
    // and cancelTextFile() takes it. That token — not the path — is a read's
    // identity, because ONE ViewerModel is shared by every viewer pane and two
    // panes may perfectly well be showing the SAME file: a path cannot tell
    // those two reads apart, so anything keyed by path makes one pane's cancel
    // or reload silently discard the other pane's reply and leave it loading
    // forever.
    //
    // The token is a QString rather than a 64-bit integer because QML numbers
    // are IEEE doubles and cannot represent large 64-bit values exactly; a
    // string round-trips through QML unchanged, and callers only ever compare
    // it for equality.
    //
    // Delivery is ALWAYS asynchronous — for EVERY outcome, not just the
    // no-client one. A reply emitted before this function returned would carry
    // a token the caller has not been given yet and could not match, so it
    // would be discarded and the pane would wait for an answer that already
    // came and went. That is not hypothetical: CodeharbordClient::call()
    // invokes its callback synchronously whenever it cannot transmit (no
    // transport bound, transport closed, write failed), which is precisely what
    // a dropped SSH session looks like.
    Q_INVOKABLE QString readTextFile(const QString &path);

    // Drop the in-flight readTextFile identified by `token`, so its reply is
    // ignored when it arrives. This cancels EXACTLY that one read: another
    // pane's read — of a different file or of the very same file — is never
    // touched. An unknown or empty token (what a caller passes when it has
    // nothing outstanding) cancels nothing.
    Q_INVOKABLE void cancelTextFile(const QString &token = QString());

    // How many readTextFile calls are still awaiting a reply. Bookkeeping that
    // outlived its request is otherwise invisible from outside the class, so
    // this is what lets a test pin that entries never accumulate.
    int inFlightTextReadCount() const
    {
        return static_cast<int>(m_liveTextReads.size());
    }

    // Asynchronously list a remote directory (SPEC 7.5). On success emits
    // directoryListed with entries sorted (directories first, then by name),
    // each a {name, kind} map; on failure or without a client, directoryError.
    Q_INVOKABLE void listDirectory(const QString &path);

    // Asynchronously ask where `path` resolves and whether it lands inside
    // `base`, the Dev Session's repository root (SPEC 9). On success emits
    // pathResolved(path, resolvedPath, insideRepositoryRoot); on failure or
    // without a client, pathResolveError.
    //
    // SPEC 9 allows paths outside the root and this call does NOT change that:
    // the flag is a UI hint the pane shows as a marker, never a gate. The
    // server's flag is lexical (see resolvePath in remote/src/files.ts), so it
    // costs no filesystem access.
    //
    // `base` is passed rather than left to the server's default (its own
    // working directory), because "the project" is the ACTIVE Dev Session's
    // repository root and only the client knows which session a pane belongs
    // to. An empty `base` is omitted from the request.
    //
    // Keyed by path like listDirectory and deliberately not by a token: unlike
    // a file read, the answer for one path is the same for every pane that
    // asks, so a second pane receiving the first pane's reply learns exactly
    // what it would have been told anyway.
    Q_INVOKABLE void resolvePath(const QString &path, const QString &base);

signals:
    void textFileRead(const QString &token, const QString &path,
                      const QString &content);
    void textFileError(const QString &token, const QString &path,
                       const QString &message);
    void directoryListed(const QString &path, const QVariantList &entries);
    void directoryError(const QString &path, const QString &message);
    // `path` echoes what was ASKED for (not the resolved spelling), so a pane
    // can tell an answer about the file it is showing from an answer about the
    // file it has since navigated away from.
    void pathResolved(const QString &path, const QString &resolvedPath,
                      bool insideRepositoryRoot);
    void pathResolveError(const QString &path, const QString &message);

private:
    ViewerProfiles *profiles();

    // Turn one settled file.readFile reply into the textFileRead/textFileError
    // the pane is waiting for, and retire its token. Always reached through a
    // queued invocation, never straight from the RPC callback: see the
    // asynchrony note on readTextFile().
    void settleTextRead(const QString &token, const QString &path,
                        const QJsonValue &result,
                        const std::optional<RpcError> &error);

    QPointer<CodeharbordClient> m_client;
    InternalUrlMap *m_map;
    ViewerProfiles *m_profiles = nullptr;
    bool m_ownsProfiles = false;
    // Tokens of the readTextFile calls still in flight. A reply whose token is
    // no longer here (cancelTextFile dropped it) is discarded; a reply that
    // finds its token removes it, so this holds live reads and nothing else.
    // Keyed by TOKEN, not by path: one hash slot per path cannot represent two
    // concurrent reads of one file, which is ordinary usage the moment two
    // panes show the same document.
    QSet<QString> m_liveTextReads;
    // Source of those tokens. Monotonic and never reset, so a token is never
    // reused and a cancelled read can never be mistaken for a later one.
    quint64 m_nextReadToken = 0;
};

} // namespace ch
