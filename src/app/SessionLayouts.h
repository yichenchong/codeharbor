#pragma once

#include "SplitTree.h"
#include "UiStateStore.h"
#include "WorkspaceDb.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVector>

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
// SplitNode::tryToJson().toVariantMap(), i.e. EXACTLY the persisted wire shape
//     leaf   { "type": "leaf",  "paneId": "viewer-1" }
//     branch { "type": "split", "orientation": "horizontal"|"vertical",
//              "children": [ ... ], "ratios": [ 1, 1 ] }
// so QML consumes them directly as `node`. Every tree published to QML has been
// through SplitNode::tryFromJson, so a malformed persisted or QML-authored tree
// can never reach the regions: it is rejected with error() instead.
//
// A terminal leaf carries one more persisted field, "terminalPaneId": the
// server's row id for that pane's terminal (SplitNode::terminalPaneId), which
// is what TerminalPaneView resolves its tmux target from.
//
// The published terminal tree carries ONE field that is NOT persisted and never
// reaches the server: "terminalLegacy": true on a leaf whose row id is missing
// because the layout predates the field. It is the pane's permission to resolve
// by its slot label, the old and recyclable key, exactly once. A leaf with
// neither an id nor that marker is waiting for a row to be minted and must
// attach nothing meanwhile. SplitNode::tryFromJson drops the marker like any
// unknown key, so a tree that comes back through saveTree() cannot persist it.
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
    // The stamp travels with every QML-originated layout edit. It is separate
    // from the Dev Session id because a deliberate reload of the SAME session
    // also makes older queued gestures history.
    Q_PROPERTY(quint64 generation READ generation NOTIFY generationChanged)
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

    quint64 generation() const { return m_generation; }

    QVariant viewerTree() const;
    QVariant terminalTree() const;
    // Fetch both regions for `devSessionId`. Switching sessions clears both
    // trees to null immediately. An empty id clears and issues no request.
    //
    // Concurrent/repeated calls are safe: each load is stamped with a monotonic
    // generation and only the newest one may touch the trees, so a late reply
    // for an abandoned Dev Session is dropped (errors are still surfaced).
    Q_INVOKABLE void load(QString devSessionId);

    // Production QML MUST use the stamped entry points below. The session id
    // and generation are captured when a gesture starts, so a reply or signal
    // from a Dev Session the user has already left is dropped before it can
    // inspect or mutate the current tree. The short methods further below stay
    // only for C++ tests and old standalone callers that have no QML stamp.
    Q_INVOKABLE void saveTreeForSession(QString devSessionId, quint64 generation,
                                        QString region, QVariant tree);
    Q_INVOKABLE void setRatiosForSession(QString devSessionId, quint64 generation,
                                         QString region, QStringList pathIndexes,
                                         QVariantList ratios);
    Q_INVOKABLE QString splitPaneForSession(QString devSessionId,
                                             quint64 generation, QString region,
                                             QString paneId, QString orientation);
    Q_INVOKABLE void closePaneForSession(QString devSessionId, quint64 generation,
                                         QString region, QString paneId);
    Q_INVOKABLE void setPaneUrlForSession(QString devSessionId,
                                          quint64 generation, QString region,
                                          QString paneId, QString url);
    Q_INVOKABLE QString setPaneTitleForSession(QString devSessionId,
                                               quint64 generation,
                                               QString region, QString paneId,
                                               QString title);

    // Unstamped compatibility entry point. Production callers must not use
    // this: it intentionally stamps the call with whatever session is current
    // at invocation and therefore cannot prove where a delayed signal came
    // from. Existing C++ tests use it to exercise the core tree operations.
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
    // UiStateStore, so a LABEL is never shown twice for one Dev Session), or an
    // empty string when nothing was changed.
    //
    // Passing the empty paneId of the placeholder leaf an emptied region is
    // left with FILLS it in place instead of splitting it: that placeholder is
    // not a pane, and it is the only handle QML has on a region with no panes.
    //
    // TERMINAL REGION: the new leaf also needs a `terminal_panes` row of its
    // own, and that is a server round trip. It is NOT waited for. The leaf is
    // published (and persisted) immediately with no terminalPaneId so the split
    // is instant, the row is minted in the background, and its id is written
    // into the leaf when it lands. Until then the leaf is PENDING and the pane
    // attaches nothing at all: it is published to QML without the
    // `terminalLegacy` marker below, which is precisely the state that means
    // "wait, your identity is coming". It deliberately does NOT fall back to
    // resolving by its slot label, because that is how a new pane used to end
    // up attached to a closed pane's still-running shell.
    //
    // A mint that FAILS - the server refuses it, answers without an id, or
    // never answers at all within terminalMintTimeoutMs() - reports through
    // error() and REMOVES the leaf again, so the user sees the split undone
    // with a reason rather than a pane that can never come up; so an id-less
    // terminal leaf, which a later load would have to read as a legacy one, is
    // not left behind on the server; and so the region becomes writable again
    // (persist() refuses to write a tree with a pending leaf in it).
    Q_INVOKABLE QString splitPane(QString region, QString paneId,
                                  QString orientation);

    // Remove the leaf holding `paneId`, then repair the shape of every branch
    // above it: one left with a single child is replaced by that child, and one
    // left with NO children is dropped from its own parent in turn. That second
    // case is reachable because a split with exactly one child is a legal
    // persisted tree, and leaving the childless branch behind put a blank,
    // permanently unoccupiable slot in the region. Closing the last pane of a
    // region leaves a single EMPTY leaf (paneId ""), never an empty tree.
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
    // Set or clear the optional display title on the terminal leaf identified by
    // its slot label. Input is trimmed and capped at SplitNode's bound; an empty
    // result removes the field and makes the header show the generated label.
    // This is a display-only edit: paneId and terminalPaneId remain untouched.
    //
    // Like setPaneUrl(), a write made while this region's current load is in
    // flight follows the load/write ordering rule: it is queued for the current
    // session/generation and replayed only if that pane still exists. A genuine
    // load error reports the existing unloadable-layout error instead.
    Q_INVOKABLE QString setPaneTitle(QString region, QString paneId,
                                     QString title);

    // Bind the terminal leaf labelled `paneName` to the `terminal_panes` row
    // `terminalPaneId`, and persist the tree. This is the self-migration step
    // for a LEGACY layout: a leaf stored before layouts carried a row id
    // resolves once by its slot label, and ch::TerminalFactory reports what
    // that found through here, so the leaf never has to ask by label again -
    // which matters because a label is recyclable and a row id is not.
    //
    // Silent and free in every other case: a different Dev Session, an unknown
    // pane (it was closed while the lookup travelled), or a leaf that already
    // carries this id. That last one is what makes it happen ONCE rather than
    // on every attach.
    //
    // Persisted through the same path every other edit uses, so it also retires
    // any getLayout still on the wire (RegionState::superseded): a backfill is
    // a local edit, and a crossing server reply must not revert it.
    void bindTerminalPaneRow(const QString& devSessionId, const QString& paneName,
                             const QString& terminalPaneId);

    // How long a `terminal_panes` mint may go unanswered before the leaf that
    // is waiting for it is taken back out (see splitPane). 0 disables the
    // deadline and leaves the transport as the only backstop. Default
    // kDefaultTerminalMintTimeoutMs.
    void setTerminalMintTimeoutMs(int ms);
    int terminalMintTimeoutMs() const { return m_mintTimeoutMs; }

    // Deliberately far beyond any answer a healthy peer takes for what is one
    // small INSERT: this is not a latency budget, it is the bound that stops an
    // unanswered mint disabling the terminal region's writes for the rest of
    // the session. It sits above CodeharbordClient's own silent-peer detection
    // (kDefaultHeartbeatIntervalMs * kDefaultHeartbeatMisses = 60 s, which
    // fails every pending request with the transport's own message) so that
    // mechanism keeps first refusal, and this only covers what it cannot see: a
    // connection that stays healthy while this one request goes missing.
    static constexpr int kDefaultTerminalMintTimeoutMs = 90000;

signals:
    void devSessionIdChanged();
    void serverIdChanged();
    void generationChanged();
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

    enum class PendingWriteKind {
        Ratios,
        Split,
        Close,
        PaneUrl,
        PaneTitle,
    };

    struct PendingWrite {
        PendingWriteKind kind = PendingWriteKind::Close;
        QString paneId;
        QString orientation;
        QStringList pathIndexes;
        QVariantList ratios;
        QString url;
        QString title;
    };

    struct RegionState {
        SplitNode tree;      // meaningful only while `valid`
        bool valid = false;  // false -> the property reads as a null QVariant
        QVariant cache;      // *tree.tryToJson() as a variant map, kept in lockstep
        // True only while this generation's getLayout answer is outstanding.
        // A current-session edit is queued during this window; an edit carrying
        // another stamp is stale and is dropped before it can inspect this tree.
        bool loading = false;
        // Edits received before the current tree arrived. They are local user
        // intent, not server replies, so they may be replayed only for this
        // generation and only after their pane/path still exists.
        QVector<PendingWrite> pendingWrites;
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
    // Return Drop for a stale session/generation stamp, Queue for a current
    // edit that arrived before this region's first successful load, and Apply
    // once a real tree is available. Rejection (no selected session/server)
    // emits the existing misuse error. Stale is deliberately silent: the user
    // already moved on, so replaying or reporting that old gesture would make
    // the new session look broken.
    enum class WriteDecision {
        Apply,
        Queue,
        Drop,
        Reject,
    };
    WriteDecision prepareWrite(int index, const QString& devSessionId,
                               quint64 generation);
    void queueWrite(int index, PendingWrite write);
    void flushPendingWrites(int index);
    void clearPendingWrites();
    void saveTreeStamped(const QString& devSessionId, quint64 generation,
                         const QString& region, const QVariant& tree);
    void setRatiosStamped(const QString& devSessionId, quint64 generation,
                          const QString& region, const QStringList& pathIndexes,
                          const QVariantList& ratios, bool replay);
    QString splitPaneStamped(const QString& devSessionId, quint64 generation,
                             const QString& region, const QString& paneId,
                             const QString& orientation, bool replay);
    void closePaneStamped(const QString& devSessionId, quint64 generation,
                          const QString& region, const QString& paneId,
                          bool replay);
    void setPaneUrlStamped(const QString& devSessionId, quint64 generation,
                           const QString& region, const QString& paneId,
                           const QString& url, bool replay);
    QString setPaneTitleStamped(const QString& devSessionId,
                                quint64 generation, const QString& region,
                                const QString& paneId, const QString& title,
                                bool replay);

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
    // Drop everything this region was showing and publish a null tree to QML.
    // Used by every "this layout cannot be used" verdict a load can reach: a
    // getLayout that failed, and one that came back with duplicate leaf keys.
    void invalidateRegion(int index);
    // Store `tree` in the slot, reserve the pane suffixes it carries, refresh
    // the variant cache, and emit the region's changed signal.
    void setTree(int index, SplitNode tree);
    // Store without emitting - for edits QML already applied (see the signal
    // discipline note above).
    void setTreeQuietly(int index, SplitNode tree);
    // Refresh the variant cache from the slot's CURRENT tree - the common case
    // by far, since almost every edit mutates the tree in place. Takes no tree
    // precisely so no caller has to hand the member back to us by value.
    void publishTree(int index);
    void publishTreeQuietly(int index);
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
    // Burn every "<region>-<n>" suffix `tree` carries so none of them is ever
    // shown twice for this Dev Session, and answer the next free one.
    //
    // Returns max(stored counter, highest suffix in `tree` + 1) and stores that
    // when it moved. This is a LABELLING aid and nothing more. It keeps the
    // numbers the user sees stable and non-confusing within one client: a
    // closed pane is gone from the tree but its suffix stays burnt, so the next
    // split shows a number that is not already on screen and not one the user
    // just closed. Consulting the TREE as well as the counter covers a Dev
    // Session created before the counter existed, or one whose settings file
    // was cleared, which would otherwise start again at 1 and put two panes on
    // screen wearing the same label.
    //
    // It is NOT a safety mechanism and must not be mistaken for one. A terminal
    // is identified by the `terminal_panes` row id its layout leaf carries
    // (SplitNode::terminalPaneId), minted by the server and never recycled.
    // This counter is client-LOCAL — a second machine keeps its own, starts
    // wherever the shared tree leaves it, and will happily re-issue a label a
    // closed pane's row still wears — so it never could guard identity, which
    // is exactly the defect that moved identity into the leaf.
    int reservePaneSuffix(int index, const SplitNode& tree);

    // Ask the server for a `terminal_panes` row for the terminal leaf labelled
    // `paneId`, and write its id into that leaf when the answer lands. Until
    // then the leaf is pending (see splitPane). `generation` is the load stamp
    // the mint was started under, so an answer for an abandoned Dev Session is
    // dropped rather than written into whatever tree is loaded now.
    void mintTerminalPaneRow(quint64 generation, const QString& paneId);
    // Remove the terminal leaf `paneId` whose mint came to nothing, report
    // `reason`, and republish/persist. Shared by the error answer and the
    // deadline so the two cannot drift apart.
    void abandonTerminalMint(const QString& paneId, const QString& reason);
    // Bring the terminal region's per-leaf bookkeeping back in step with a tree
    // this client did NOT derive from the server (see saveTree): drop the
    // resolve-by-label permission of every slot the authored tree no longer
    // holds - the permission belonged to that leaf, and a later leaf wearing
    // the same label is a different pane - and mint a `terminal_panes` row for
    // every leaf that has none, is not a pre-migration leaf, and has no mint in
    // flight already. Without that last step an authored tree's brand new
    // terminal leaves would be PENDING with nothing on the wire to resolve
    // them, and persist() refuses for the rest of the session to write a region
    // that holds one (see hasPendingTerminalLeaf()).
    void adoptAuthoredTerminalTree(quint64 generation);
    // Find the terminal leaf labelled `paneId` in the loaded terminal tree, or
    // nullptr. Terminal region only: a viewer leaf has no row to bind.
    SplitNode* findTerminalLeaf(const QString& paneId);
    // Remove the leaf labelled `paneId` from a region, collapsing every branch
    // the removal empties or leaves with a single child (see closePane) and
    // leaving an emptied region with a single EMPTY leaf. False when the region
    // holds no such leaf. Neither publishes nor persists; the caller decides
    // both.
    bool dropLeaf(int index, const QString& paneId);
    // True while any terminal leaf is still waiting for the `terminal_panes`
    // row being minted for it. persist() refuses to write such a tree; see
    // there for why that is load-bearing rather than tidiness.
    bool hasPendingTerminalLeaf() const;

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
    // See setTerminalMintTimeoutMs().
    int m_mintTimeoutMs = kDefaultTerminalMintTimeoutMs;
    // Terminal slot labels this client may resolve by NAME, i.e. the leaves of
    // a layout the SERVER handed us that carry no terminalPaneId. Those are
    // genuinely pre-migration leaves, and for them the label really is the
    // historical key, so the fallback is correct exactly once per leaf -
    // bindTerminalPaneRow() then writes the answer in and drops the label from
    // here.
    //
    // Everything NOT in this set and without an id is PENDING (its row is being
    // minted) and resolves nothing at all. That is the fail-safe direction: a
    // marker that goes missing leaves a pane visibly stuck instead of silently
    // adopting whatever shell happens to wear its label. Published to QML as
    // `terminalLegacy` on the leaf; never persisted, because it is a statement
    // about what THIS client has learned, not about the layout.
    QSet<QString> m_legacyTerminalSlots;
    // Terminal slot labels whose `terminal_panes` mint has been sent and not
    // yet settled, i.e. exactly the leaves that already have a row coming.
    // Kept so a tree written twice cannot start a SECOND mint for the same leaf
    // - the loser's row and its tmux session would be orphaned on the server -
    // and so adoptAuthoredTerminalTree() can tell "its row is on the way" from
    // "nothing has ever been asked for it". A label leaves the set the moment
    // its mint settles either way, and load() empties it because a new
    // generation drops every outstanding answer anyway.
    QSet<QString> m_pendingTerminalMints;
};

} // namespace ch
