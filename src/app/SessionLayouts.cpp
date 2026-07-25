#include "SessionLayouts.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QStringView>

#include <cmath>
#include <utility>

namespace ch {

namespace {

// Decode a QML-authored (or otherwise foreign) tree. Returns std::nullopt when
// the value is not a valid split tree, so an invalid layout is rejected instead
// of being silently replaced by an empty leaf.
//
// SplitNode::fromJson returns the empty-leaf sentinel BOTH for a genuine leaf
// and for input it rejected; the persisted "type" tag is what disambiguates the
// two. A split that parses is necessarily valid (it has children), which is why
// only the leaf case needs the tag check.
std::optional<SplitNode> parseVariantTree(const QVariant& value)
{
    const QJsonValue json = QJsonValue::fromVariant(value);
    if (!json.isObject())
        return std::nullopt;
    const QJsonObject obj = json.toObject();
    const SplitNode node = SplitNode::fromJson(obj);
    if (!node.isLeaf())
        return node;
    if (obj.value(QStringLiteral("type")).toString() == QStringLiteral("leaf"))
        return node;
    return std::nullopt;
}

// Depth-first search for the leaf carrying `paneId`. On success `outNode` points
// at the leaf, `outParent` at the branch holding it (nullptr when the leaf IS
// the root) and `outIndex` at its position among that branch's children.
bool locateLeaf(SplitNode& node, SplitNode* parent, int indexInParent,
                const QString& paneId, SplitNode*& outNode,
                SplitNode*& outParent, int& outIndex)
{
    if (node.isLeaf()) {
        if (node.paneId != paneId)
            return false;
        outNode = &node;
        outParent = parent;
        outIndex = indexInParent;
        return true;
    }
    for (int i = 0; i < node.children.size(); ++i) {
        if (locateLeaf(node.children[i], &node, i, paneId, outNode, outParent,
                       outIndex))
            return true;
    }
    return false;
}

// Highest n over every "<prefix>n" leaf paneId in the tree, or 0 when none
// matches. Makes the generated ids deterministic for a given tree.
void collectMaxPaneSuffix(const SplitNode& node, const QString& prefix,
                          int& highest)
{
    if (node.isLeaf()) {
        if (!node.paneId.startsWith(prefix))
            return;
        bool ok = false;
        const int n = QStringView(node.paneId).mid(prefix.size()).toInt(&ok);
        if (ok && n > highest)
            highest = n;
        return;
    }
    for (const SplitNode& child : node.children)
        collectMaxPaneSuffix(child, prefix, highest);
}

} // namespace

SessionLayouts::SessionLayouts(WorkspaceDb* db, QObject* parent)
    : QObject(parent)
    , m_db(db)
{
}

QVariant SessionLayouts::viewerTree() const
{
    return m_regions[kViewer].cache;
}

QVariant SessionLayouts::terminalTree() const
{
    return m_regions[kTerminal].cache;
}

void SessionLayouts::setDevSessionId(QString devSessionId)
{
    if (devSessionId == m_devSessionId)
        return;
    load(std::move(devSessionId));
}

void SessionLayouts::setServerId(QString serverId)
{
    if (serverId == m_serverId)
        return;
    m_serverId = std::move(serverId);
    emit serverIdChanged();
}

int SessionLayouts::regionIndex(const QString& region)
{
    if (region == QStringLiteral("viewer"))
        return kViewer;
    if (region == QStringLiteral("terminal"))
        return kTerminal;
    emit error(QStringLiteral("SessionLayouts: unknown region \"%1\"").arg(region));
    return -1;
}

Region SessionLayouts::regionEnum(int index)
{
    return index == kTerminal ? Region::Terminal : Region::Viewer;
}

QString SessionLayouts::defaultPaneId(int index)
{
    return index == kTerminal ? QStringLiteral("terminal-1")
                              : QStringLiteral("viewer-1");
}

void SessionLayouts::setTree(int index, SplitNode tree)
{
    setTreeQuietly(index, std::move(tree));
    if (index == kTerminal)
        emit terminalTreeChanged();
    else
        emit viewerTreeChanged();
}

void SessionLayouts::setTreeQuietly(int index, SplitNode tree)
{
    RegionState& state = m_regions[index];
    state.tree = std::move(tree);
    state.valid = true;
    // Publish the exact persisted wire shape; QML reads paneId/orientation/
    // children/ratios straight off this map.
    state.cache = state.tree.toJson().toVariantMap();
}

void SessionLayouts::clearTrees()
{
    for (int index = 0; index < kRegionCount; ++index) {
        RegionState& state = m_regions[index];
        if (!state.valid)
            continue; // already null; no redundant signal
        state = RegionState{};
        if (index == kTerminal)
            emit terminalTreeChanged();
        else
            emit viewerTreeChanged();
    }
}

void SessionLayouts::load(QString devSessionId)
{
    const bool sessionChanged = devSessionId != m_devSessionId;
    // Bump first: any reply still on the wire now belongs to a superseded
    // generation and will be dropped by applyLoadedTree.
    const quint64 generation = ++m_generation;
    m_devSessionId = std::move(devSessionId);
    m_pendingLoads = 0;
    if (sessionChanged) {
        emit devSessionIdChanged();
        // The previous session's panes must not linger while the new layout is
        // in flight, and must not be edited into the new session either.
        clearTrees();
    }
    if (m_devSessionId.isEmpty())
        return; // deselection: nothing to fetch

    m_pendingLoads = kRegionCount;
    QPointer<SessionLayouts> self(this);
    const DevSessionId sessionId{m_devSessionId};
    for (int index = 0; index < kRegionCount; ++index) {
        m_db->getLayout(sessionId, regionEnum(index),
                        [self, generation, index](std::optional<SplitNode> tree,
                                                  std::optional<RpcError> err) {
                            if (!self)
                                return;
                            self->applyLoadedTree(generation, index,
                                                  std::move(tree),
                                                  std::move(err));
                        });
    }
}

void SessionLayouts::applyLoadedTree(quint64 generation, int index,
                                     std::optional<SplitNode> tree,
                                     std::optional<RpcError> err)
{
    // A real server error is worth surfacing even when superseded (same rule as
    // AppController::refresh).
    if (err)
        emit error(err->message);
    if (generation != m_generation)
        return; // a newer load() has taken over; drop this reply entirely

    if (!err) {
        // No persisted layout for this region -> the SPEC 4.5 "always one pane"
        // default, held only in memory: a plain selection must not write to the
        // server. It is persisted by the first real edit.
        SplitNode next = tree ? std::move(*tree) : SplitNode{};
        if (!tree)
            next.paneId = defaultPaneId(index);
        RegionState& state = m_regions[index];
        if (!state.valid || !(state.tree == next))
            setTree(index, std::move(next));
    }

    if (m_pendingLoads > 0 && --m_pendingLoads == 0)
        emit loaded(m_devSessionId);
}

bool SessionLayouts::canEdit()
{
    if (m_devSessionId.isEmpty()) {
        emit error(QStringLiteral(
            "SessionLayouts: no Dev Session selected; layout edit ignored"));
        return false;
    }
    if (m_serverId.isEmpty()) {
        emit error(QStringLiteral(
            "SessionLayouts: no server selected; layout edit ignored"));
        return false;
    }
    return true;
}

void SessionLayouts::persist(int index)
{
    // Callers gate on canEdit(), so both ids are set here.
    QPointer<SessionLayouts> self(this);
    m_db->setLayout(ServerId{m_serverId}, DevSessionId{m_devSessionId},
                    regionEnum(index), m_regions[index].tree,
                    [self](std::optional<SplitNode>,
                           std::optional<RpcError> err) {
                        // The server stores and echoes the tree verbatim
                        // (remote/src/workspace.ts setLayout), so there is
                        // nothing to adopt on success - and re-publishing an
                        // identical tree would needlessly rebuild every pane.
                        if (self && err)
                            emit self->error(err->message);
                    });
}

void SessionLayouts::saveTree(QString region, QVariant tree)
{
    const int index = regionIndex(region);
    if (index < 0 || !canEdit())
        return;
    const std::optional<SplitNode> parsed = parseVariantTree(tree);
    if (!parsed) {
        emit error(QStringLiteral(
            "SessionLayouts: %1 tree is not a valid split tree; not saved")
                       .arg(region));
        return;
    }
    // Quiet: the caller already holds this tree (see the header's signal
    // discipline note).
    setTreeQuietly(index, *parsed);
    persist(index);
}

void SessionLayouts::setRatios(QString region, QStringList pathIndexes,
                               QVariantList ratios)
{
    const int index = regionIndex(region);
    if (index < 0 || !canEdit())
        return;
    RegionState& state = m_regions[index];
    if (!state.valid) {
        emit error(QStringLiteral(
            "SessionLayouts: %1 layout not loaded; ratios ignored").arg(region));
        return;
    }

    // Walk the index path from the root to the branch being resized.
    SplitNode* node = &state.tree;
    for (const QString& step : pathIndexes) {
        bool ok = false;
        const int childIndex = step.toInt(&ok);
        if (!ok || node->isLeaf() || childIndex < 0
            || childIndex >= node->children.size()) {
            emit error(QStringLiteral(
                "SessionLayouts: no %1 node at path [%2]")
                           .arg(region, pathIndexes.join(QLatin1Char(','))));
            return;
        }
        node = &node->children[childIndex];
    }
    if (node->isLeaf()) {
        emit error(QStringLiteral(
            "SessionLayouts: %1 node at path [%2] is a leaf and has no ratios")
                       .arg(region, pathIndexes.join(QLatin1Char(','))));
        return;
    }
    if (ratios.size() != node->children.size()) {
        emit error(QStringLiteral("SessionLayouts: expected %1 %2 ratios, got %3")
                       .arg(node->children.size())
                       .arg(region)
                       .arg(ratios.size()));
        return;
    }

    QVector<double> parsed;
    parsed.reserve(ratios.size());
    for (const QVariant& value : ratios) {
        bool ok = false;
        const double ratio = value.toDouble(&ok);
        // Same rule SplitNode::fromJson enforces: a non-finite or non-positive
        // ratio yields broken geometry and would not survive a round-trip.
        if (!ok || !std::isfinite(ratio) || ratio <= 0.0) {
            emit error(QStringLiteral(
                "SessionLayouts: invalid %1 ratio \"%2\"")
                           .arg(region, value.toString()));
            return;
        }
        parsed.append(ratio);
    }

    node->ratios = std::move(parsed);
    // Quiet: the drag already resized the panes; re-publishing the tree would
    // destroy and rebuild them.
    setTreeQuietly(index, state.tree);
    persist(index);
}

QString SessionLayouts::splitPane(QString region, QString paneId,
                                  QString orientation)
{
    const int index = regionIndex(region);
    if (index < 0 || !canEdit())
        return {};
    const bool vertical = orientation == QStringLiteral("vertical");
    if (!vertical && orientation != QStringLiteral("horizontal")) {
        emit error(QStringLiteral(
            "SessionLayouts: unknown orientation \"%1\"").arg(orientation));
        return {};
    }
    RegionState& state = m_regions[index];
    if (!state.valid) {
        emit error(QStringLiteral(
            "SessionLayouts: %1 layout not loaded; split ignored").arg(region));
        return {};
    }

    SplitNode* leaf = nullptr;
    SplitNode* parent = nullptr;
    int childIndex = -1;
    if (!locateLeaf(state.tree, nullptr, -1, paneId, leaf, parent, childIndex)) {
        emit error(QStringLiteral("SessionLayouts: no %1 pane \"%2\" to split")
                       .arg(region, paneId));
        return {};
    }

    int highest = 0;
    const QString prefix = (index == kTerminal ? QStringLiteral("terminal-")
                                               : QStringLiteral("viewer-"));
    collectMaxPaneSuffix(state.tree, prefix, highest);
    const QString newPaneId = prefix + QString::number(highest + 1);

    if (leaf->paneId.isEmpty()) {
        // The empty leaf a region is left with after its last pane closed is a
        // placeholder, not a pane: splitting it would strand a permanently dead
        // half. Fill it in place so an emptied region can be used again - the
        // only handle QML has on a region with no panes.
        leaf->paneId = newPaneId;
    } else {
        SplitNode newLeaf;
        newLeaf.paneId = newPaneId;
        SplitNode branch;
        branch.orientation =
            vertical ? SplitOrientation::Vertical : SplitOrientation::Horizontal;
        // *leaf is copied into the branch before the branch overwrites it.
        branch.children = {*leaf, newLeaf};
        branch.ratios = {1.0, 1.0}; // equal halves; a drag persists real ratios
        *leaf = std::move(branch);
    }

    setTree(index, state.tree);
    persist(index);
    return newPaneId;
}

void SessionLayouts::closePane(QString region, QString paneId)
{
    const int index = regionIndex(region);
    if (index < 0 || !canEdit())
        return;
    RegionState& state = m_regions[index];
    if (!state.valid) {
        emit error(QStringLiteral(
            "SessionLayouts: %1 layout not loaded; close ignored").arg(region));
        return;
    }

    SplitNode* leaf = nullptr;
    SplitNode* parent = nullptr;
    int childIndex = -1;
    if (!locateLeaf(state.tree, nullptr, -1, paneId, leaf, parent, childIndex)) {
        emit error(QStringLiteral("SessionLayouts: no %1 pane \"%2\" to close")
                       .arg(region, paneId));
        return;
    }

    if (!parent) {
        // The region's only pane: a region always has a tree, so leave a single
        // EMPTY leaf rather than an empty (unrenderable) tree.
        state.tree = SplitNode{};
    } else {
        parent->children.removeAt(childIndex);
        parent->ratios.removeAt(childIndex);
        if (parent->children.size() == 1) {
            // A branch with one child is not a split any more; hoist the
            // survivor (copied out before the assignment) into its place.
            const SplitNode survivor = parent->children.at(0);
            *parent = survivor;
        }
    }

    setTree(index, state.tree);
    persist(index);
}

} // namespace ch
