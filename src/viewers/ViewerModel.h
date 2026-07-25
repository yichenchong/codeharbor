#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

QT_BEGIN_NAMESPACE
class QQuickWebEngineProfile;
QT_END_NAMESPACE

namespace ch {

class CodeharbordClient;
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

    void setClient(CodeharbordClient *client) { m_client = client; }
    CodeharbordClient *client() const { return m_client; }

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

    // Asynchronously read a remote text file (SPEC 8.3). On success emits
    // textFileRead(path, content); on failure or without a client, textFileError.
    Q_INVOKABLE void readTextFile(const QString &path);

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

    CodeharbordClient *m_client;
    InternalUrlMap *m_map;
    ViewerProfiles *m_profiles = nullptr;
    bool m_ownsProfiles = false;
};

} // namespace ch
