#include "ViewerModel.h"

#include "CodeharbordClient.h"
#include "InternalUrlSchemeHandler.h"
#include "RpcTypes.h"
#include "ViewerHandlerRegistry.h"
#include "ViewerProfiles.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <QVariantMap>
#include <QtWebEngineQuick/QQuickWebEngineProfile>
#include <algorithm>
#include <utility>

namespace ch {

namespace {

QString viewKindFor(ViewerResolution resolution)
{
    switch (resolution) {
    case ViewerResolution::DirectWebNavigation:
        return QStringLiteral("web");
    case ViewerResolution::InternalHtmlRenderer:
        return QStringLiteral("markdown");
    case ViewerResolution::TextEditor:
        return QStringLiteral("text");
    case ViewerResolution::ImageViewer:
        return QStringLiteral("image");
    case ViewerResolution::PdfViewer:
        return QStringLiteral("pdf");
    case ViewerResolution::DirectoryViewer:
        return QStringLiteral("directory");
    case ViewerResolution::Download:
    case ViewerResolution::OpenExternally:
    case ViewerResolution::Error:
        break;
    }
    // Download / OpenExternally / Error all fall back to the binary view, which
    // shows metadata plus a download/open affordance.
    return QStringLiteral("binary");
}

// One remote directory entry on its way to the {name, kind} map QML consumes.
// Sorting these rather than the finished QVariantMaps keeps the comparator off
// QVariant::toMap() and QMap key lookups, which it would otherwise pay four
// times for every single comparison.
struct DirectoryEntry {
    QString name;
    QString kind;
};

// Directories first, then by name. The name comparison is case-insensitive with
// a case-sensitive tie-break, so two names differing only in case ("Readme" vs
// "readme") get a stable, reproducible order instead of whatever std::sort
// happens to produce for "equal" elements.
bool directoryEntryLess(const DirectoryEntry &a, const DirectoryEntry &b)
{
    const bool aDir = a.kind == QLatin1String("directory");
    const bool bDir = b.kind == QLatin1String("directory");
    if (aDir != bDir)
        return aDir;
    const int folded = a.name.compare(b.name, Qt::CaseInsensitive);
    if (folded != 0)
        return folded < 0;
    return a.name.compare(b.name, Qt::CaseSensitive) < 0;
}

} // namespace

ViewerModel::ViewerModel(CodeharbordClient *client, InternalUrlMap *map,
                         QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_map(map ? map : &InternalUrlMap::shared())
{
}

void ViewerModel::setProfiles(ViewerProfiles *profiles)
{
    // Handing back the very object we already own must not delete it out from
    // under the caller and leave a dangling pointer behind.
    if (profiles == m_profiles)
        return;
    if (m_ownsProfiles)
        delete m_profiles;
    m_profiles = profiles;
    m_ownsProfiles = false;
}

ViewerProfiles *ViewerModel::profiles()
{
    if (!m_profiles) {
        m_profiles = new ViewerProfiles(m_client, this);
        m_ownsProfiles = true;
    }
    return m_profiles;
}

QString ViewerModel::viewKind(const QUrl &url) const
{
    return viewKindFor(ViewerHandlerRegistry::resolve(url));
}

QString ViewerModel::internalUrlFor(const QUrl &fileUrl)
{
    return m_map->internalUrlFor(fileUrl);
}

QUrl ViewerModel::fileUrlFor(const QString &internalUrl) const
{
    return m_map->fileUrlFor(internalUrl);
}

QQuickWebEngineProfile *ViewerModel::externalProfile()
{
    return profiles()->externalProfile();
}

QQuickWebEngineProfile *ViewerModel::internalProfile()
{
    return profiles()->internalProfile();
}

QString ViewerModel::readTextFile(const QString &path)
{
    // One monotonic counter for the whole model, so a token is never reused and
    // a cancelled read can never be revived by a later one. Minted BEFORE the
    // no-client check so every read, successful or not, is named the same way.
    const QString token = QString::number(++m_nextReadToken);
    m_liveTextReads.insert(token);

    if (!m_client) {
        // Deferred, not emitted here: the caller has not been handed the token
        // yet, so a reply delivered before this function returns is a reply it
        // cannot recognise as its own. Passing `this` as the context object
        // makes the queued call die with the model.
        QMetaObject::invokeMethod(
            this,
            [this, token, path] {
                if (!m_liveTextReads.remove(token))
                    return;
                emit textFileError(
                    token, path,
                    QStringLiteral("no remote client is connected"));
            },
            Qt::QueuedConnection);
        return token;
    }

    // Bound the read exactly as the internal scheme handler does: a text pane
    // must never try to pull a multi-gigabyte file through a single JSON-RPC
    // frame. Passing offset/length makes the file service perform a ranged read
    // and report `truncated` when the file is bigger than the window.
    const QJsonObject params{
        {QStringLiteral("path"), path},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("length"),
         InternalUrlSchemeHandler::kMaxInlineReadBytes},
    };
    QPointer<ViewerModel> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), params,
        [guard, path, token](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            // NEVER settled straight from here. call() runs its callback
            // SYNCHRONOUSLY when it cannot transmit — no transport bound,
            // transport closed, short write — which is exactly the state a
            // dropped SSH session leaves the client in. That reply would be
            // emitted before readTextFile() had returned the token, so the pane
            // (which matches replies by token) would throw its own failure away
            // and sit on "Loading…" for good. One queued hop puts every reply
            // after the token, and the context object makes it die with the
            // model.
            ViewerModel *const self = guard;
            QMetaObject::invokeMethod(
                self,
                [self, path, token, result = std::move(result),
                 error = std::move(error)] {
                    self->settleTextRead(token, path, result, error);
                },
                Qt::QueuedConnection);
        });
    return token;
}

void ViewerModel::settleTextRead(const QString &token, const QString &path,
                                 const QJsonValue &result,
                                 const std::optional<RpcError> &error)
{
    // cancelTextFile() dropped this exact read: ignore its reply. Removing the
    // token here is also what stops the set from growing — a settled read is no
    // longer in flight.
    if (!m_liveTextReads.remove(token))
        return;
    if (error) {
        emit textFileError(token, path, error->message);
        return;
    }
    const QJsonObject obj = result.toObject();
    if (obj.value(QStringLiteral("truncated")).toBool()) {
        // Report the failure rather than silently showing a prefix of the file
        // as if it were the whole thing.
        emit textFileError(token, path,
                           QStringLiteral("file is too large to display inline"));
        return;
    }
    const QString encoding = obj.value(QStringLiteral("encoding")).toString();
    const QString content = obj.value(QStringLiteral("content")).toString();
    // Text is served utf-8; decode a base64 payload defensively so the view
    // still shows something legible for near-text binaries. A malformed payload
    // is reported, never rendered as an empty file.
    QString text = content;
    if (encoding == QLatin1String("base64")) {
        QByteArray encoded = content.toUtf8();
        const auto decoded = QByteArray::fromBase64Encoding(
            std::move(encoded),
            QByteArray::Base64Encoding
                | QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            emit textFileError(
                token, path,
                QStringLiteral("file contents could not be decoded"));
            return;
        }
        text = QString::fromUtf8(*decoded);
    }
    emit textFileRead(token, path, text);
}

void ViewerModel::cancelTextFile(const QString &token)
{
    // Forget this ONE read: its reply then finds no token and is dropped (see
    // readTextFile). Any other read — including a concurrent read of the very
    // same file by another pane — keeps its own token and is untouched. An
    // unknown or empty token means nothing to do.
    m_liveTextReads.remove(token);
}

void ViewerModel::listDirectory(const QString &path)
{
    if (!m_client) {
        emit directoryError(path, QStringLiteral("no remote client is connected"));
        return;
    }

    const QJsonObject params{{QStringLiteral("path"), path}};
    QPointer<ViewerModel> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodListDirectory), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                emit guard->directoryError(path, error->message);
                return;
            }
            const QJsonArray array =
                result.toObject().value(QStringLiteral("entries")).toArray();
            QList<DirectoryEntry> parsed;
            parsed.reserve(array.size());
            for (const QJsonValue &value : array) {
                const QJsonObject entry = value.toObject();
                parsed.append(
                    DirectoryEntry{entry.value(QStringLiteral("name")).toString(),
                                   entry.value(QStringLiteral("kind")).toString()});
            }
            // Server order is unspecified.
            std::sort(parsed.begin(), parsed.end(), directoryEntryLess);
            QVariantList entries;
            entries.reserve(parsed.size());
            for (const DirectoryEntry &entry : std::as_const(parsed)) {
                entries.push_back(QVariantMap{
                    {QStringLiteral("name"), entry.name},
                    {QStringLiteral("kind"), entry.kind},
                });
            }
            emit guard->directoryListed(path, entries);
        });
}

void ViewerModel::resolvePath(const QString &path, const QString &base)
{
    if (!m_client) {
        emit pathResolveError(path,
                              QStringLiteral("no remote client is connected"));
        return;
    }

    QJsonObject params{{QStringLiteral("path"), path}};
    // Omitted rather than sent empty: an empty `base` would resolve against the
    // filesystem root on the server side, and every path is inside THAT.
    if (!base.isEmpty())
        params.insert(QStringLiteral("base"), base);
    QPointer<ViewerModel> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodResolvePath), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                emit guard->pathResolveError(path, error->message);
                return;
            }
            const QJsonObject obj = result.toObject();
            const QJsonValue inside =
                obj.value(QStringLiteral("insideRepositoryRoot"));
            // A server that did not answer the question is reported as a
            // FAILURE, never defaulted: toBool() on an absent value is false,
            // which would mark every file as outside the project.
            if (!inside.isBool()) {
                emit guard->pathResolveError(
                    path,
                    QStringLiteral("reply carried no insideRepositoryRoot flag"));
                return;
            }
            emit guard->pathResolved(
                path, obj.value(QStringLiteral("path")).toString(),
                inside.toBool());
        });
}

} // namespace ch
