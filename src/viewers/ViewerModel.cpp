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
            // cancelTextFile() dropped this exact read: ignore its reply.
            // Removing the token here is also what stops the set from growing —
            // a settled read is no longer in flight.
            if (!guard->m_liveTextReads.remove(token))
                return;
            if (error) {
                emit guard->textFileError(token, path, error->message);
                return;
            }
            const QJsonObject obj = result.toObject();
            if (obj.value(QStringLiteral("truncated")).toBool()) {
                // Report the failure rather than silently showing a prefix of
                // the file as if it were the whole thing.
                emit guard->textFileError(
                    token, path,
                    QStringLiteral("file is too large to display inline"));
                return;
            }
            const QString encoding =
                obj.value(QStringLiteral("encoding")).toString();
            const QString content =
                obj.value(QStringLiteral("content")).toString();
            // Text is served utf-8; decode a base64 payload defensively so the
            // view still shows something legible for near-text binaries. A
            // malformed payload is reported, never rendered as an empty file.
            QString text = content;
            if (encoding == QLatin1String("base64")) {
                QByteArray encoded = content.toUtf8();
                const auto decoded = QByteArray::fromBase64Encoding(
                    std::move(encoded),
                    QByteArray::Base64Encoding
                        | QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded) {
                    emit guard->textFileError(
                        token, path,
                        QStringLiteral("file contents could not be decoded"));
                    return;
                }
                text = QString::fromUtf8(*decoded);
            }
            emit guard->textFileRead(token, path, text);
        });
    return token;
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
            QVariantList entries;
            entries.reserve(array.size());
            for (const QJsonValue &value : array) {
                const QJsonObject entry = value.toObject();
                entries.push_back(QVariantMap{
                    {QStringLiteral("name"),
                     entry.value(QStringLiteral("name")).toString()},
                    {QStringLiteral("kind"),
                     entry.value(QStringLiteral("kind")).toString()},
                });
            }
            // Server order is unspecified; sort directories first, then by
            // name. The name comparison is case-insensitive with a
            // case-sensitive tie-break, so two names differing only in case
            // ("Readme" vs "readme") get a stable, reproducible order instead
            // of whatever std::sort happens to produce for "equal" elements.
            std::sort(entries.begin(), entries.end(),
                      [](const QVariant &a, const QVariant &b) {
                          const QVariantMap ma = a.toMap();
                          const QVariantMap mb = b.toMap();
                          const bool aDir =
                              ma.value(QStringLiteral("kind")).toString()
                              == QLatin1String("directory");
                          const bool bDir =
                              mb.value(QStringLiteral("kind")).toString()
                              == QLatin1String("directory");
                          if (aDir != bDir)
                              return aDir;
                          const QString na =
                              ma.value(QStringLiteral("name")).toString();
                          const QString nb =
                              mb.value(QStringLiteral("name")).toString();
                          const int folded =
                              na.compare(nb, Qt::CaseInsensitive);
                          if (folded != 0)
                              return folded < 0;
                          return na.compare(nb, Qt::CaseSensitive) < 0;
                      });
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
