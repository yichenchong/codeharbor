#include "PaneListModel.h"

#include "SplitTree.h"
#include "ViewerHandlerRegistry.h"

#include <QCoreApplication>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

namespace ch {
namespace {

// The two region words. They are the same strings the daemon stores layouts
// under (workspace.getLayout's `region` parameter), so a paneKey read out of
// UiStateStore by one shell is understood by the other.
const QString& viewerRegion()
{
    static const QString value = QStringLiteral("viewer");
    return value;
}

const QString& terminalRegion()
{
    static const QString value = QStringLiteral("terminal");
    return value;
}

// What a viewer leaf's stored url is CALLED in the picker. A leaf with no url
// has nothing open, and on mobile that is not an empty pane the user has to fill
// in: it is the session's repository root, which is the one place the directory
// browser can start from with no further input.
QString viewerTitleForUrl(const QUrl& url, const QString& raw)
{
    if (raw.isEmpty())
        return QCoreApplication::translate("ch::PaneListModel", "Session root");

    // Empty for a directory url, which ends in '/' by the viewer registry's own
    // convention (see ViewerHandlerRegistry::resolve) - so fall back to the last
    // non-empty path segment, which is the directory's own name.
    QString name = url.fileName();
    if (name.isEmpty()) {
        const QStringList segments = url.path().split(QLatin1Char('/'),
                                                      Qt::SkipEmptyParts);
        if (!segments.isEmpty())
            name = segments.last();
    }
    // An http(s) url with no path at all is named by its host; anything else
    // that got this far is shown verbatim rather than as a blank row.
    if (name.isEmpty())
        name = url.host();
    if (name.isEmpty())
        name = raw;
    return name;
}

// Which single mobile page a viewer leaf resolves to. The classification is the
// SAME one the desktop uses - ch::ViewerHandlerRegistry, now reachable without
// WebEngine as ch_viewers_core - so the two shells cannot disagree about what a
// file is. The first applicable kind is the registry's documented default.
//
// A leaf with no url is the session root, i.e. a directory listing. A url the
// registry claims nothing for (an unknown scheme) is "unsupported", which is a
// kind the host page has a real page for; inventing "text" here would hand an
// arbitrary remote resource to the editor.
QString viewerKindForUrl(const QUrl& url, const QString& raw)
{
    if (raw.isEmpty())
        return QStringLiteral("directory");
    const QStringList kinds = ViewerHandlerRegistry::applicableViewKinds(url);
    if (kinds.isEmpty())
        return QStringLiteral("unsupported");
    return kinds.first();
}

} // namespace

PaneListModel::PaneListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int PaneListModel::rowCount(const QModelIndex& parent) const
{
    // A list model has rows only under the invisible root; a valid parent index
    // is a request for the children of a row, and this model has none.
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_panes.size());
}

QVariant PaneListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_panes.size())
        return {};
    const Pane& pane = m_panes.at(index.row());
    switch (role) {
    case PaneKeyRole:
        return pane.paneKey;
    case RegionRole:
        return pane.region;
    case PaneIdRole:
        return pane.paneId;
    case UrlRole:
        return pane.url;
    case TerminalPaneIdRole:
        return pane.terminalPaneId;
    case TitleRole:
        return pane.title;
    case KindRole:
        return pane.kind;
    default:
        return {};
    }
}

QHash<int, QByteArray> PaneListModel::roleNames() const
{
    return {
        {PaneKeyRole, QByteArrayLiteral("paneKey")},
        {RegionRole, QByteArrayLiteral("region")},
        {PaneIdRole, QByteArrayLiteral("paneId")},
        {UrlRole, QByteArrayLiteral("url")},
        {TerminalPaneIdRole, QByteArrayLiteral("terminalPaneId")},
        {TitleRole, QByteArrayLiteral("title")},
        {KindRole, QByteArrayLiteral("kind")},
    };
}

bool PaneListModel::flattenRegion(const QVariant& tree, const QString& region,
                                  int depth, QVector<Pane>& out)
{
    // The SAME bound the parser, the writer and operator== enforce, so no tree
    // this model is handed can drive it into stack exhaustion whatever its
    // provenance. "Same" means the same COUNTING too: SplitTree.cpp starts its
    // root at depth 1, so this must be entered with 1 for the root, or a tree
    // one level deeper than any tree that can be parsed would be walked here.
    if (depth > SplitNode::kMaxDepth)
        return false;
    // Null is "this region is not loaded", which is a legitimate state and not a
    // malformed tree: it contributes nothing and is not an error.
    if (!tree.isValid() || tree.isNull())
        return true;
    if (tree.metaType().id() != QMetaType::QVariantMap)
        return false;

    const QVariantMap node = tree.toMap();
    const QString type = node.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("split")) {
        const QVariantList children =
            node.value(QStringLiteral("children")).toList();
        // A split with no children is a shape the parser rejects; treating it as
        // "no panes here" instead would quietly hide the rest of the region.
        if (children.isEmpty())
            return false;
        for (const QVariant& child : children) {
            if (!flattenRegion(child, region, depth + 1, out))
                return false;
        }
        return true;
    }

    if (type != QLatin1String("leaf"))
        return false;

    Pane pane;
    pane.region = region;
    pane.paneId = node.value(QStringLiteral("paneId")).toString();
    // The EMPTY paneId is the placeholder leaf an emptied region is left with
    // (SessionLayouts::closePane). It is not a pane: it has no content, no
    // identity and nothing to show, and it exists only so the desktop regions
    // have a handle to split. It is skipped rather than listed, so the picker
    // never offers a row that opens nothing - and skipping is not an error,
    // because a region with no panes is a perfectly good persisted state.
    if (pane.paneId.isEmpty())
        return true;

    pane.paneKey = region + QLatin1Char(':') + pane.paneId;
    // A DUPLICATE paneId within one region is not a second pane. The key the
    // shell selects by would be identical, so the row could only ever open the
    // first one, while the picker showed two rows and highlighted both as
    // selected. Slot labels are minted from a per-region counter and cannot
    // collide by accident, but nothing in SplitNode enforces uniqueness, so a
    // corrupt or hostile tree can carry two - and dropping the whole region over
    // it would cost the user every pane in it. A linear scan is right here: one
    // Dev Session's pane count is a handful, and `out` is exactly the panes of
    // this one region.
    for (const Pane& existing : out) {
        if (existing.paneKey == pane.paneKey)
            return true;
    }
    pane.url = node.value(QStringLiteral("url")).toString();
    pane.terminalPaneId =
        node.value(QStringLiteral("terminalPaneId")).toString();

    if (region == terminalRegion()) {
        // The user's own title wins over the generated slot label, exactly as
        // the desktop pane header resolves it (SplitNode::customTitle).
        const QString customTitle = SplitNode::normalizeCustomTitle(
            node.value(QStringLiteral("customTitle")).toString());
        pane.title = customTitle.isEmpty() ? pane.paneId : customTitle;
        pane.kind = terminalRegion();
    } else {
        const QUrl url(pane.url);
        pane.title = viewerTitleForUrl(url, pane.url);
        pane.kind = viewerKindForUrl(url, pane.url);
    }

    out.push_back(std::move(pane));
    return true;
}

void PaneListModel::setTrees(const QVariant& viewerTree,
                             const QVariant& terminalTree)
{
    QVector<Pane> panes;
    // Viewer region first: it is the region a Dev Session is about (a file, a
    // document, a directory), and the terminals are the tools acting on it. The
    // desktop puts them in the same order left to right.
    // Depth 1 for the root, counting exactly as SplitTree.cpp's parser does.
    QVector<Pane> viewerPanes;
    if (flattenRegion(viewerTree, viewerRegion(), 1, viewerPanes))
        panes += viewerPanes;
    QVector<Pane> terminalPanes;
    if (flattenRegion(terminalTree, terminalRegion(), 1, terminalPanes))
        panes += terminalPanes;

    // A full reset, not a diff. The list is a menu rebuilt from an authoritative
    // republish, it is bounded by the number of panes in one Dev Session, and
    // the mobile picker holds no per-row state (no selection lives in the
    // delegates - MobileAppController owns the selected paneKey), so there is
    // nothing a diff would preserve.
    beginResetModel();
    m_panes = std::move(panes);
    endResetModel();
}

QVariantMap PaneListModel::paneByKey(const QString& paneKey) const
{
    for (const Pane& pane : m_panes) {
        if (pane.paneKey != paneKey)
            continue;
        return QVariantMap{
            {QStringLiteral("paneKey"), pane.paneKey},
            {QStringLiteral("region"), pane.region},
            {QStringLiteral("paneId"), pane.paneId},
            {QStringLiteral("url"), pane.url},
            {QStringLiteral("terminalPaneId"), pane.terminalPaneId},
            {QStringLiteral("title"), pane.title},
            {QStringLiteral("kind"), pane.kind},
        };
    }
    return {};
}

} // namespace ch
