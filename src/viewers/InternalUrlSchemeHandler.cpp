#include "InternalUrlSchemeHandler.h"

#include "CodeharbordClient.h"
#include "RpcTypes.h"

#include <QBuffer>
#include <QByteArray>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QMimeType>
#include <QMutexLocker>
#include <QPointer>
#include <QWebEngineUrlRequestJob>

namespace ch {

QString InternalUrlMap::scheme()
{
    return QStringLiteral("codeharbor-internal");
}

QString InternalUrlMap::prefix()
{
    return QStringLiteral("codeharbor-internal://file/");
}

InternalUrlMap &InternalUrlMap::shared()
{
    static InternalUrlMap instance;
    return instance;
}

QString InternalUrlMap::internalUrlFor(const QUrl &fileUrl)
{
    const QString key = fileUrl.toString();
    QMutexLocker lock(&m_mutex);
    const auto it = m_fileToId.constFind(key);
    if (it != m_fileToId.constEnd())
        return prefix() + it.value();

    const QString id = QStringLiteral("f%1").arg(++m_counter);
    m_fileToId.insert(key, id);
    m_idToFile.insert(id, fileUrl);
    return prefix() + id;
}

QUrl InternalUrlMap::fileUrlFor(const QString &internalUrl) const
{
    const QUrl u(internalUrl);
    QString id;
    if (u.scheme().compare(scheme(), Qt::CaseInsensitive) == 0) {
        // codeharbor-internal://file/<id>: host is "file", path is "/<id>".
        id = u.path();
        if (id.startsWith(QLatin1Char('/')))
            id = id.mid(1);
    } else {
        // Treat the argument as a bare id.
        id = internalUrl;
    }
    return fileUrlForId(id);
}

QUrl InternalUrlMap::fileUrlForId(const QString &id) const
{
    QMutexLocker lock(&m_mutex);
    return m_idToFile.value(id);
}

InternalUrlSchemeHandler::InternalUrlSchemeHandler(CodeharbordClient *client,
                                                   InternalUrlMap *map,
                                                   QObject *parent)
    : QWebEngineUrlSchemeHandler(parent)
    , m_client(client)
    , m_map(map ? map : &InternalUrlMap::shared())
{
}

QByteArray InternalUrlSchemeHandler::mimeForPath(const QString &path)
{
    QMimeDatabase db;
    const QMimeType mt = db.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
    if (mt.isValid() && !mt.name().isEmpty())
        return mt.name().toUtf8();
    return QByteArrayLiteral("application/octet-stream");
}

void InternalUrlSchemeHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    if (!job)
        return;

    const QUrl requestUrl = job->requestUrl();
    QString id = requestUrl.path();
    if (id.startsWith(QLatin1Char('/')))
        id = id.mid(1);

    const QUrl fileUrl = m_map->fileUrlForId(id);
    if (!fileUrl.isValid()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    if (!m_client) {
        job->fail(QWebEngineUrlRequestJob::RequestFailed);
        return;
    }

    // Remote path for the read: a remote file:// URL's path is the server path.
    const QString localPath = fileUrl.toLocalFile();
    const QString path = localPath.isEmpty() ? fileUrl.path() : localPath;

    // Guard against the job being destroyed before the async reply arrives.
    QPointer<QWebEngineUrlRequestJob> guard(job);
    const QJsonObject params{{QStringLiteral("path"), path}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                guard->fail(QWebEngineUrlRequestJob::RequestFailed);
                return;
            }
            const QJsonObject obj = result.toObject();
            const QString encoding = obj.value(QStringLiteral("encoding")).toString();
            const QString content = obj.value(QStringLiteral("content")).toString();
            const QByteArray bytes =
                encoding == QLatin1String("base64")
                    ? QByteArray::fromBase64(content.toUtf8())
                    : content.toUtf8();

            auto *buffer = new QBuffer(guard);
            buffer->setData(bytes);
            buffer->open(QIODevice::ReadOnly);
            guard->reply(InternalUrlSchemeHandler::mimeForPath(path), buffer);
        });
}

} // namespace ch
