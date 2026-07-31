#pragma once

#include "SplitTree.h"
#include "UiStateStore.h"
#include "WorkspaceDb.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include <optional>

namespace ch {

// QML-facing bridge between the persisted per-region split layouts
// (workspace.getLayout / workspace.setLayout, SPEC 4.5) and the recursive
// ViewerRegion/TerminalRegion QML trees. Selecting a Dev Session calls load(),
// which fetches BOTH regions; every structural edit (splitPane/closePane), every
// ratio drag (setRatios) and every QML-authored tree (saveTree) is written back
// through WorkspaceDb. No layout state is kept client-side beyond this cache:
// the codeharbord database stays authoritative (SPEC 2.1).
//
// Tree shape: `viewerTree` / `terminalTree` are plain QVariantMaps produced by
// SplitNode::toJson().toVariantMap(), i.e. EXACTLY the persisted wire shape
//     leaf   { "type": "leaf",  "paneId": "viewer-1" }
//     branch { "type": "split", "orientation": "horizontal"|"vertical",
//              "children": [ ... ], "ratios": [ 1, 1 ] }
// so QML consumes them directly as `node`. Every tree published to QML has been
// through SplitNode::tryFromJson, so a malformed persisted or QML-authored tree
// can never reach the regions: it is rejected with error() instead.
//
// Both trees read as a NULL QVariant until a load resolves (and again while a
// load for a different Dev Session is in flight). That is deliberate: the QML
// regions are inert for a null `node`, so no throwaway pane is ever built for a
// session whose real layout is still on the wire. A region whose getLayout
// FAILED also stays null rather than falling back to a default leaf — showing a
// fabricated single-pane layout would invite the user to edit it and overwrite
// their real one.
//
// A region the server has NO row for is a different case from a failed one: the
// region default (see defaultTree()) is adopted AND written back, so a brand
// new Dev Session's layout - two stacked terminal panes and one viewer pane -
// is in the database from the first time the session is opened rather than
// re-derived on every load. That write is idempotent: it creates the row, so
// the next load reads it and seeds nothing.
//
// Signal discipline (matters: the QML regions rebuild their delegates, and
// therefore destroy and recreate live panes, whenever the tree object changes):
//   * load(), splitPane() and closePane() DO emit viewerTreeChanged /
//     terminalTreeChanged - the structure genuinely changed.
//   * setRatios(), setPaneUrl() and saveTree() do NOT. Their input already came
//     from QML, so re-publishing an identical tree would only churn panes (a
//     terminal would be killed and respawned on every splitter drag, and the
//     pane that just opened a file would be destroyed by the very write that
//     recorded it).
class SessionLayouts : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString devSessionId READ devSessionId WRITE setDevSessionId
                   NOTIFY devSessionIdChanged)
    // Needed for setLayout's serverId argument; also exposed as a property so
    // QML can bind it to AppController::serverId.
    Q_PROPERTY(QString serverId READ serverId WRITE setServerId
                   NOTIFY serverIdChanged)
    Q_PROPERTY(QVariant viewerTree READ viewerTree NOTIFY viewerTreeChanged)
    Q_PROPERTY(QVariant terminalTree READ terminalTree NOTIFY terminalTreeChanged)

public:
    // `db` and `uiState` are both borrowed (owned by AppController/main.cpp) and
    // must outlive this object. `uiState` holds the persisted per-(Dev Session,
    // region) pane-suffix counter splitPane() mints from.
    explicit SessionLayouts(WorkspaceDb* db, UiStateStore* uiState,
                            QObject* parent = nullptr);

    QString devSessionId() const { return m_devSessionId; }
    // Equivalent to load(id), skipped when the id is unchanged. Lets QML bind
    // the selected session directly.
    void setDevSessionId(QString devSessionId);

    QString serverId() const { return m_serverId; }
    // Changing the server also DESELECTS the current Dev Session (dropping both
    // region trees): a Dev Session belongs to exactly one server, so keeping it
    // loaded would let the next edit persist the old session's tree under the
    // new server's key.
    void setServerId(QString serverId);

    QVariant viewerTree() const;
    QVariant terminalTree() const;

    // Fetch both regions for `devSessionId`. Switching sessions clears both
    // trees to null immediately. An empty id clears and issues no request.
    //
    // Concurrent/repeated calls are safe: each load is stamped with a monotonic
    // generation and only the newest one may touch the trees, so a late reply
    // for an abandoned Dev Session is dropped (errors are still surfaced
    // verbatim, superseded or not - the same rule AppController::refresh uses).
    Q_INVOKABLE void load(QString devSessionId);

    // Replace a region's tree with a QML-authored one and persist it. `tree`
    // must be SplitNode::toJson()-shaped (see above); anything else is rejected
    // via error() and changes nothing.
    Q_INVOKABLE void saveTree(QString region, QVariant tree);

    // Persist drag-adjusted ratios for ONE branch node, addressed by an index
    // path from the root: an empty list is the root, ["1"] its second child,
    // ["1","0"] that child's first child. `ratios` must hold exactly one finite,
    // strictly positive value per child of the addressed node.
    Q_INVOKABLE void setRatios(QString region, QStringList pathIndexes,
                               QVariantList ratios);

    // Split the leaf holding `paneId` into a branch of the given orientation
    // ("horizontal" | "vertical") with the original pane first and a new,
    // equally sized pane second. Returns the new paneId ("<region>-<n>", n taken
    // from the never-decreasing per-(Dev Session, region) counter in
    // UiStateStore, so an id is never minted twice for one Dev Session), or an
    // empty string when nothing was changed.
    //
    // Passing the empty paneId of the placeholder leaf an emptied region is
    // left with FILLS it in place instead of splitting it: that placeholder is
    // not a pane, and it is the only handle QML has on a region with no panes.
    Q_INVOKABLE QString splitPane(QString region, QString paneId,
                                  QString orientation);

    // Remove the leaf holding `paneId`, collapsing a branch left with a single
    // child into that child. Closing the last pane of a region leaves a single
    // EMPTY leaf (paneId ""), never an empty tree.
    Q_INVOKABLE void closePane(QString region, QString paneId);

    // Record what the leaf holding `paneId` currently has open, so reopening
    // the Dev Session restores the pane's CONTENT and not just its geometry.
    // The url rides in the split-tree leaf and is persisted by the same
    // workspace.setLayout write the ratios use (see SplitNode::url).
    //
    // Deliberately silent when the url is unchanged: the regions re-assert a
    // restored url onto every pane they mint, so the common call is an echo of
    // what is already stored and must cost neither an RPC nor an error.
    Q_INVOKABLE void setPaneUrl(QString region, QString paneId, QString url);

signals:
    void devSessionIdChanged();
    void serverIdChanged();
    void viewerTreeChanged();
    void terminalTreeChanged();
    // Server-forwarded RPC message, verbatim (SPEC 10.3), or a locally detected
    // misuse (unknown region, unknown paneId, malformed tree/ratios).
    void error(QString message);
    // Both regions of the load for this Dev Session have resolved. Fires once
    // per load that actually issued requests, including when a region errored
    // (error() reported that separately), so a consumer waiting on it can never
    // hang. A load with an EMPTY id is a deselection: it fetches nothing and
    // therefore reports nothing.
    void loaded(QString devSessionId);

private:
    // Region slots, indexed by kViewer/kTerminal.
    static constexpr int kViewer = 0;
    static constexpr int kTerminal = 1;
    static constexpr int kRegionCount = 2;

    struct RegionState {
        SplitNode tree;      // meaningful only while `valid`
        bool valid = false;  // false -> the property reads as a null QVariant
        QVariant cache;      // tree.toJson().toVariantMap(), kept in lockstep
        // A local edit has already replaced this region's tree since the
        // in-flight load for it was issued, so that load's reply is history and
        // must not be applied. Without it, a getLayout answer that crossed a
        // saveTree/splitPane on the wire would silently revert the user's edit -
        // or, when the server had no row yet, overwrite it with the region
        // DEFAULT and persist that, destroying the edit on the server too.
        // Cleared by load() (a deliberate reload must adopt the server's tree)
        // and by clearTrees().
        bool superseded = false;
    };

    // "viewer"/"terminal" -> kViewer/kTerminal; -1 (after emitting error) for
    // anything else.
    int regionIndex(const QString& region);
    static Region regionEnum(int index);
    // The layout a region starts with when the server has no row for it.
    //
    // Viewer: one leaf, "viewer-1".
    // Terminal: TWO leaves, "terminal-1" above "terminal-2", evenly split — a
    // "vertical" branch stacks its children top to bottom (SplitTree.cpp, and
    // TerminalRegion.qml maps "vertical" to Qt.Vertical). A single terminal was
    // never a considered default, only the smallest thing that rendered: real
    // work on a remote box is one shell running something and a second to look
    // at it, and reaching the second one meant finding "Split Terminal Pane" in
    // the command palette.
    //
    // The pane ids stay "<region>-<n>" with n starting at 1, so the next id
    // splitPane() hands out is "terminal-3" (reservePaneSuffix() sees both
    // default panes), and a Dev Session created before this change keeps the
    // panes it already has (its layout row exists and is loaded verbatim).
    static SplitNode defaultTree(int index);

    void applyLoadedTree(quint64 generation, int index,
                         std::optional<SplitNode> tree,
                         std::optional<RpcError> err);
    // Store `tree` in the slot, reserve the pane suffixes it carries, refresh
    // the variant cache, and emit the region's changed signal.
    void setTree(int index, SplitNode tree);
    // Store without emitting - for edits QML already applied (see the signal
    // discipline note above).
    void setTreeQuietly(int index, SplitNode tree);
    void clearTrees();
    // Layout edits are only meaningful for a selected Dev Session on a known
    // server: setLayout needs both ids, and mutating the cache without being
    // able to persist would silently diverge from the authoritative tree. Emits
    // error() and returns false when either is missing.
    bool canEdit();
    // Write the slot's current tree back with workspace.setLayout. Only valid
    // after canEdit() returned true.
    void persist(int index);
    // "viewer" / "terminal" for the region slot - both the pane-id prefix (with
    // a "-" appended) and the per-region key half of the persisted counter.
    static QString regionKey(int index);
    // Burn every "<region>-<n>" suffix `tree` carries so none of them can ever
    // be minted again for this Dev Session, and answer the next free one.
    //
    // Returns max(stored counter, highest suffix in `tree` + 1) and stores that
    // when it moved. Consulting the TREE as well as the counter is what makes an
    // existing Dev Session safe: one created before the counter existed, or one
    // whose settings file was cleared, reads the default 1 and would otherwise
    // mint an id its own layout is already showing. Consulting the COUNTER as
    // well as the tree is what fixes the recycling bug: a closed pane is gone
    // from the tree but its suffix stays burnt, so splitting again cannot re-mint
    // the id that names its remote tmux session (see UiStateStore's comment) and
    // silently re-attach the closed shell.
    int reservePaneSuffix(int index, const SplitNode& tree);

    WorkspaceDb* m_db = nullptr;
    UiStateStore* m_uiState = nullptr;
    QString m_devSessionId;
    QString m_serverId;
    RegionState m_regions[kRegionCount];
    // Monotonic stamp so a reply from a superseded load() can never overwrite a
    // newer session's tree; see load().
    quint64 m_generation = 0;
    // getLayout replies still outstanding for the current generation; `loaded`
    // fires when it reaches zero.
    int m_pendingLoads = 0;
};

} // namespace ch
