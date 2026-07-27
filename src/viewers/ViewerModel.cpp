#include "ViewerModel.h"

#include "CodeharbordClient.h"
#include "InternalUrlSchemeHandler.h"
#include "RpcTypes.h"
#include "ViewerHandlerRegistry.h"
#include "ViewerProfiles.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QVariantMap>
#include <QtWebEngineQuick/QQuickWebEngineProfile>
#include <algorithm>

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

void ViewerModel::readTextFile(const QString &path)
{
    if (!m_client) {
        emit textFileError(path, QStringLiteral("no remote client is connected"));
        return;
    }

    const QJsonObject params{{QStringLiteral("path"), path}};
    QPointer<ViewerModel> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                emit guard->textFileError(path, error->message);
                return;
            }
            const QJsonObject obj = result.toObject();
            const QString encoding =
                obj.value(QStringLiteral("encoding")).toString();
            const QString content =
                obj.value(QStringLiteral("content")).toString();
            // Text is served utf-8; decode a base64 payload defensively so the
            // view still shows something legible for near-text binaries.
            const QString text =
                encoding == QLatin1String("base64")
                    ? QString::fromUtf8(QByteArray::fromBase64(content.toUtf8()))
                    : content;
            emit guard->textFileRead(path, text);
        });
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
            // Server order is unspecified; sort directories first, then by name.
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
                          return ma.value(QStringLiteral("name"))
                                     .toString()
                                     .compare(mb.value(QStringLiteral("name"))
                                                  .toString(),
                                              Qt::CaseInsensitive)
                                 < 0;
                      });
            emit guard->directoryListed(path, entries);
        });
}

} // namespace ch
