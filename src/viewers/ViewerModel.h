#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
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
    // truncated. On success emits textFileRead(path, content); on failure, on a
    // file over the cap, or without a client, textFileError.
    Q_INVOKABLE void readTextFile(const QString &path);

    // Drop the in-flight readTextFile for `path`, so its reply is ignored when
    // it arrives. A fresh readTextFile for the SAME path also supersedes an
    // earlier one implicitly; this lets a caller drop a read without
    // immediately starting another (e.g. when a pane's URL clears).
    //
    // The path is REQUIRED to identify the read. ONE ViewerModel is shared by
    // every viewer pane (it is a single QML context property), so a blanket
    // "cancel whatever is in flight" would drop the read another pane is still
    // waiting for and leave that pane loading forever. Calling this with no
    // path therefore cancels nothing.
    Q_INVOKABLE void cancelTextFile(const QString &path = QString());

    // Asynchronously list a remote directory (SPEC 7.5). On success emits
    // directoryListed with entries sorted (directories first, then by name),
    // each a {name, kind} map; on failure or without a client, directoryError.
    Q_INVOKABLE void listDirectory(const QString &path);

signals:
    void textFileRead(const QString &path, const QString &content);
    void textFileError(const QString &path, const QString &message);
    void directoryListed(const QString &path, const QVariantList &entries);
    void directoryError(const QString &path, const QString &message);

private:
    ViewerProfiles *profiles();

    QPointer<CodeharbordClient> m_client;
    InternalUrlMap *m_map;
    ViewerProfiles *m_profiles = nullptr;
    bool m_ownsProfiles = false;
    // Stamp of the in-flight readTextFile for each path, keyed BY PATH because
    // one ViewerModel serves every pane: two panes reading two different files
    // must not supersede each other. A reply whose captured stamp is no longer
    // the one recorded for its path (a newer read replaced it, or
    // cancelTextFile dropped it) is superseded and discarded. Entries are
    // erased as soon as their read settles, so this holds only live reads.
    QHash<QString, quint64> m_textReadGenerations;
    // Source of those stamps. Monotonic and never reset, so a stamp is never
    // reused and a cancelled read can never be mistaken for a later one.
    quint64 m_nextReadGeneration = 0;
};

} // namespace ch
