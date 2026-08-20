#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

// Full definitions (not forward declarations): m_client is a QPointer, whose
// QObject static_cast needs the complete CodeharbordClient type; the image cache
// is held by value in a shared_ptr; and createMarkdownModel() is a Q_INVOKABLE
// returning ch::MarkdownModel*, which moc must see as a complete QObject type.
#include "CodeharbordClient.h"
#include "MarkdownModel.h"
#include "MobileImageProvider.h"

namespace ch {

// QML-facing remote-file facade for the MOBILE client (installed as the
// `viewerService` context property). It is the mobile counterpart of the parts
// of ch::ViewerModel that do not involve WebEngine: it classifies URLs through
// the same ch::ViewerHandlerRegistry, and it performs every remote file
// operation the native mobile viewers need over the SAME JSON-RPC methods the
// desktop uses (file.readFile, file.writeFile, file.stat, file.listDirectory,
// file.resolvePath). The daemon protocol is unchanged; only the presentation is.
//
// WHAT IT DELIBERATELY CANNOT DO, because Android and iOS have neither
// WebEngine nor any business reaching outside the app sandbox:
//   * no local filesystem READ, ever — every byte a viewer shows comes from an
//     RPC reply. There is exactly ONE local WRITE, the app-private PDF spool
//     (see requestPdf()), which exists because QtQuick.Pdf's QML PdfDocument can
//     only take a URL; it is documented there, keyed by digest rather than by
//     remote name, and emptied on release, on destruction and at startup;
//   * no network access of its own — the only socket is the SSH transport the
//     CodeharbordClient is already bound to;
//   * no handing of a remote URL to the OS. There is no QDesktopServices call
//     in this class and there must never be one: the desktop's
//     ViewerModel::openWithApplication() is a DESKTOP affordance, and on a
//     phone the equivalent would mean exporting a remote path to another
//     application (SPEC 7.4 / 2.4).
//
// ASYNCHRONY. Like ViewerModel, every operation is fire-and-forget and every
// answer arrives as a SIGNAL carrying the path it is about, so a page can match
// a reply against what it asked for. Nothing here is pane-scoped: one service
// instance serves the single visible pane, and the path in each signal is what
// disambiguates a late answer from a current one.
class MobileViewerService : public QObject {
    Q_OBJECT
public:
    // Byte ceiling on every file.readFile this service issues.
    //
    // The number is 8 MiB and it is NOT a fresh choice: it is exactly
    // InternalUrlSchemeHandler::kMaxInlineReadBytes, the ceiling the desktop's
    // inline viewer already applies, and exactly
    // EditorController::kMaxEditableReadBytes, the ceiling the editor applies.
    // Keeping the third consumer on the same number is the point — one file
    // must never be viewable on one surface and unviewable on another, and a
    // mobile-specific limit would create precisely that split.
    //
    // It is restated here rather than shared because ch_mobile links
    // ch_viewers_core (Qt Core only), NOT ch_viewers, which is WebEngine-linked
    // and cannot exist on Android or iOS. The same reason ch_editor restates it.
    //
    // The cap is also what makes `truncated` MEANINGFUL: remote/src/files.ts
    // derives that flag from the requested length, so an unranged read cannot
    // report that the file was too big — it just silently returns everything and
    // holds the whole file in memory on the server, in this process, and again
    // in the view.
    static constexpr int kMaxInlineReadBytes = 8 * 1024 * 1024;

    // `client` performs every remote call and is BORROWED, not owned: it is held
    // through a QPointer because a service that lives as long as the QML engine
    // outlives the client whenever the session is torn down first, and every
    // operation must then degrade to an error signal rather than crash.
    explicit MobileViewerService(CodeharbordClient *client,
                                 QObject *parent = nullptr);

    // Empties the app-private PDF spool. The pages call releasePdf() when they
    // unload; this is the backstop for a teardown that skipped them.
    ~MobileViewerService() override;

    // Mint the Qt Quick image provider that answers this service's imageUrl()
    // addresses, sharing this service's byte cache.
    //
    // A FACTORY rather than a member, because QQmlEngine::addImageProvider()
    // takes ownership of what it is given: a provider owned by this service
    // would be deleted twice, and a provider owning the cache would take the
    // cache away from this service at engine teardown. Both hold a shared_ptr
    // to the cache instead. Called once, from mobile main.cpp.
    MobileImageProvider *createImageProvider();

    // Mint a ch::MarkdownModel parented to `owner` (the QML page asking), the
    // same shape as ch::EditorFactory::create().
    //
    // A FACTORY rather than a QML-registered type, deliberately: ch_mobile is a
    // plain static library, not a QML module, so a QML_ELEMENT here would need
    // either a second QML module or a qmlRegisterType() call in main.cpp — which
    // belongs to another slice. This keeps the model's whole surface inside the
    // slice that owns it, and parenting keeps ownership in C++ so the QML engine
    // cannot garbage-collect a model a ListView is still bound to. A NULL owner
    // parents to this service rather than returning it unparented, for exactly
    // the reason EditorFactory::create() documents.
    Q_INVOKABLE ch::MarkdownModel *createMarkdownModel(QObject *owner = nullptr);

public slots:
    // ---- remote file operations (results arrive as signals) ----

    // Read a remote file for DISPLAY, bounded at kMaxInlineReadBytes.
    //
    // Answers fileRead(path, text, binary, revision, truncated) on success:
    //   * a utf-8 reply becomes `text`, binary=false;
    //   * a base64 reply — which the daemon sends when its STRICT UTF-8 decoder
    //     refused the file, i.e. the file is not text — is reported as
    //     binary=true with an EMPTY `text`. The bytes are deliberately not
    //     offered as text: rendering the base64 alphabet, or a wall of U+FFFD,
    //     as though it were the file is the exact dishonesty the mobile viewers
    //     exist to avoid. Bytes for the one consumer that genuinely wants them
    //     (an image) travel through requestImage() instead.
    //   * `truncated` is the SERVER's flag, forwarded verbatim, never inferred.
    //   * `revision` is REQUIRED. A reply without one is an error rather than a
    //     read with an empty token, because an empty token means CREATE-ONLY to
    //     file.writeFile: the page would load the buffer happily and only find
    //     out at save time that it could never be saved.
    // Answers fileError(path, message) on any failure, including a reply whose
    // encoding, content, revision or size fields cannot be trusted.
    Q_INVOKABLE void readFile(const QString &path);

    // Write `content` back, guarded by the revision the buffer was loaded at
    // (SPEC 8.4/8.6). `expectedRevision` is an OPAQUE server-minted token,
    // echoed verbatim and never parsed or synthesised here, and it is ALWAYS
    // sent — the empty token included, which the server reads as create-only
    // and which is exactly the guard a buffer with no baseline wants. Answers
    // fileWritten(path, revision) or fileError(path, message) — a revision
    // mismatch (ch::rpc::kRevisionMismatch) reaches the user through the same
    // error path carrying the server's own wording, exactly as on the desktop.
    Q_INVOKABLE void writeFile(const QString &path, const QString &content,
                               const QString &expectedRevision);

    // List a remote directory. Answers directoryListed(path, entries) with each
    // entry a {name, kind} map, sorted DIRECTORIES FIRST then case-insensitively
    // by name with a case-sensitive tie-break — byte-for-byte the desktop's
    // ordering (ViewerModel::listDirectory), because a user with both clients
    // must not see two different orders for one directory. Server order is
    // unspecified, so the sort happens here.
    Q_INVOKABLE void listDirectory(const QString &path);

    // file.stat. Answers stated(path, info) with the server's object converted
    // to a QVariantMap, or fileError(path, message).
    Q_INVOKABLE void stat(const QString &path);

    // Ask where `path` resolves and whether it lands inside `base` (the active
    // Dev Session's repository root, SPEC 9). Answers
    // pathResolved(path, resolvedPath, insideRepositoryRoot) or
    // fileError(path, message).
    //
    // As on the desktop the flag is a UI HINT, never a gate: SPEC 9 allows
    // paths outside the root and this client keeps them openable. It has one
    // second, non-cosmetic use in the markdown viewer, where an image whose
    // destination lands outside the repository root is rendered as its alt text
    // instead of being fetched — not because reading it would be refused, but
    // because a document must not be able to make the client fetch arbitrary
    // paths just by naming them.
    //
    // An empty `base` is OMITTED from the request rather than sent: the server
    // would resolve it against its own working directory, and everything is
    // inside the filesystem root.
    Q_INVOKABLE void resolvePath(const QString &path, const QString &base);

    // ---- image handshake (see MobileImageProvider.h for the full protocol) ---

    // Fetch `path`'s bytes into the image cache and then announce the address
    // the Image element should load. Answers imageReady(path, url) — always
    // AFTER the bytes are cached — or imageError(path, message).
    //
    // A file the server reports as TRUNCATED is refused here rather than
    // decoded: a prefix of an image is not a smaller image, it is a corrupt one,
    // and the desktop refuses the same case (InternalUrlSchemeHandler::TooLarge)
    // rather than serving a partial resource as if it were whole.
    //
    // A path already cached answers imageReady immediately without an RPC, which
    // is what makes stepping back to the previous image instant.
    Q_INVOKABLE void requestImage(const QString &path);

    // Drop a cached image (a pane navigating away, or a forced re-read). Never
    // required for correctness — the cache is LRU-bounded — but it lets a pane
    // hand back a multi-megabyte buffer the moment it stops showing it.
    //
    // It also RETIRES a read that is still in flight, so a reply for a path the
    // page has stopped showing does not fill the cache behind its back. See
    // claimRequest() in the .cpp.
    Q_INVOKABLE void forgetImage(const QString &path);

    // ---- PDF spool ---------------------------------------------------------

    // Fetch `path` and make it loadable by QtQuick.Pdf, answering
    // pdfReady(path, fileUrl) or pdfError(path, message).
    //
    // WHY A FILE, and not the in-memory route this class uses for images.
    // QtQuick.Pdf's QML PdfDocument exposes exactly one input, `source`, and it
    // is a URL; QPdfDocument::load(QIODevice*) exists in C++ but is not reachable
    // from the QML type, and there is no QQuickImageProvider equivalent for PDF
    // pages. So the bytes have to land somewhere a URL can name. The in-memory
    // source was preferred and is not available.
    //
    // The landing place is APP-PRIVATE: a "pdf-spool" directory under
    // QStandardPaths::CacheLocation, which on Android is inside the app's own
    // cache dir and on iOS inside the app container. Nothing else on the device
    // can read it, and no OS handler is ever pointed at it — the URL goes to a
    // PdfDocument inside this process and nowhere else.
    //
    // The file name is a SHA-256 digest of the remote path, never the remote
    // basename: a server-controlled name must not be able to choose a filename
    // on this device, which is how "../" and reserved names get in.
    //
    // The spool is emptied in three places, so a PDF cannot outlive its use:
    // releasePdf() (the pane unloading), the destructor, and construction —
    // which also clears whatever a previous run left behind if it was killed by
    // the OS before it could tidy up, the normal way a mobile app dies.
    Q_INVOKABLE void requestPdf(const QString &path);

    // Delete the spooled file for `path`, and retire a read for it that is still
    // in flight so its reply never spools a file nobody will release (see
    // claimRequest() in the .cpp). MUST be called when the page unloads; the
    // destructor is the backstop, not the plan.
    Q_INVOKABLE void releasePdf(const QString &path);

public:
    // ---- pure helpers (no I/O) ----

    // The opaque address for a remote image, image://chremote/<percent-encoded
    // remote path>. The percent-encoding is total (even '/'), so the path is one
    // opaque component that no part of the URL machinery can reinterpret as a
    // host, a query, or a fragment — the same discipline as
    // RemotePath.pathToFileUrl() on the desktop, for the same reason.
    //
    // Returns an EMPTY QUrl for an empty path, so a caller cannot accidentally
    // bind an Image to a provider address that names nothing.
    Q_INVOKABLE QUrl imageUrl(const QString &path) const;

    // Classification, delegated to ch::ViewerHandlerRegistry so the mobile
    // client and the desktop resolve one URL to one kind. Returns the same
    // ch::ViewerKinds vocabulary the desktop's ViewerModel::viewKind() returns:
    // "web", "markdown", "text", "image", "pdf", "directory" or "binary".
    //
    // Unlike the desktop's version there is no per-extension user override: the
    // mobile client has no settings surface, so there is nothing to override
    // with and nothing to keep in step.
    Q_INVOKABLE QString viewKindFor(const QUrl &url) const;

    // The path a remote file:// URL names, and its inverse. The mobile pages
    // need both (a layout leaf stores a URL; every RPC takes a path), and this
    // is a security-relevant conversion, so — exactly as on the desktop, where
    // RemotePath.js is the single place — it lives in ONE place rather than
    // being re-derived per page.
    //
    // Non-static despite being pure: QML resolves methods through the
    // metaobject of the INSTANCE it is given, and `viewerService` is an
    // instance. Keeping these ordinary const members means one spelling for
    // QML and for C++ callers instead of two.
    Q_INVOKABLE QString remotePathFor(const QUrl &url) const;
    Q_INVOKABLE QUrl fileUrlFor(const QString &path) const;

    // Bytes currently retained for `path`, for tests and for the image
    // handshake's own assertions. Empty when not cached.
    QByteArray cachedImageBytes(const QString &path) const;
    const MobileImageCache &imageCache() const { return *m_imageCache; }

signals:
    void fileRead(const QString &path, const QString &text, bool binary,
                  const QString &revision, bool truncated);
    void fileError(const QString &path, const QString &message);
    void fileWritten(const QString &path, const QString &revision);
    void directoryListed(const QString &path, const QVariantList &entries);
    void directoryError(const QString &path, const QString &message);
    void stated(const QString &path, const QVariantMap &info);
    void pathResolved(const QString &path, const QString &resolvedPath,
                      bool insideRepositoryRoot);
    // The bytes for `path` are cached and `url` may be loaded. Emitted after the
    // cache write, never before: the provider answers from the cache on a
    // worker thread and cannot wait for anything.
    void imageReady(const QString &path, const QUrl &url);
    void imageError(const QString &path, const QString &message);
    // The spooled file for `path` is written and `fileUrl` may be handed to a
    // PdfDocument. Never to anything else.
    void pdfReady(const QString &path, const QUrl &fileUrl);
    void pdfError(const QString &path, const QString &message);

private:
    // The one place a file.readFile reply is turned into "bytes we hold". Shared
    // by readFile(), requestImage() and requestPdf(), which need the same
    // validation and differ only in what they do with the result.
    //
    // Returns false and fills `error` when the reply cannot be trusted: a
    // missing or non-string encoding/content, an encoding this build does not
    // know, a base64 payload that is not valid base64, or a payload LARGER than
    // kMaxInlineReadBytes. Every field is CHECKED rather than coerced, for the
    // reason InternalUrlSchemeHandler::decodeReadReply documents: an absent
    // `truncated` reads as false and would let a prefix be presented as a whole
    // file, an unknown encoding decoded as UTF-8 means serving the base64
    // alphabet as if it were the image, and a payload past the cap breaks the
    // bound MobileImageCache sizes itself against.
    //
    // `revision` is passed through when present but NOT required here: only
    // readFile() produces an editable buffer, so only readFile() insists on it.
    struct ReadReply {
        QByteArray bytes;   // the file's exact bytes
        QString text;       // the utf-8 text, empty for a base64 reply
        bool binary = false;
        bool truncated = false;
        QString revision;
    };
    static bool decodeRead(const QJsonObject &result, ReadReply *out,
                           QString *error);

    // Absolute path of the app-private spool directory, created on demand. Empty
    // when the platform reports no writable cache location at all, which
    // disables PDF viewing rather than writing somewhere unexpected.
    QString pdfSpoolDir() const;
    // Delete every file this instance spooled, and the directory's stale
    // leftovers with them.
    void purgePdfSpool();

    // QPointer: see the constructor note — the client may be destroyed first.
    QPointer<CodeharbordClient> m_client;
    // Shared with every MobileImageProvider minted by createImageProvider(),
    // which the QML engine owns and deletes on its own schedule.
    std::shared_ptr<MobileImageCache> m_imageCache;
    // Remote path -> absolute spooled file. Cleared by releasePdf() and by the
    // destructor.
    QHash<QString, QString> m_pdfSpool;

    // In-flight requests whose reply WRITES something a release call is
    // expected to clean up, keyed by path and carrying the epoch of the LATEST
    // request for that path. requestImage()/requestPdf() record an epoch,
    // forgetImage()/releasePdf() drop it, and only a reply whose epoch is still
    // current may write or announce anything — see claimRequest() in the .cpp
    // for the ordering this makes correct, and for the leak it exists to stop.
    // Bounded by the number of distinct paths with a request outstanding.
    qint64 m_requestEpoch = 0;
    QHash<QString, qint64> m_imageRequests;
    QHash<QString, qint64> m_pdfRequests;
};

} // namespace ch
