#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QWebEngineUrlSchemeHandler>

namespace ch {

class CodeharbordClient;

// Free-standing, WebEngine-independent bidirectional map between remote
// file:// URLs and opaque codeharbor-internal://file/<id> URLs (SPEC 7.4).
//
// Remote file:// URLs must never reach Chromium directly (it would treat them
// as client-machine paths), so a privileged internal viewer references remote
// resources through an opaque internal URL. The mapping is:
//   * stable  — the same file URL always maps to the same id;
//   * injective — distinct file URLs map to distinct ids;
//   * invertible — fileUrlFor(internalUrlFor(u)) == u for any file URL u.
// It holds no WebEngine state, so it is unit-testable on its own.
class InternalUrlMap {
public:
    // Scheme name and the fixed "codeharbor-internal://file/" URL prefix.
    static QString scheme();
    static QString prefix();

    // A process-wide shared instance so the scheme handler (which resolves
    // incoming internal requests) and the ViewerModel / QML (which mint internal
    // URLs) agree on the same id space without explicit wiring.
    static InternalUrlMap &shared();

    // Mint (or reuse) the opaque internal URL for a remote file URL.
    QString internalUrlFor(const QUrl &fileUrl);

    // Inverse of internalUrlFor. Accepts either a full internal URL or a bare
    // id; returns an invalid QUrl when the id is unknown.
    QUrl fileUrlFor(const QString &internalUrl) const;

    // Resolve just the opaque id component back to its file URL.
    QUrl fileUrlForId(const QString &id) const;

private:
    mutable QMutex m_mutex;
    QHash<QString, QString> m_fileToId; // file url string -> id
    QHash<QString, QUrl> m_idToFile;    // id -> original file url
    quint64 m_counter = 0;
};

// Custom URL scheme handler for the privileged internal profile (SPEC 7.4).
// Translates codeharbor-internal://file/<id> requests back to the remote file
// URL and streams the file's bytes fetched over the injected CodeharbordClient
// (file.readFile), replying with a MIME type derived from the extension. Text
// is served verbatim (utf-8); binary/image content is base64-decoded first.
class InternalUrlSchemeHandler : public QWebEngineUrlSchemeHandler {
    Q_OBJECT
public:
    // `client` performs the remote reads; `map` resolves opaque ids (defaults to
    // InternalUrlMap::shared()). Neither is owned.
    explicit InternalUrlSchemeHandler(CodeharbordClient *client,
                                      InternalUrlMap *map = nullptr,
                                      QObject *parent = nullptr);

    void requestStarted(QWebEngineUrlRequestJob *job) override;

    // MIME type for a path, derived from its extension only (no filesystem
    // access). Falls back to application/octet-stream.
    static QByteArray mimeForPath(const QString &path);

private:
    CodeharbordClient *m_client;
    InternalUrlMap *m_map;
};

} // namespace ch
