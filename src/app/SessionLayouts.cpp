#include "SessionLayouts.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QStringView>
#include <QTimer>

#include <cmath>
#include <memory>
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

// DEPTH: none of the recursive walks below carries a bound of its own, and none
// needs one. SplitNode::kMaxDepth is the ONE nesting bound for this type and it
// is enforced at both ends - SplitNode::tryFromJson refuses a deeper tree on the
// way in, tryToJson refuses to emit one on the way out - so every SplitNode that
// reaches this file has already been through one of them, and markLegacyLeaves
// only ever sees tryToJson output. A second guard here would duplicate an
// invariant that already holds, and duplicating it is how the two get to
// disagree.

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

// Outcome of removing one leaf from a subtree.
enum class DropOutcome {
    NotFound,  // `paneId` is not in this subtree; nothing was touched
    Removed,   // the leaf went; this subtree still holds at least one child
    Emptied,   // this subtree has nothing left in it and must itself go
};

// Remove the leaf carrying `paneId` from the subtree rooted at `node`, then
// repair the shape on the way back up: a branch left with ONE child is not a
// split any more and is replaced by that child, and a branch left with NO
// children reports Emptied so its own parent drops it too.
//
// The no-children case is reachable: SplitNode::tryFromJson accepts a split
// with a single child, so a server another client also writes to can hand us
// one. Removing that child used to leave a childless branch nested inside its
// parent, which renders as a blank slot no pane can ever occupy. Collapsing
// recursively means the only place an empty node can survive is the root, and
// there it is the deliberate placeholder an emptied region keeps.
DropOutcome dropLeafFrom(SplitNode& node, const QString& paneId)
{
    if (node.isLeaf())
        return node.paneId == paneId ? DropOutcome::Emptied : DropOutcome::NotFound;
    for (int i = 0; i < node.children.size(); ++i) {
        const DropOutcome outcome = dropLeafFrom(node.children[i], paneId);
        if (outcome == DropOutcome::NotFound)
            continue;
        if (outcome == DropOutcome::Emptied) {
            node.children.removeAt(i);
            // A tree off the wire is not required to carry one ratio per child
            // (SplitNode fills the missing ones in), so this is guarded rather
            // than assumed to be in step.
            if (i < node.ratios.size())
                node.ratios.removeAt(i);
        }
        if (node.children.isEmpty())
            return DropOutcome::Emptied;
        if (node.children.size() == 1) {
            // Hoisted through a local: the survivor lives inside the very node
            // being overwritten.
            SplitNode survivor = std::move(node.children[0]);
            node = std::move(survivor);
        }
        return DropOutcome::Removed;
    }
    return DropOutcome::NotFound;
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

// Ceiling on a pane-label suffix. Far above any Dev Session a person will ever
// build, and far enough below INT_MAX that `highest + 1` and `suffix + 1` are
// always representable.
//
// It exists because neither input to the counter is ours. The tree comes off
// the wire from a server another client also writes to, and the counter itself
// is read back out of a hand-editable settings file, so "viewer-2147483647" is
// reachable without anything in this client ever minting it. Adding one to
// that is signed overflow: undefined behaviour, and in practice a wrap to
// INT_MIN, which qMax() then floors back to 1 - so the very next split hands
// out a label a pane already on screen is wearing. Saturating instead keeps
// the numbers monotonic and the arithmetic defined.
constexpr int kMaxPaneSuffix = 1000000000;

// Highest n over every "<prefix>n" leaf paneId in the tree, or 0 when none
// matches, ignoring anything above kMaxPaneSuffix. Makes the generated ids
// deterministic for a given tree.
void collectMaxPaneSuffix(const SplitNode& node, const QString& prefix,
                          int& highest)
{
    if (node.isLeaf()) {
        if (!node.paneId.startsWith(prefix))
            return;
        bool ok = false;
        const int n = QStringView(node.paneId).mid(prefix.size()).toInt(&ok);
        if (ok && n > highest && n <= kMaxPaneSuffix)
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

// Mark the leaves in `obj` (a SplitNode::tryToJson() tree) that `legacy` names, so
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

void SessionLayouts::setTerminalMintTimeoutMs(int ms)
{
    m_mintTimeoutMs = qMax(0, ms);
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
    int stored = m_uiState->nextPaneSuffix(m_devSessionId, region);
    // A counter above the ceiling cannot have come from this client - every
    // write below is bounded by kMaxPaneSuffix - so it is a corrupt or
    // hand-edited value, and the answer is the same one a MISSING counter gets:
    // fall back to the tree. Clamping it to the ceiling instead would be worse
    // than useless, because the ceiling is a fixed point: every later split
    // would mint the very same label.
    if (stored > kMaxPaneSuffix)
        stored = 1;
    // `highest` is bounded by collectMaxPaneSuffix() and `stored` by the line
    // above, so both this addition and the `suffix + 1` splitPane() stores
    // afterwards stay far inside int.
    const int next = qMin(qMax(stored, highest + 1), kMaxPaneSuffix);
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
    m_regions[index].tree = std::move(tree);
    publishTree(index);
}

void SessionLayouts::setTreeQuietly(int index, SplitNode tree)
{
    m_regions[index].tree = std::move(tree);
    publishTreeQuietly(index);
}

void SessionLayouts::publishTree(int index)
{
    publishTreeQuietly(index);
    if (index == kTerminal)
        emit terminalTreeChanged();
    else
        emit viewerTreeChanged();
}

void SessionLayouts::publishTreeQuietly(int index)
{
    RegionState& state = m_regions[index];
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
    const std::optional<QJsonObject> published = state.tree.tryToJson();
    if (!published) {
        // Not reachable while every tree that lands here comes from the parser,
        // defaultTree() or the structural edits in this file, all of which keep
        // SplitNode's invariant. If one ever stops doing so, the region reads as
        // EMPTY and says why - the same verdict a tree the parser rejects gets -
        // rather than QML being handed a layout it cannot render.
        state.valid = false;
        state.cache = QVariant();
        emit error(QStringLiteral("SessionLayouts: %1 tree is not a valid split "
                                  "tree; not shown")
                       .arg(regionKey(index)));
        return;
    }
    QJsonObject obj = *published;
    // Plus, for the terminal region only, the one field that is published but
    // never persisted: which id-less leaves are pre-migration leaves the pane
    // may resolve by label. See m_legacyTerminalSlots.
    if (index == kTerminal && !m_legacyTerminalSlots.isEmpty())
        markLegacyLeaves(obj, m_legacyTerminalSlots);
    state.cache = obj.toVariantMap();
}

void SessionLayouts::clearTrees()
{
    // The set names slots of the CURRENT Dev Session's terminal layout. Carrying
    // it into another session would grant a same-named slot there permission to
    // resolve by label, which is the one thing that must be earned per leaf.
    m_legacyTerminalSlots.clear();
    for (int index = 0; index < kRegionCount; ++index) {
        RegionState& state = m_regions[index];
        const bool wasValid = state.valid;
        state = RegionState{};
        if (!wasValid)
            continue; // already null; no redundant signal
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
    emit generationChanged();
    m_devSessionId = std::move(devSessionId);
    m_pendingLoads = 0;
    if (sessionChanged) {
        emit devSessionIdChanged();
        // The previous session's panes must not linger while the new layout is
        // in flight, and must not be edited into the new session either.
        clearTrees();
    }
    if (m_devSessionId.isEmpty()) {
        clearPendingWrites();
        return; // deselection: nothing to fetch
    }

    m_pendingLoads = kRegionCount;
    // A deliberate reload adopts whatever the server has, so no earlier edit
    // may veto it; only an edit made from HERE ON supersedes these replies.
    for (RegionState& state : m_regions) {
        state.loading = true;
        state.pendingWrites.clear();
        state.superseded = false;
    }
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
    // Surface transport failures even when the request belongs to a session
    // already abandoned, but do not touch the CURRENT slot until the stamp is
    // known to be current. A late old-session reply used to clear `loading`
    // here; the next gesture then saw a null tree as unloadable instead of
    // queueing behind the new session's still-pending load.
    if (err)
        emit error(err->message);
    if (generation != m_generation)
        return; // a newer load() has taken over; drop this reply entirely

    RegionState& state = m_regions[index];
    state.loading = false;
    if (err)
        state.pendingWrites.clear();
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
    // A current-session gesture that arrived while this region was null is
    // replayed only after the authoritative tree (or the seeded default) is in
    // place. Any pane/path that disappeared is silently stale and is dropped.
    if (!err)
        flushPendingWrites(index);

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

SessionLayouts::WriteDecision SessionLayouts::prepareWrite(
    int index, const QString& devSessionId, quint64 generation)
{
    // Check the stamp BEFORE canEdit() or any tree lookup. A delayed signal from
    // a session the user left is not a current misuse and must not produce the
    // "layout not loaded" toast; more importantly, it must never inspect a
    // same-named pane in the new session.
    if (devSessionId != m_devSessionId || generation != m_generation)
        return WriteDecision::Drop;
    if (!canEdit())
        return WriteDecision::Reject;
    if (!m_regions[index].valid && m_regions[index].loading)
        return WriteDecision::Queue;
    return WriteDecision::Apply;
}

void SessionLayouts::queueWrite(int index, PendingWrite write)
{
    RegionState& state = m_regions[index];
    // URL, title and ratio reports are continuous gestures. Keeping only the
    // report for one target prevents a fast switch from replaying every
    // intermediate mouse position when the load resolves. Structural actions
    // remain ordered because each click is a separate user intent.
    if (write.kind == PendingWriteKind::PaneUrl
        || write.kind == PendingWriteKind::PaneTitle
        || write.kind == PendingWriteKind::Ratios) {
        for (auto it = state.pendingWrites.rbegin();
             it != state.pendingWrites.rend(); ++it) {
            const bool sameTarget =
                ((write.kind == PendingWriteKind::PaneUrl
                  || write.kind == PendingWriteKind::PaneTitle)
                 && it->kind == write.kind && it->paneId == write.paneId)
                || (write.kind == PendingWriteKind::Ratios
                    && it->kind == write.kind
                    && it->pathIndexes == write.pathIndexes);
            if (sameTarget) {
                *it = std::move(write);
                return;
            }
        }
    }
    state.pendingWrites.push_back(std::move(write));
}

void SessionLayouts::clearPendingWrites()
{
    for (RegionState& state : m_regions)
        state.pendingWrites.clear();
}

void SessionLayouts::flushPendingWrites(int index)
{
    RegionState& state = m_regions[index];
    if (state.pendingWrites.isEmpty())
        return;
    QVector<PendingWrite> pending = std::move(state.pendingWrites);
    state.pendingWrites.clear();
    for (const PendingWrite& write : pending) {
        switch (write.kind) {
        case PendingWriteKind::Ratios:
            setRatiosStamped(m_devSessionId, m_generation, regionKey(index),
                             write.pathIndexes, write.ratios, true);
            break;
        case PendingWriteKind::Split:
            // A queued split cannot return its new pane id synchronously. The
            // UI has no pane to focus until the tree is published, so replay it
            // as a fire-and-forget intent and let the normal tree signal select
            // the resulting pane.
            splitPaneStamped(m_devSessionId, m_generation, regionKey(index),
                             write.paneId, write.orientation, true);
            break;
        case PendingWriteKind::Close:
            closePaneStamped(m_devSessionId, m_generation, regionKey(index),
                             write.paneId, true);
            break;
        case PendingWriteKind::PaneUrl:
            setPaneUrlStamped(m_devSessionId, m_generation, regionKey(index),
                              write.paneId, write.url, true);
            break;
        case PendingWriteKind::PaneTitle:
            setPaneTitleStamped(m_devSessionId, m_generation,
                                regionKey(index), write.paneId, write.title,
                                true);
            break;
        }
    }
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

void SessionLayouts::saveTreeForSession(QString devSessionId,
                                        quint64 generation, QString region,
                                        QVariant tree)
{
    saveTreeStamped(devSessionId, generation, region, tree);
}

void SessionLayouts::saveTree(QString region, QVariant tree)
{
    // Compatibility wrapper for C++ tests; production QML uses the stamped
    // entry point so a delayed signal cannot be mistaken for current intent.
    saveTreeStamped(m_devSessionId, m_generation, region, tree);
}

void SessionLayouts::saveTreeStamped(const QString& devSessionId,
                                     quint64 generation, const QString& region,
                                     const QVariant& tree)
{
    const int index = regionIndex(region);
    if (index < 0)
        return;
    if (devSessionId != m_devSessionId || generation != m_generation)
        return; // stale authored tree; never inspect the current session
    if (!canEdit())
        return;
    std::optional<SplitNode> parsed = parseVariantTree(tree);
    if (!parsed) {
        emit error(QStringLiteral(
            "SessionLayouts: %1 tree is not a valid split tree; not saved")
                       .arg(region));
        return;
    }
    // A full authored tree is itself authoritative user intent, even while
    // getLayout is outstanding. It replaces the null/fallback state now and
    // retires that reply rather than waiting and risking a revert.
    setTreeQuietly(index, std::move(*parsed));
    persist(index);
}

void SessionLayouts::setRatiosForSession(QString devSessionId,
                                         quint64 generation, QString region,
                                         QStringList pathIndexes,
                                         QVariantList ratios)
{
    setRatiosStamped(devSessionId, generation, region, pathIndexes, ratios, false);
}
QString SessionLayouts::setPaneTitleForSession(QString devSessionId,
                                               quint64 generation,
                                               QString region, QString paneId,
                                               QString title)
{
    return setPaneTitleStamped(devSessionId, generation, region, paneId, title,
                               false);
}


void SessionLayouts::setRatios(QString region, QStringList pathIndexes,
                               QVariantList ratios)
{
    // Compatibility wrapper for C++ tests; production QML uses the stamped
    // entry point so a delayed drag cannot be mistaken for current intent.
    setRatiosStamped(m_devSessionId, m_generation, region, pathIndexes, ratios,
                     false);
}

void SessionLayouts::setRatiosStamped(const QString& devSessionId,
                                      quint64 generation, const QString& region,
                                      const QStringList& pathIndexes,
                                      const QVariantList& ratios, bool replay)
{
    const int index = regionIndex(region);
    if (index < 0)
        return;
    const WriteDecision decision =
        prepareWrite(index, devSessionId, generation);
    if (decision == WriteDecision::Drop || decision == WriteDecision::Reject)
        return;
    if (decision == WriteDecision::Queue) {
        // The branch count is unknown until the server tree arrives, but the
        // values themselves are still user input and must be checked now.
        for (const QVariant& value : ratios) {
            bool ok = false;
            const double ratio = value.toDouble(&ok);
            if (!ok || !std::isfinite(ratio) || ratio <= 0.0) {
                emit error(QStringLiteral(
                    "SessionLayouts: invalid %1 ratio \"%2\"")
                               .arg(region, value.toString()));
                return;
            }
        }
        PendingWrite write;
        write.kind = PendingWriteKind::Ratios;
        write.pathIndexes = pathIndexes;
        write.ratios = ratios;
        queueWrite(index, std::move(write));
        return;
    }

    RegionState& state = m_regions[index];
    SplitNode* node = &state.tree;
    for (const QString& step : pathIndexes) {
        bool ok = false;
        const int childIndex = step.toInt(&ok);
        if (!ok || node->isLeaf() || childIndex < 0
            || childIndex >= node->children.size()) {
            if (!replay)
                emit error(QStringLiteral(
                    "SessionLayouts: no %1 node at path [%2]")
                               .arg(region, pathIndexes.join(QLatin1Char(','))));
            return;
        }
        node = &node->children[childIndex];
    }
    if (node->isLeaf()) {
        if (!replay)
            emit error(QStringLiteral(
                "SessionLayouts: %1 node at path [%2] is a leaf and has no ratios")
                           .arg(region, pathIndexes.join(QLatin1Char(','))));
        return;
    }
    if (ratios.size() != node->children.size()) {
        if (!replay)
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
            if (!replay)
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
    publishTreeQuietly(index);
    persist(index);
}

QString SessionLayouts::splitPaneForSession(QString devSessionId,
                                            quint64 generation, QString region,
                                            QString paneId, QString orientation)
{
    return splitPaneStamped(devSessionId, generation, region, paneId,
                            orientation, false);
}

QString SessionLayouts::splitPane(QString region, QString paneId,
                                  QString orientation)
{
    // Compatibility wrapper for C++ tests; production QML uses the stamped
    // entry point so a delayed command cannot be mistaken for current intent.
    return splitPaneStamped(m_devSessionId, m_generation, region, paneId,
                            orientation, false);
}

QString SessionLayouts::splitPaneStamped(const QString& devSessionId,
                                         quint64 generation,
                                         const QString& region,
                                         const QString& paneId,
                                         const QString& orientation, bool replay)
{
    const int index = regionIndex(region);
    if (index < 0)
        return {};
    const WriteDecision decision =
        prepareWrite(index, devSessionId, generation);
    if (decision == WriteDecision::Drop || decision == WriteDecision::Reject)
        return {};
    const bool vertical = orientation == QStringLiteral("vertical");
    if (!vertical && orientation != QStringLiteral("horizontal")) {
        if (!replay)
            emit error(QStringLiteral(
                "SessionLayouts: unknown orientation \"%1\"").arg(orientation));
        return {};
    }
    if (decision == WriteDecision::Queue) {
        PendingWrite write;
        write.kind = PendingWriteKind::Split;
        write.paneId = paneId;
        write.orientation = orientation;
        queueWrite(index, std::move(write));
        return {};
    }

    RegionState& state = m_regions[index];
    SplitNode* leaf = nullptr;
    SplitNode* parent = nullptr;
    int childIndex = -1;
    if (!locateLeaf(state.tree, nullptr, -1, paneId, leaf, parent, childIndex)) {
        if (!replay)
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
    //
    // reservePaneSuffix() bounds `suffix` by kMaxPaneSuffix, so this addition
    // is always representable; qMin keeps what is STORED in the same range, so
    // the next read never has to treat our own counter as corrupt.
    m_uiState->setNextPaneSuffix(m_devSessionId, regionKey(index),
                                 qMin(suffix + 1, kMaxPaneSuffix));

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
    publishTree(index);
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

// Undo a terminal leaf whose row could not be minted, and say why. Shared by
// the failed-answer path and the no-answer-at-all path so both leave exactly
// the same state: no half-made pane, no id-less leaf on the server, and - the
// point of the exercise - no PENDING leaf, so persist() may write the region
// again.
void SessionLayouts::abandonTerminalMint(const QString& paneId,
                                         const QString& reason)
{
    emit error(reason);
    if (!dropLeaf(kTerminal, paneId))
        return;
    publishTree(kTerminal);
    if (canEdit())
        persist(kTerminal);
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
    // One mint, one outcome. Whichever of the answer and the deadline arrives
    // first flips this and the other becomes a no-op; the flag is shared rather
    // than a QTimer this object owns so a mint for a Dev Session that is long
    // gone cannot be resurrected by a stray stop().
    auto settled = std::make_shared<bool>(false);
    m_db->createTerminalPane(
        params, [self, generation, paneId, devSessionId, settled](
                    std::optional<TerminalPane> pane, std::optional<RpcError> err) {
            if (!self || *settled)
                return;
            *settled = true;
            // A load has taken over since. Its tree is authoritative and this
            // leaf may not even be in it; the row itself stays on the server,
            // enumerable through the Dev Session's terminal pane list, rather
            // than being deleted from under a shell that may already be running.
            if (self->m_generation != generation || self->m_devSessionId != devSessionId)
                return;

            SplitNode* leaf = self->findTerminalLeaf(paneId);
            if (err || !pane || pane->id.value.isEmpty()) {
                // Take the half-made pane back out rather than leave one that
                // can never attach and can never be told apart from a
                // pre-migration leaf if anything later persisted it.
                self->abandonTerminalMint(
                    paneId,
                    err ? err->message
                        : QStringLiteral("the server created this terminal without an id"));
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
            self->publishTree(kTerminal);
            if (self->canEdit())
                self->persist(kTerminal);
        });
    if (m_mintTimeoutMs <= 0)
        return; // opted out; the transport is then the only backstop
    // The deadline. persist() declines to write the terminal region while ANY
    // leaf is still waiting for its row, which is load-bearing (see there) but
    // has no lower bound of its own: a request the peer simply never answers
    // leaves the region unwritable for the rest of the session, so every later
    // split, close, ratio drag and pane url is silently dropped. The transport
    // does fail pending requests when it notices the peer has gone quiet, and
    // that stays the primary and better-worded mechanism - this fires well
    // after it (see kDefaultTerminalMintTimeoutMs) and only covers the case it
    // cannot see: a healthy connection on which this one request was lost.
    QTimer::singleShot(m_mintTimeoutMs, this,
                       [self, generation, paneId, devSessionId, settled] {
                           if (!self || *settled)
                               return;
                           *settled = true;
                           if (self->m_generation != generation
                               || self->m_devSessionId != devSessionId)
                               return;
                           self->abandonTerminalMint(
                               paneId,
                               QStringLiteral(
                                   "SessionLayouts: the server never answered the "
                                   "request to create terminal \"%1\" within %2 s; "
                                   "the pane has been removed")
                                   .arg(paneId)
                                   .arg(self->m_mintTimeoutMs / 1000));
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
    const DropOutcome outcome = dropLeafFrom(state.tree, paneId);
    if (outcome == DropOutcome::NotFound)
        return false;
    if (outcome == DropOutcome::Emptied) {
        // Nothing is left anywhere in the region: a region always has a tree,
        // so leave a single EMPTY placeholder leaf rather than an empty (and
        // unrenderable) one. Reached both when the closed pane WAS the root and
        // when collapsing the parent chain consumed every branch above it.
        state.tree = SplitNode{};
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
    publishTreeQuietly(kTerminal);
    persist(kTerminal);
}

void SessionLayouts::closePaneForSession(QString devSessionId,
                                          quint64 generation, QString region,
                                          QString paneId)
{
    closePaneStamped(devSessionId, generation, region, paneId, false);
}

void SessionLayouts::closePane(QString region, QString paneId)
{
    // Compatibility wrapper for C++ tests; production QML uses the stamped
    // entry point so a delayed close cannot be mistaken for current intent.
    closePaneStamped(m_devSessionId, m_generation, region, paneId, false);
}

void SessionLayouts::closePaneStamped(const QString& devSessionId,
                                      quint64 generation, const QString& region,
                                      const QString& paneId, bool replay)
{
    const int index = regionIndex(region);
    if (index < 0)
        return;
    const WriteDecision decision =
        prepareWrite(index, devSessionId, generation);
    if (decision == WriteDecision::Drop || decision == WriteDecision::Reject)
        return;
    if (decision == WriteDecision::Queue) {
        PendingWrite write;
        write.kind = PendingWriteKind::Close;
        write.paneId = paneId;
        queueWrite(index, std::move(write));
        return;
    }
    RegionState& state = m_regions[index];
    if (!state.valid) {
        if (!replay)
            emit error(QStringLiteral(
                "SessionLayouts: %1 layout not loaded; close ignored").arg(region));
        return;
    }

    // Deliberately layout-only. The pane's `terminal_panes` row and its remote
    // tmux session are LEFT ALONE: a closed pane's shell keeps running so the
    // user can come back to it, and the row keeps it enumerable in the Dev
    // Session's terminal pane list. Nothing here deletes either.
    if (!dropLeaf(index, paneId)) {
        if (!replay)
            emit error(QStringLiteral("SessionLayouts: no %1 pane \"%2\" to close")
                           .arg(region, paneId));
        return;
    }

    publishTree(index);
    persist(index);
}

void SessionLayouts::setPaneUrlForSession(QString devSessionId,
                                           quint64 generation, QString region,
                                           QString paneId, QString url)
{
    setPaneUrlStamped(devSessionId, generation, region, paneId, url, false);
}

void SessionLayouts::setPaneUrl(QString region, QString paneId, QString url)
{
    // Compatibility wrapper for C++ tests; production QML uses the stamped
    // entry point so a delayed pane event cannot be mistaken for current intent.
    setPaneUrlStamped(m_devSessionId, m_generation, region, paneId, url, false);
}

void SessionLayouts::setPaneUrlStamped(const QString& devSessionId,
                                       quint64 generation, const QString& region,
                                       const QString& paneId, const QString& url,
                                       bool replay)
{
    const int index = regionIndex(region);
    if (index < 0)
        return;
    const WriteDecision decision =
        prepareWrite(index, devSessionId, generation);
    if (decision == WriteDecision::Drop || decision == WriteDecision::Reject)
        return;
    if (decision == WriteDecision::Queue) {
        PendingWrite write;
        write.kind = PendingWriteKind::PaneUrl;
        write.paneId = paneId;
        write.url = url;
        queueWrite(index, std::move(write));
        return;
    }
    RegionState& state = m_regions[index];
    if (!state.valid) {
        if (!replay)
            emit error(QStringLiteral(
                "SessionLayouts: %1 layout not loaded; pane url ignored").arg(region));
        return;
    }

    SplitNode* leaf = nullptr;
    SplitNode* parent = nullptr;
    int childIndex = -1;
    if (!locateLeaf(state.tree, nullptr, -1, paneId, leaf, parent, childIndex)) {
        if (!replay)
            emit error(QStringLiteral(
                "SessionLayouts: no %1 pane \"%2\" to record a url for")
                           .arg(region, paneId));
        return;
    }

    // Checked BEFORE any persistence: every pane the regions mint re-asserts
    // the url it was restored with, so an unchanged url is the normal case, not
    // an edit. Treating it as one would spend an RPC per pane on every open.
    if (leaf->url == url)
        return;
    leaf->url = url;
    // Quiet: the pane is ALREADY showing this url. Re-publishing the tree would
    // rebuild the region's delegates and destroy the very pane that just opened
    // the file - the write would undo what it recorded.
    publishTreeQuietly(index);
    persist(index);
}

QString SessionLayouts::setPaneTitleStamped(const QString& devSessionId,
                                            quint64 generation,
                                            const QString& region,
                                            const QString& paneId,
                                            const QString& title,
                                            bool replay)
{
    const int index = regionIndex(region);
    if (index < 0)
        return {};
    const WriteDecision decision =
        prepareWrite(index, devSessionId, generation);
    if (decision == WriteDecision::Drop || decision == WriteDecision::Reject)
        return {};
    const QString normalized = SplitNode::normalizeCustomTitle(title);
    if (decision == WriteDecision::Queue) {
        PendingWrite write;
        write.kind = PendingWriteKind::PaneTitle;
        write.paneId = paneId;
        write.title = normalized;
        queueWrite(index, std::move(write));
        return normalized;
    }

    RegionState& state = m_regions[index];
    if (!state.valid) {
        if (!replay)
            emit error(QStringLiteral(
                "SessionLayouts: %1 layout not loaded; pane title ignored")
                           .arg(region));
        return {};
    }

    SplitNode* leaf = nullptr;
    SplitNode* parent = nullptr;
    int childIndex = -1;
    if (!locateLeaf(state.tree, nullptr, -1, paneId, leaf, parent, childIndex)) {
        if (!replay)
            emit error(QStringLiteral(
                "SessionLayouts: no %1 pane \"%2\" to set a title for")
                           .arg(region, paneId));
        return {};
    }
    if (leaf->customTitle == normalized)
        return normalized;

    // Only the display field changes. The layout slot and server-minted row id
    // stay untouched so a rename can never retarget a running terminal.
    leaf->customTitle = normalized;
    publishTreeQuietly(index);
    persist(index);
    return normalized;
}

QString SessionLayouts::setPaneTitle(QString region, QString paneId, QString title)
{
    // Compatibility wrapper for C++ tests; production QML uses the stamped
    // entry point so a delayed pane event cannot be mistaken for current intent.
    return setPaneTitleStamped(m_devSessionId, m_generation, region, paneId, title,
                               false);
}

} // namespace ch
