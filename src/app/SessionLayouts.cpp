#include "SessionLayouts.h"

#include <QJsonArray>
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
// of being silently replaced by an empty leaf - which is a legitimate tree in
// its own right (it is what a region with no panes left persists), not a
// "nothing here" marker. SplitNode::tryFromJson draws that distinction from the
// parser itself; see its comment for why fromJson() cannot.
std::optional<SplitNode> parseVariantTree(const QVariant& value)
{
    const QJsonValue json = QJsonValue::fromVariant(value);
    if (!json.isObject())
        return std::nullopt;
    return SplitNode::tryFromJson(json.toObject());
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

// Does any LEAF of `node` satisfy `pred`? Short-circuits.
template <typename Predicate>
bool anyLeaf(const SplitNode& node, Predicate pred)
{
    if (node.isLeaf())
        return pred(node);
    for (const SplitNode& child : node.children) {
        if (anyLeaf(child, pred))
            return true;
    }
    return false;
}

// Every non-empty leaf paneId in the tree, in depth-first order.
void collectLeafPaneIds(const SplitNode& node, QStringList& out)
{
    if (node.isLeaf()) {
        if (!node.paneId.isEmpty())
            out.append(node.paneId);
        return;
    }
    for (const SplitNode& child : node.children)
        collectLeafPaneIds(child, out);
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

// Every terminal leaf in `node` that carries no row id, i.e. every leaf this
// client is allowed to resolve by its slot label. Only ever fed a tree the
// SERVER sent: a leaf minted here gets its id from a mint, and one still
// waiting for that mint must NOT be mistaken for a pre-migration leaf.
void collectLegacyTerminalSlots(const SplitNode& node, QSet<QString>& out)
{
    if (node.isLeaf()) {
        if (node.terminalPaneId.isEmpty() && !node.paneId.isEmpty())
            out.insert(node.paneId);
        return;
    }
    for (const SplitNode& child : node.children)
        collectLegacyTerminalSlots(child, out);
}

// Mark the leaves in `obj` (a SplitNode::toJson() tree) that `legacy` names, so
// QML can tell "resolve this by its old label, once" from "your row is being
// minted, wait". Applied to the published copy only - `obj` here is already
// detached from the SplitNode, and the marker is never handed to setLayout.
void markLegacyLeaves(QJsonObject& obj, const QSet<QString>& legacy)
{
    const QJsonArray children = obj.value(QStringLiteral("children")).toArray();
    if (children.isEmpty()) {
        if (legacy.contains(obj.value(QStringLiteral("paneId")).toString()))
            obj[QStringLiteral("terminalLegacy")] = true;
        return;
    }
    QJsonArray marked;
    for (const QJsonValue& value : children) {
        QJsonObject child = value.toObject();
        markLegacyLeaves(child, legacy);
        marked.append(child);
    }
    obj[QStringLiteral("children")] = marked;
}

} // namespace

SessionLayouts::SessionLayouts(WorkspaceDb* db, UiStateStore* uiState,
                               QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_uiState(uiState)
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
    // A Dev Session belongs to exactly one server, so a tree loaded for the
    // PREVIOUS server's session must not stay editable under the new key: the
    // next splitPane/closePane/setRatios would persist it as
    // setLayout(NEW serverId, OLD devSessionId) and write one server's layout
    // into another server's row. load(QString()) drops the id and both region
    // trees and issues no request. AppController::setServerId already does this
    // from its side; enforcing it here too means the guarantee does not depend
    // on the order in which one caller happens to drive the two setters.
    if (!m_devSessionId.isEmpty())
        load(QString());
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

QString SessionLayouts::regionKey(int index)
{
    return index == kTerminal ? QStringLiteral("terminal")
                              : QStringLiteral("viewer");
}

int SessionLayouts::reservePaneSuffix(int index, const SplitNode& tree)
{
    const QString region = regionKey(index);
    int highest = 0;
    collectMaxPaneSuffix(tree, region + QLatin1Char('-'), highest);
    const int stored = m_uiState->nextPaneSuffix(m_devSessionId, region);
    const int next = qMax(stored, highest + 1);
    // Only write when the counter actually moved: this runs on every published
    // tree, including the one a splitter drag republishes, and each write syncs
    // to disk.
    if (next != stored)
        m_uiState->setNextPaneSuffix(m_devSessionId, region, next);
    return next;
}

SplitNode SessionLayouts::defaultTree(int index)
{
    if (index != kTerminal) {
        SplitNode only;
        only.paneId = QStringLiteral("viewer-1");
        return only;
    }

    SplitNode top;
    top.paneId = QStringLiteral("terminal-1");
    SplitNode bottom;
    bottom.paneId = QStringLiteral("terminal-2");
    SplitNode stacked;
    // "vertical" stacks children top to bottom, so terminal-1 is the upper pane.
    stacked.orientation = SplitOrientation::Vertical;
    stacked.children = {top, bottom};
    stacked.ratios = {1.0, 1.0};
    return stacked;
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
    // Burn the suffixes this tree carries HERE, the one funnel every tree
    // assignment goes through (a loaded tree, a QML-authored saveTree, a split,
    // a close, a ratio drag). Doing it at mint time only would not be enough:
    // closePane() removes the pane from the tree, so by the time the next split
    // looked at the tree the closed pane's suffix would look free again - and
    // the user would be shown a label they had just closed.
    reservePaneSuffix(index, state.tree);
    // Publish the exact persisted wire shape; QML reads paneId/orientation/
    // children/ratios straight off this map.
    QJsonObject published = state.tree.toJson();
    // Plus, for the terminal region only, the one field that is published but
    // never persisted: which id-less leaves are pre-migration leaves the pane
    // may resolve by label. See m_legacyTerminalSlots.
    if (index == kTerminal && !m_legacyTerminalSlots.isEmpty())
        markLegacyLeaves(published, m_legacyTerminalSlots);
    state.cache = published.toVariantMap();
}

void SessionLayouts::clearTrees()
{
    // The set names slots of the CURRENT Dev Session's terminal layout. Carrying
    // it into another session would grant a same-named slot there permission to
    // resolve by label, which is the one thing that must be earned per leaf.
    m_legacyTerminalSlots.clear();
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
    // A deliberate reload adopts whatever the server has, so no earlier edit
    // may veto it; only an edit made from HERE ON supersedes these replies.
    for (RegionState& state : m_regions)
        state.superseded = false;
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

    // `superseded`: an edit was persisted while this getLayout was on the wire,
    // so its answer is older than the tree QML is already showing. Applying it
    // would revert the edit - and on the seeding branch below would replace it
    // with the region default and write THAT to the server. See RegionState.
    if (!err && !m_regions[index].superseded) {
        // No persisted layout for this region: adopt the default AND write it
        // back, so it becomes the session's real layout instead of a shape that
        // is re-derived on every load and lost the moment anything saves a tree.
        //
        // This is a deliberate reversal. The default used to be memory-only,
        // reasoning that "a plain selection must not write to the server". That
        // held while the default was one leaf, because one leaf is also what a
        // region collapses to and what QML falls back to, so nothing could
        // disagree with it. It does not hold for a MULTI-pane default: the QML
        // fallback node is a single pane, and the first setRatios/setPaneUrl
        // would persist whatever tree the region happened to be showing. The
        // two stacked terminals have to be in the database to survive a restart,
        // and the only moment we know a session has no layout is right here.
        //
        // Idempotent by construction: the write creates the row, so the next
        // load for this session takes the `tree` branch and seeds nothing. Two
        // clients racing both write the identical default.
        const bool seeding = !tree;
        SplitNode next = tree ? std::move(*tree) : defaultTree(index);
        // Which terminal leaves may resolve by their slot LABEL. Recomputed
        // from scratch on every adopted server tree, and ONLY from a server
        // tree: a leaf the server has with no row id was written before layouts
        // carried one, so its label really is its historical key. A leaf we
        // seeded or split ourselves is not in here and never becomes so - it
        // waits for the row being minted for it (see persist()).
        if (index == kTerminal) {
            QSet<QString> legacy;
            if (!seeding)
                collectLegacyTerminalSlots(next, legacy);
            m_legacyTerminalSlots = std::move(legacy);
        }

        RegionState& state = m_regions[index];
        if (!state.valid || !(state.tree == next))
            setTree(index, std::move(next));
        // Not canEdit(): a load with no server selected yet is not a misuse and
        // must not raise an error. The in-memory default still stands, and the
        // first real edit persists it as before.
        if (seeding && !m_devSessionId.isEmpty() && !m_serverId.isEmpty()) {
            // Every terminal leaf of a freshly seeded default is brand new and
            // needs a row of its own. persist() will hold the write back until
            // the last of them lands, so the default reaches the server with
            // its ids already in it rather than as id-less leaves a later load
            // would have to read as pre-migration ones.
            if (index == kTerminal) {
                // Not "slots": that is a Qt keyword macro.
                QStringList labels;
                collectLeafPaneIds(m_regions[index].tree, labels);
                for (const QString& label : labels)
                    mintTerminalPaneRow(generation, label);
            }
            persist(index);
        }
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

// Is any terminal leaf still waiting for its `terminal_panes` row? A leaf is
// pending when it has no row id AND is not a pre-migration leaf this client has
// permission to resolve by label. The empty placeholder leaf an emptied region
// keeps is not a pane and never pending.
bool SessionLayouts::hasPendingTerminalLeaf() const
{
    const RegionState& state = m_regions[kTerminal];
    if (!state.valid)
        return false;
    return anyLeaf(state.tree, [this](const SplitNode& leaf) {
        return !leaf.paneId.isEmpty() && leaf.terminalPaneId.isEmpty()
                && !m_legacyTerminalSlots.contains(leaf.paneId);
    });
}

void SessionLayouts::persist(int index)
{
    // Callers gate on canEdit(), so both ids are set here.
    //
    // Every write also retires any getLayout still on the wire for this region:
    // its answer predates this tree and applying it would undo the edit being
    // persisted right now.
    m_regions[index].superseded = true;
    // THE invariant that makes the whole scheme decidable: an id-less terminal
    // leaf on the SERVER always means "written before layouts carried row ids",
    // and is therefore always safe to resolve by its slot label once. A leaf
    // whose row is merely still being minted looks identical on the wire, so it
    // must never get there - otherwise a reload would read it as pre-migration
    // and hand it whatever shell wears its label. The mint's own persist writes
    // this tree, ratio drags and all, the moment the last id lands.
    if (index == kTerminal && hasPendingTerminalLeaf())
        return;
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

    // Every earlier return above left the tree untouched, so the counter is only
    // consumed once the split is certain to happen.
    const int suffix = reservePaneSuffix(index, state.tree);
    const QString newPaneId =
        regionKey(index) + QLatin1Char('-') + QString::number(suffix);
    // Consume it: this id is now spent for this Dev Session's region for good,
    // whether or not the pane holding it is ever closed.
    m_uiState->setNextPaneSuffix(m_devSessionId, regionKey(index), suffix + 1);

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

    // Published straight away: the split has to feel instant, and the new pane
    // has real chrome to show while its identity is on the wire.
    setTree(index, state.tree);
    if (index == kTerminal) {
        // The new leaf has no `terminal_panes` row yet, so it is PENDING and
        // persist() below will decline to write the tree until the mint lands.
        // Nothing about this pane resolves in the meantime - notably it does
        // NOT fall back to its slot label, which is how a recycled label used
        // to hand a new pane a closed pane's still-running shell.
        mintTerminalPaneRow(m_generation, newPaneId);
    }
    persist(index);
    return newPaneId;
}

void SessionLayouts::mintTerminalPaneRow(quint64 generation, const QString& paneId)
{
    CreateTerminalPaneParams params;
    params.serverId = ServerId{m_serverId};
    params.devSessionId = DevSessionId{m_devSessionId};
    // The row's `name` is this leaf's slot LABEL, and only a label: it is not
    // required to be free, and nothing resolves by it except a pre-migration
    // leaf. It is sent so the row is legible in the Dev Session's terminal pane
    // list rather than being an anonymous UUID.
    params.name = paneId;
    const QString devSessionId = m_devSessionId;
    QPointer<SessionLayouts> self(this);
    m_db->createTerminalPane(
        params, [self, generation, paneId, devSessionId](std::optional<TerminalPane> pane,
                                                         std::optional<RpcError> err) {
            if (!self)
                return;
            // A load has taken over since. Its tree is authoritative and this
            // leaf may not even be in it; the row itself stays on the server,
            // enumerable through the Dev Session's terminal pane list, rather
            // than being deleted from under a shell that may already be running.
            if (self->m_generation != generation || self->m_devSessionId != devSessionId)
                return;

            SplitNode* leaf = self->findTerminalLeaf(paneId);
            if (err || !pane || pane->id.value.isEmpty()) {
                emit self->error(
                    err ? err->message
                        : QStringLiteral("the server created this terminal without an id"));
                // Take the half-made pane back out rather than leave one that
                // can never attach and can never be told apart from a
                // pre-migration leaf if anything later persisted it.
                if (leaf && self->dropLeaf(kTerminal, paneId)) {
                    self->setTree(kTerminal, self->m_regions[kTerminal].tree);
                    if (self->canEdit())
                        self->persist(kTerminal);
                }
                return;
            }
            // The pane was closed while its row was being minted. The row and
            // its tmux session stay: closing a pane never destroys either.
            if (!leaf)
                return;
            if (!leaf->terminalPaneId.isEmpty())
                return; // already bound; nothing to publish
            leaf->terminalPaneId = pane->id.value;
            // Republished, not quiet: the pane is waiting for exactly this
            // field before it may resolve anything, and it reads it off the
            // tree. The regions re-home their panes rather than rebuild them,
            // so no live terminal is disturbed by the republish.
            self->setTree(kTerminal, self->m_regions[kTerminal].tree);
            if (self->canEdit())
                self->persist(kTerminal);
        });
}

SplitNode* SessionLayouts::findTerminalLeaf(const QString& paneId)
{
    RegionState& state = m_regions[kTerminal];
    if (!state.valid || paneId.isEmpty())
        return nullptr;
    SplitNode* leaf = nullptr;
    SplitNode* parent = nullptr;
    int childIndex = -1;
    if (!locateLeaf(state.tree, nullptr, -1, paneId, leaf, parent, childIndex))
        return nullptr;
    return leaf;
}

bool SessionLayouts::dropLeaf(int index, const QString& paneId)
{
    RegionState& state = m_regions[index];
    if (!state.valid)
        return false;
    SplitNode* leaf = nullptr;
    SplitNode* parent = nullptr;
    int childIndex = -1;
    if (!locateLeaf(state.tree, nullptr, -1, paneId, leaf, parent, childIndex))
        return false;

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
    // A closed slot may no longer resolve by its label: the permission belonged
    // to that leaf, and the next pane wearing this label is a different pane.
    if (index == kTerminal)
        m_legacyTerminalSlots.remove(paneId);
    return true;
}

void SessionLayouts::bindTerminalPaneRow(const QString& devSessionId,
                                         const QString& paneName,
                                         const QString& terminalPaneId)
{
    if (devSessionId != m_devSessionId || terminalPaneId.isEmpty())
        return;
    SplitNode* leaf = findTerminalLeaf(paneName);
    // Unknown pane (closed while the lookup travelled), or already bound. The
    // second case is what makes this happen once per leaf instead of on every
    // attach: the factory answers from its cache on a reconnect, and the leaf
    // already carries the id by then.
    if (!leaf || leaf->terminalPaneId == terminalPaneId)
        return;
    if (!leaf->terminalPaneId.isEmpty())
        return; // bound to a DIFFERENT row; a stale answer must not retarget it
    leaf->terminalPaneId = terminalPaneId;
    // This leaf has spent its one legal use of the old key.
    m_legacyTerminalSlots.remove(paneName);
    if (!canEdit())
        return;
    // Quiet: the pane is already attached to this very row, and republishing
    // the terminal tree only to record what it just did would be churn. The
    // next load reads the id off the server.
    setTreeQuietly(kTerminal, m_regions[kTerminal].tree);
    persist(kTerminal);
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

    // Deliberately layout-only. The pane's `terminal_panes` row and its remote
    // tmux session are LEFT ALONE: a closed pane's shell keeps running so the
    // user can come back to it, and the row keeps it enumerable in the Dev
    // Session's terminal pane list. Nothing here deletes either.
    if (!dropLeaf(index, paneId)) {
        emit error(QStringLiteral("SessionLayouts: no %1 pane \"%2\" to close")
                       .arg(region, paneId));
        return;
    }

    setTree(index, state.tree);
    persist(index);
}

void SessionLayouts::setPaneUrl(QString region, QString paneId, QString url)
{
    const int index = regionIndex(region);
    if (index < 0)
        return;
    RegionState& state = m_regions[index];
    if (!state.valid) {
        emit error(QStringLiteral(
            "SessionLayouts: %1 layout not loaded; pane url ignored").arg(region));
        return;
    }

    SplitNode* leaf = nullptr;
    SplitNode* parent = nullptr;
    int childIndex = -1;
    if (!locateLeaf(state.tree, nullptr, -1, paneId, leaf, parent, childIndex)) {
        emit error(QStringLiteral("SessionLayouts: no %1 pane \"%2\" to record a url for")
                       .arg(region, paneId));
        return;
    }

    // Checked BEFORE canEdit(): every pane the regions mint re-asserts the url
    // it was restored with, so an unchanged url is the normal case, not an edit.
    // Treating it as one would spend an RPC per pane on every session open, and
    // would report a spurious "no server selected" for a pane merely echoing
    // what is already stored.
    if (leaf->url == url)
        return;
    if (!canEdit())
        return;

    leaf->url = std::move(url);
    // Quiet: the pane is ALREADY showing this url. Re-publishing the tree would
    // rebuild the region's delegates and destroy the very pane that just opened
    // the file - the write would undo what it recorded.
    setTreeQuietly(index, state.tree);
    persist(index);
}

} // namespace ch
