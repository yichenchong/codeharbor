#pragma once

#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

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
// profiles, lists remote directories, and resolves remote paths. It holds no
// view state.
//
// It deliberately has NO "read a text file" operation. SPEC 3.3/7.5 make the
// viewer pane a browser that delegates to handlers, and the ONE text handler is
// the Monaco editor (SPEC 8.1), which does its own reads through
// EditorController. A second model-level text read existed only to feed a
// second, read-only text pane that nothing resolved to; both were removed
// together. See the removal note in docs/bug-hunt-2026-08-01.md, which also
// records what happened to the VW1 fix that lived on that path.
class ViewerModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(quint64 viewKindsRevision READ viewKindsRevision
                   NOTIFY viewKindsRevisionChanged)
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

    // AppSettings lives in ch_app, which cannot be a dependency of this
    // library. The application therefore pushes a plain validated map across
    // this boundary.
    void setDefaultKinds(const QHash<QString, QString>& defaults);
    quint64 viewKindsRevision() const { return m_viewKindsRevision; }

    Q_INVOKABLE QString viewKind(const QUrl &url) const;

    // The settings page asks the model for the same handler-assignability rule
    // that validates a persisted preference; QML does not duplicate it.
    Q_INVOKABLE QStringList validViewKindsForExtension(
        const QString& extension) const;


    // Same registry claims as viewKind(), expressed as the user-facing
    // "Open as" vocabulary. The first entry is the default.
    Q_INVOKABLE QStringList applicableViewKinds(const QUrl &url) const;

    // Strict validation for a user/plugin-provided desktop application scheme.
    Q_INVOKABLE bool isValidApplicationScheme(const QString &scheme) const;

    // Hand a remote path to the desktop handler registered for `scheme`.
    // Returns false for invalid/reserved schemes, malformed targets, or when
    // the desktop reports that no handler accepted the URL.
    Q_INVOKABLE bool openWithApplication(const QString &scheme,
                                         const QString &remotePath) const;
    // Mint the opaque internal URL used by privileged WebEngine handlers for a
    // remote file URL, and resolve that URL back when a handler needs the
    // original address. These are the model-facing half of InternalUrlMap;
    // keeping them here makes the QML handlers use the same shared map as the
    // scheme handler rather than creating a second mapping.
    Q_INVOKABLE QString internalUrlFor(const QUrl &fileUrl);
    Q_INVOKABLE QUrl fileUrlFor(const QString &internalUrl) const;

    // Keep an internal URL alive for as long as a pane is DISPLAYING it, and
    // let it go again when the pane navigates away or is destroyed.
    //
    // The internal-URL table is LRU-bounded. Recency is refreshed when a URL is
    // minted and when the browser resolves it, and a pane that loaded its image
    // hours ago does neither again — so once a thousand other files have been
    // opened, the address that pane is still showing ages out and its next
    // reload fails on a URL that is visibly on screen. Retaining is the only
    // way the table can learn "still displayed".
    //
    // CONTRACT: every retainInternalUrl() that returned true must be matched by
    // exactly one releaseInternalUrl(). Retains are counted, so two panes
    // showing the same file cannot unpin each other. retainInternalUrl()
    // returns false when the address is unknown or malformed — nothing was
    // pinned and nothing must be released. Failing to release costs one table
    // entry, not the bound: eviction continues on the unpinned remainder.
    Q_INVOKABLE bool retainInternalUrl(const QString &internalUrl);
    Q_INVOKABLE void releaseInternalUrl(const QString &internalUrl);
    // The sandboxed external (http/https) profile and the privileged internal
    // profile. QML binds these to WebEngineView.profile. Never null once a
    // client is set.
    Q_INVOKABLE QQuickWebEngineProfile *externalProfile();
    Q_INVOKABLE QQuickWebEngineProfile *internalProfile();

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
    Q_INVOKABLE void resolvePath(const QString &path, const QString &base);

signals:
    void viewKindsRevisionChanged();
    void directoryListed(const QString &path, const QVariantList &entries);
    void directoryError(const QString &path, const QString &message);
    // `path` echoes what was ASKED for (not the resolved spelling), so a pane
    // can tell an answer about the file it is showing from an answer about the
    // file it has since navigated away from.
    void pathResolved(const QString &path, const QString &resolvedPath,
                      bool insideRepositoryRoot);
    void pathResolveError(const QString &path, const QString &message);
    // A request the embedded browser made for an internal URL was refused, with
    // the reason the WebEngine job interface could not carry: fail() takes only
    // Chromium's coarse error enum, so without this an oversized image or PDF
    // reaches the pane as a blank failed page with nothing to explain it.
    // Forwarded verbatim from InternalUrlSchemeHandler::requestFailed().
    // `internalUrl` is the codeharbor-internal:// address that failed; a pane
    // matches it against the one it is showing, exactly as it matches a path.
    void internalResourceError(const QUrl &internalUrl, const QString &message);

private:
    ViewerProfiles *profiles();

    // Forward the internal scheme handler's failure reports onto
    // internalResourceError. Idempotent: re-wiring the same or a new
    // ViewerProfiles drops the previous connection first.
    void wireProfileSignals();

    QPointer<CodeharbordClient> m_client;
    InternalUrlMap *m_map;
    ViewerProfiles *m_profiles = nullptr;
    bool m_ownsProfiles = false;
    // The live InternalUrlSchemeHandler::requestFailed forwarding connection,
    // held so re-wiring can drop it without touching the old handler (which may
    // already be gone with its ViewerProfiles).
    QMetaObject::Connection m_handlerConnection;
    QHash<QString, QString> m_defaultKinds;
    quint64 m_viewKindsRevision = 0;
};

} // namespace ch