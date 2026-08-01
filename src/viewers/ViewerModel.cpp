#include "ViewerModel.h"

#include "CodeharbordClient.h"
#include "InternalUrlSchemeHandler.h"
#include "RpcTypes.h"
#include "ViewerHandlerRegistry.h"
#include "ViewerProfiles.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
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
    case ViewerResolution::Error:
        break;
    }
    // Download and Error both fall back to the binary view, which shows
    // metadata plus a download/open affordance. This is the "unclaimed
    // resource" disposition SPEC 7.5 mandates: an unrecognised resource stays
    // with the browser and becomes a download/metadata view, never a text
    // buffer.
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
    wireProfileSignals();
}

ViewerProfiles *ViewerModel::profiles()
{
    if (!m_profiles) {
        m_profiles = new ViewerProfiles(m_client, this);
        m_ownsProfiles = true;
        wireProfileSignals();
    }
    return m_profiles;
}

void ViewerModel::wireProfileSignals()
{
    // Drop whatever the previous ViewerProfiles was connected through: a model
    // handed profiles A, then B (or nothing) must not keep forwarding A's
    // reports.
    disconnect(m_handlerConnection);
    if (!m_profiles)
        return;
    m_handlerConnection =
        connect(m_profiles->internalSchemeHandler(),
                &InternalUrlSchemeHandler::requestFailed, this,
                [this](const QUrl &internalUrl,
                       InternalUrlSchemeHandler::Failure,
                       const QString &message) {
                    emit internalResourceError(internalUrl, message);
                });
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

bool ViewerModel::retainInternalUrl(const QString &internalUrl)
{
    return m_map->retain(internalUrl);
}

void ViewerModel::releaseInternalUrl(const QString &internalUrl)
{
    m_map->release(internalUrl);
}

QQuickWebEngineProfile *ViewerModel::externalProfile()
{
    return profiles()->externalProfile();
}

QQuickWebEngineProfile *ViewerModel::internalProfile()
{
    return profiles()->internalProfile();
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
