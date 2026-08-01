#include <QtTest/QtTest>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSet>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "SessionBootstrap.h"
#include "SessionLayouts.h"
#include "SshConnectionPool.h"
#include "UiStateStore.h"
#include "WorkspaceDb.h"

using namespace ch;

namespace {

QByteArray jsonLine(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

// The QML-facing tree as canonical (key-sorted) JSON, for exact comparison.
QJsonObject asObject(const QVariant& tree)
{
    return QJsonValue::fromVariant(tree).toObject();
}

QByteArray compact(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QJsonObject leaf(const QString& paneId)
{
    return QJsonObject{{"type", "leaf"}, {"paneId", paneId}};
}

// A terminal leaf bound to its server-minted `terminal_panes` row. That id is
// the pane's identity; the paneId beside it is only a slot label.
QJsonObject terminalLeaf(const QString& paneId, const QString& rowId)
{
    return QJsonObject{{"type", "leaf"}, {"paneId", paneId}, {"terminalPaneId", rowId}};
}

// How a leaf with no row id is PUBLISHED after being loaded from the server:
// marked as pre-migration, i.e. allowed to resolve by its label exactly once.
// The marker is client-side only and never persisted.
QJsonObject legacyLeaf(const QString& paneId)
{
    return QJsonObject{{"type", "leaf"}, {"paneId", paneId}, {"terminalLegacy", true}};
}

// The row id the fake server hands back for a mint of `paneId`. Deterministic
// so a test can predict the tree that results.
QString rowIdFor(const QString& paneId)
{
    return QStringLiteral("row-") + paneId;
}

// A workspace.createTerminalPane / resolveTerminalPane result row. Only `id`
// and `tmuxTarget` are read by the client.
QJsonObject terminalPaneRow(const QString& paneId)
{
    return QJsonObject{{"id", rowIdFor(paneId)},
                       {"serverId", "srv-1"},
                       {"devSessionId", "s1"},
                       {"name", paneId},
                       {"tmuxTarget", QStringLiteral("ch_s1_") + rowIdFor(paneId)},
                       {"position", 0}};
}

QJsonObject split(const QString& orientation, const QJsonArray& children,
                  const QJsonArray& ratios)
{
    return QJsonObject{{"type", "split"},
                       {"orientation", orientation},
                       {"children", children},
                       {"ratios", ratios}};
}

// workspace.getLayout/setLayout return a SessionLayout row; only "tree" matters.
QJsonObject layoutRow(const QJsonObject& tree)
{
    return QJsonObject{{"id", "layout-1"}, {"tree", tree}};
}

// The terminal region default AFTER its two rows have been minted: two stacked
// leaves, each bound to the row the fake server handed back.
QJsonObject seededTerminalTree()
{
    return QJsonObject{
        {"type", "split"},
        {"orientation", "vertical"},
        {"children",
         QJsonArray{terminalLeaf(QStringLiteral("terminal-1"),
                                 rowIdFor(QStringLiteral("terminal-1"))),
                    terminalLeaf(QStringLiteral("terminal-2"),
                                 rowIdFor(QStringLiteral("terminal-2")))}},
        {"ratios", QJsonArray{1, 1}}};
}

// Every leaf's terminalPaneId, depth first, one entry per leaf (empty for a
// leaf that carries none). The live case cannot spell a server-minted UUID out
// in a literal, so it asserts on these instead.
QStringList terminalRowIds(const QJsonObject& node)
{
    const QJsonArray children = node.value(QStringLiteral("children")).toArray();
    if (children.isEmpty())
        return {node.value(QStringLiteral("terminalPaneId")).toString()};
    QStringList ids;
    for (const QJsonValue& child : children)
        ids += terminalRowIds(child.toObject());
    return ids;
}

// The same tree with every leaf's terminalPaneId taken out, so the STRUCTURE -
// slot labels, orientation, ratios - can be compared against a literal while
// the ids the server chose are asserted separately.
QJsonObject withoutRowIds(const QJsonObject& node)
{
    QJsonObject stripped = node;
    const QJsonArray children = node.value(QStringLiteral("children")).toArray();
    if (children.isEmpty()) {
        stripped.remove(QStringLiteral("terminalPaneId"));
        return stripped;
    }
    QJsonArray rebuilt;
    for (const QJsonValue& child : children)
        rebuilt.append(withoutRowIds(child.toObject()));
    stripped[QStringLiteral("children")] = rebuilt;
    return stripped;
}

} // namespace

// Exercises SessionLayouts against a QLocalSocket pair standing in for
// codeharbord: the CodeharbordClient transport is one socket, the test reads the
// workspace.getLayout/setLayout frames the bridge emits and writes canned
// JSON-RPC responses on the other. The final case runs the same flow against the
// REAL Node codeharbord over the live SSH fixture.
class TstSessionLayouts : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void loadWithoutPersistedLayoutSeedsTheRegionDefaults();
    void terminalDefaultIsTwoStackedPanesAndIsPersisted();
    void splitSerializesExactSetLayoutParams();
    void splitPersistsAndReloadsByteIdentical();
    void closeCollapsesBranchIntoSurvivor();
    void closeLastPaneYieldsEmptyLeaf();
    void ratiosPersistedAtNestedPath();
    void paneUrlPersistsWithoutRebuildingPanes();
    void staleLoadReplyIsDropped();
    void editDuringAnInFlightLoadIsNotReverted();
    void aSeedingLoadReplyNeverOverwritesASavedTree();
    void rpcErrorSurfacesWithoutCorruptingTree();
    void invalidSavedTreeIsRejected();
    void publishedTreeIsConsumableFromQml();
    void splitAfterCloseNeverReusesThePaneId();
    void refillingAnEmptiedRegionNeverReusesThePaneId();
    void paneNumberingIsPerDevSessionAndSurvivesRestart();
    void legacyTerminalLeafIsBackfilledOnceAndPersisted();
    void aFailedTerminalMintReportsAndTakesTheHalfMadePaneBack();
    void aPaneCreatedAfterOneWithTheSameLabelWasClosedGetsItsOwnRow();
    void liveLayoutRoundTripOverSsh();

private:
    void makePair();
    // Pop one framed JSON-RPC request; empty object on timeout.
    QJsonObject nextRequest();
    void respondResult(int id, const QJsonValue& result);
    void respondError(int id, int code, const QString& message);
    // True when the bridge wrote nothing more (used to prove load() never
    // writes to the server).
    bool noMoreRequests();
    // Drive load(sessionId) to completion, answering both getLayout requests
    // AND the workspace.setLayout seed each region answered with null then
    // writes back (see applyLoadedTree), so every case starts from a quiet
    // socket.
    void completeLoad(SessionLayouts& layouts, const QString& sessionId,
                      const QJsonValue& viewerTree,
                      const QJsonValue& terminalTree);

    QLocalServer* m_server = nullptr;
    QLocalSocket* m_clientSide = nullptr; // transport bound to the client
    QLocalSocket* m_serverSide = nullptr; // test reads/writes canned frames here
    CodeharbordClient* m_client = nullptr;
    WorkspaceDb* m_db = nullptr;
    // Per-test settings file, so one case's pane-suffix counters can never be
    // read by the next: the counter is keyed by Dev Session id and every case
    // here uses "s1".
    std::unique_ptr<QTemporaryDir> m_settingsDir;
    UiStateStore* m_uiState = nullptr;
    QString settingsPath() const
    {
        return m_settingsDir->filePath(QStringLiteral("uistate.ini"));
    }
    QByteArray m_rxBuffer;                // holds frames read but not yet popped
    static int s_seq;
};

int TstSessionLayouts::s_seq = 0;

void TstSessionLayouts::init()
{
    m_client = new CodeharbordClient;
    m_db = new WorkspaceDb(m_client);
    m_settingsDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_settingsDir->isValid());
    m_uiState = new UiStateStore(settingsPath());
    m_rxBuffer.clear();
}

void TstSessionLayouts::cleanup()
{
    delete m_uiState;
    m_uiState = nullptr;
    m_settingsDir.reset();
    delete m_db;
    m_db = nullptr;
    delete m_client;
    m_client = nullptr;
    delete m_serverSide;
    m_serverSide = nullptr;
    delete m_clientSide;
    m_clientSide = nullptr;
    delete m_server;
    m_server = nullptr;
}

void TstSessionLayouts::makePair()
{
    const QString name = QStringLiteral("ch_sl_test_%1_%2")
                             .arg(QCoreApplication::applicationPid())
                             .arg(++s_seq);
    QLocalServer::removeServer(name);

    m_server = new QLocalServer;
    QVERIFY(m_server->listen(name));

    m_clientSide = new QLocalSocket;
    m_clientSide->connectToServer(name);
    QVERIFY(m_clientSide->waitForConnected(2000));
    QVERIFY(m_server->waitForNewConnection(2000));
    m_serverSide = m_server->nextPendingConnection();
    QVERIFY(m_serverSide != nullptr);

    m_client->setTransport(m_clientSide);
}

QJsonObject TstSessionLayouts::nextRequest()
{
    m_clientSide->flush();
    QDeadlineTimer deadline(3000);
    // Buffer across calls: load() writes BOTH getLayout frames before the test
    // reads either, so a reader that dropped the tail of its read would lose the
    // second request.
    while (!m_rxBuffer.contains('\n') && !deadline.hasExpired()) {
        // The client side needs the event loop to see a response we already
        // wrote: a request can be the CONSEQUENCE of one (a mint reply is what
        // releases the layout write), and waiting on the server socket alone
        // would never deliver it.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (m_serverSide->bytesAvailable() > 0
            || m_serverSide->waitForReadyRead(10))
            m_rxBuffer += m_serverSide->readAll();
    }
    const qsizetype newline = m_rxBuffer.indexOf('\n');
    if (newline < 0)
        return {};
    const QByteArray line = m_rxBuffer.left(newline);
    m_rxBuffer.remove(0, newline + 1);
    return QJsonDocument::fromJson(line).object();
}

void TstSessionLayouts::respondResult(int id, const QJsonValue& result)
{
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    m_serverSide->flush();
}

void TstSessionLayouts::respondError(int id, int code, const QString& message)
{
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", id},
         {"error", QJsonObject{{"code", code}, {"message", message}}}}));
    m_serverSide->flush();
}

bool TstSessionLayouts::noMoreRequests()
{
    m_clientSide->flush();
    QTest::qWait(50);
    if (m_serverSide->bytesAvailable() > 0)
        m_rxBuffer += m_serverSide->readAll();
    return m_rxBuffer.isEmpty();
}

void TstSessionLayouts::completeLoad(SessionLayouts& layouts,
                                     const QString& sessionId,
                                     const QJsonValue& viewerTree,
                                     const QJsonValue& terminalTree)
{
    QSignalSpy loadedSpy(&layouts, &SessionLayouts::loaded);
    layouts.load(sessionId);
    const QJsonObject first = nextRequest();
    const QJsonObject second = nextRequest();
    QCOMPARE(first.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.getLayout"));
    QCOMPARE(second.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.getLayout"));
    // Both regions are fetched, viewer first.
    QCOMPARE(first.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("region")).toString(),
             QStringLiteral("viewer"));
    QCOMPARE(second.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("region")).toString(),
             QStringLiteral("terminal"));

    respondResult(first.value(QStringLiteral("id")).toInt(), viewerTree);
    respondResult(second.value(QStringLiteral("id")).toInt(), terminalTree);
    QTRY_COMPARE(loadedSpy.count(), 1);
    QCOMPARE(loadedSpy.at(0).at(0).toString(), sessionId);

    // A region the server had no row for is SEEDED: applyLoadedTree adopts the
    // region default and persists it, so two stacked terminals survive a restart
    // instead of being re-derived until something else saves a tree. Answer
    // those writes here; the case that asserts on them is
    // terminalDefaultIsTwoStackedPanesAndIsPersisted().
    if (viewerTree.isNull()) {
        const QJsonObject seed = nextRequest();
        QCOMPARE(seed.value(QStringLiteral("method")).toString(),
                 QStringLiteral("workspace.setLayout"));
        const QJsonObject params = seed.value(QStringLiteral("params")).toObject();
        respondResult(seed.value(QStringLiteral("id")).toInt(),
                      layoutRow(params.value(QStringLiteral("tree")).toObject()));
    }
    if (terminalTree.isNull()) {
        // Both leaves of the seeded terminal default are BRAND NEW panes, so
        // each mints its own terminal_panes row first. The layout write is held
        // back until the last id lands: an id-less terminal leaf on the server
        // is indistinguishable from a pre-migration one, and must never be
        // written.
        for (const QString& label :
             {QStringLiteral("terminal-1"), QStringLiteral("terminal-2")}) {
            const QJsonObject mint = nextRequest();
            QCOMPARE(mint.value(QStringLiteral("method")).toString(),
                     QStringLiteral("workspace.createTerminalPane"));
            QCOMPARE(mint.value(QStringLiteral("params")).toObject()
                         .value(QStringLiteral("name")).toString(),
                     label);
            respondResult(mint.value(QStringLiteral("id")).toInt(),
                          terminalPaneRow(label));
        }
        const QJsonObject seed = nextRequest();
        QCOMPARE(seed.value(QStringLiteral("method")).toString(),
                 QStringLiteral("workspace.setLayout"));
        const QJsonObject params = seed.value(QStringLiteral("params")).toObject();
        respondResult(seed.value(QStringLiteral("id")).toInt(),
                      layoutRow(params.value(QStringLiteral("tree")).toObject()));
    }
}

void TstSessionLayouts::loadWithoutPersistedLayoutSeedsTheRegionDefaults()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));

    QSignalSpy viewerSpy(&layouts, &SessionLayouts::viewerTreeChanged);
    QSignalSpy terminalSpy(&layouts, &SessionLayouts::terminalTreeChanged);

    // Not loaded yet: both regions read as a null QVariant so the QML regions
    // stay inert instead of building a throwaway pane.
    QVERIFY(layouts.viewerTree().isNull());
    QVERIFY(layouts.terminalTree().isNull());

    // The server has no layout row for either region.
    completeLoad(layouts, QStringLiteral("s1"), QJsonValue(QJsonValue::Null),
                 QJsonValue(QJsonValue::Null));

    QCOMPARE(layouts.devSessionId(), QStringLiteral("s1"));
    QCOMPARE(compact(asObject(layouts.viewerTree())),
             compact(leaf(QStringLiteral("viewer-1"))));
    // Each seeded terminal leaf carries the id of the row minted for it. That
    // id is the pane's identity; the "terminal-N" beside it is only a label.
    QCOMPARE(compact(asObject(layouts.terminalTree())),
             compact(seededTerminalTree()));
    QCOMPARE(viewerSpy.count(), 1);
    // Once for the default itself, then once per row id written into it.
    QCOMPARE(terminalSpy.count(), 3);

    // completeLoad() already consumed one seed write per region; nothing else
    // reaches the server on a plain selection.
    QVERIFY(noMoreRequests());
}

// A new Dev Session comes up with TWO terminals, one above the other, and they
// are in the database rather than re-derived on every load: a single terminal
// was only ever the smallest thing that rendered, and reaching a second one
// meant hunting for "Split Terminal Pane" in the command palette.
void TstSessionLayouts::terminalDefaultIsTwoStackedPanesAndIsPersisted()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));

    QSignalSpy loadedSpy(&layouts, &SessionLayouts::loaded);
    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);

    // Driven by hand rather than through completeLoad(), because the seed writes
    // ARE what this case is about.
    layouts.load(QStringLiteral("s1"));
    const QJsonObject viewerGet = nextRequest();
    const QJsonObject terminalGet = nextRequest();
    respondResult(viewerGet.value(QStringLiteral("id")).toInt(),
                  QJsonValue(QJsonValue::Null));
    respondResult(terminalGet.value(QStringLiteral("id")).toInt(),
                  QJsonValue(QJsonValue::Null));
    QTRY_COMPARE(loadedSpy.count(), 1);

    // "vertical" stacks children top to bottom, so terminal-1 is the upper pane.
    // Published immediately, with no row ids yet: the panes are on screen while
    // their identities are still on the wire.
    const QJsonObject stacked = split(QStringLiteral("vertical"),
                                      {leaf(QStringLiteral("terminal-1")),
                                       leaf(QStringLiteral("terminal-2"))},
                                      {1, 1});
    QCOMPARE(compact(asObject(layouts.terminalTree())), compact(stacked));

    // The viewer default is written straight away. The terminal default is NOT:
    // both its leaves are brand new panes with no `terminal_panes` row yet, and
    // an id-less terminal leaf on the server is indistinguishable from one
    // written before layouts carried ids - which a later load would then resolve
    // by its recyclable label. So the mints go first.
    const QJsonObject viewerSeed = nextRequest();
    QCOMPARE(viewerSeed.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    const QJsonObject viewerParams =
        viewerSeed.value(QStringLiteral("params")).toObject();
    QCOMPARE(viewerParams.value(QStringLiteral("region")).toString(),
             QStringLiteral("viewer"));
    respondResult(viewerSeed.value(QStringLiteral("id")).toInt(),
                  layoutRow(viewerParams.value(QStringLiteral("tree")).toObject()));

    for (const QString& label :
         {QStringLiteral("terminal-1"), QStringLiteral("terminal-2")}) {
        const QJsonObject mint = nextRequest();
        QCOMPARE(mint.value(QStringLiteral("method")).toString(),
                 QStringLiteral("workspace.createTerminalPane"));
        const QJsonObject mintParams =
            mint.value(QStringLiteral("params")).toObject();
        QCOMPARE(mintParams.value(QStringLiteral("serverId")).toString(),
                 QStringLiteral("srv-1"));
        QCOMPARE(mintParams.value(QStringLiteral("devSessionId")).toString(),
                 QStringLiteral("s1"));
        QCOMPARE(mintParams.value(QStringLiteral("name")).toString(), label);
        respondResult(mint.value(QStringLiteral("id")).toInt(),
                      terminalPaneRow(label));
    }

    // Only once BOTH ids are in does the terminal default reach the server, and
    // it reaches it with those ids in the tree.
    const QJsonObject terminalSeedReq = nextRequest();
    QCOMPARE(terminalSeedReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    const QJsonObject terminalParams =
        terminalSeedReq.value(QStringLiteral("params")).toObject();
    QCOMPARE(terminalParams.value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-1"));
    QCOMPARE(terminalParams.value(QStringLiteral("devSessionId")).toString(),
             QStringLiteral("s1"));
    QCOMPARE(terminalParams.value(QStringLiteral("region")).toString(),
             QStringLiteral("terminal"));
    const QJsonObject terminalSeed =
        terminalParams.value(QStringLiteral("tree")).toObject();
    QCOMPARE(compact(terminalSeed), compact(seededTerminalTree()));
    respondResult(terminalSeedReq.value(QStringLiteral("id")).toInt(),
                  layoutRow(terminalSeed));
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(noMoreRequests());

    // Idempotent: the seed created the row, so reopening the session loads it
    // and writes nothing at all. Its leaves already carry row ids, so nothing
    // is minted and nothing is marked as pre-migration either.
    SessionLayouts reopened(m_db, m_uiState);
    reopened.setServerId(QStringLiteral("srv-1"));
    completeLoad(reopened, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 layoutRow(seededTerminalTree()));
    QCOMPARE(compact(asObject(reopened.terminalTree())),
             compact(seededTerminalTree()));
    QVERIFY(noMoreRequests());

    // Pane LABELLING continues past the second default pane instead of showing
    // a "terminal-2" that is already on screen. The counter is consulted
    // alongside the tree, and a Dev Session seeded before the counter existed
    // has none stored, so this is also the case that proves the tree half of
    // that maximum still guards an existing layout.
    QCOMPARE(reopened.splitPane(QStringLiteral("terminal"),
                                QStringLiteral("terminal-2"),
                                QStringLiteral("horizontal")),
             QStringLiteral("terminal-3"));
}

void TstSessionLayouts::splitSerializesExactSetLayoutParams()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));
    completeLoad(layouts, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 QJsonValue(QJsonValue::Null));

    QSignalSpy viewerSpy(&layouts, &SessionLayouts::viewerTreeChanged);
    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);

    const QString newPaneId = layouts.splitPane(
        QStringLiteral("viewer"), QStringLiteral("viewer-1"),
        QStringLiteral("vertical"));
    QCOMPARE(newPaneId, QStringLiteral("viewer-2"));
    QCOMPARE(viewerSpy.count(), 1); // structure changed: QML must rebuild
    QCOMPARE(errorSpy.count(), 0);

    const QJsonObject req = nextRequest();
    QCOMPARE(req.value(QStringLiteral("jsonrpc")).toString(),
             QStringLiteral("2.0"));
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));

    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.size(), 4); // exactly serverId, devSessionId, region, tree
    QCOMPARE(params.value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-1"));
    QCOMPARE(params.value(QStringLiteral("devSessionId")).toString(),
             QStringLiteral("s1"));
    QCOMPARE(params.value(QStringLiteral("region")).toString(),
             QStringLiteral("viewer"));

    const QJsonObject expected =
        split(QStringLiteral("vertical"),
              {leaf(QStringLiteral("viewer-1")), leaf(QStringLiteral("viewer-2"))},
              {1, 1});
    QCOMPARE(compact(params.value(QStringLiteral("tree")).toObject()),
             compact(expected));
    // What went on the wire is exactly what QML now renders.
    QCOMPARE(compact(asObject(layouts.viewerTree())), compact(expected));
}

void TstSessionLayouts::splitPersistsAndReloadsByteIdentical()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));
    completeLoad(layouts, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 QJsonValue(QJsonValue::Null));

    // Two splits, so the reloaded tree is genuinely nested.
    QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                               QStringLiteral("viewer-1"),
                               QStringLiteral("horizontal")),
             QStringLiteral("viewer-2"));
    const QJsonObject firstReq = nextRequest();
    respondResult(firstReq.value(QStringLiteral("id")).toInt(),
                  layoutRow(firstReq.value(QStringLiteral("params")).toObject()
                                .value(QStringLiteral("tree")).toObject()));

    QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                               QStringLiteral("viewer-2"),
                               QStringLiteral("vertical")),
             QStringLiteral("viewer-3"));
    const QJsonObject secondReq = nextRequest();
    const QJsonObject persisted = secondReq.value(QStringLiteral("params"))
                                      .toObject()
                                      .value(QStringLiteral("tree"))
                                      .toObject();
    respondResult(secondReq.value(QStringLiteral("id")).toInt(),
                  layoutRow(persisted));

    const QByteArray beforeReload = compact(asObject(layouts.viewerTree()));

    // A fresh bridge (a relaunched app) loading the same Dev Session must land
    // on the very same tree the server was handed.
    SessionLayouts reloaded(m_db, m_uiState);
    reloaded.setServerId(QStringLiteral("srv-1"));
    completeLoad(reloaded, QStringLiteral("s1"), layoutRow(persisted),
                 QJsonValue(QJsonValue::Null));

    QCOMPARE(compact(asObject(reloaded.viewerTree())), beforeReload);
    QCOMPARE(compact(persisted), beforeReload);
}

void TstSessionLayouts::closeCollapsesBranchIntoSurvivor()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));

    // root = split[ viewer-1 , split[ viewer-2 , viewer-3 ] ]
    const QJsonObject inner =
        split(QStringLiteral("vertical"),
              {leaf(QStringLiteral("viewer-2")), leaf(QStringLiteral("viewer-3"))},
              {3, 4});
    const QJsonObject root = split(QStringLiteral("horizontal"),
                                   {leaf(QStringLiteral("viewer-1")), inner},
                                   {1, 2});
    completeLoad(layouts, QStringLiteral("s1"), layoutRow(root),
                 QJsonValue(QJsonValue::Null));

    layouts.closePane(QStringLiteral("viewer"), QStringLiteral("viewer-3"));

    // The inner branch is left with one child and collapses into it; the outer
    // branch keeps its own orientation and ratios.
    const QJsonObject expected =
        split(QStringLiteral("horizontal"),
              {leaf(QStringLiteral("viewer-1")), leaf(QStringLiteral("viewer-2"))},
              {1, 2});
    QCOMPARE(compact(asObject(layouts.viewerTree())), compact(expected));

    const QJsonObject req = nextRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    QCOMPARE(compact(req.value(QStringLiteral("params")).toObject()
                         .value(QStringLiteral("tree")).toObject()),
             compact(expected));
}

void TstSessionLayouts::closeLastPaneYieldsEmptyLeaf()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));
    completeLoad(layouts, QStringLiteral("s1"), QJsonValue(QJsonValue::Null),
                 layoutRow(leaf(QStringLiteral("terminal-1"))));

    layouts.closePane(QStringLiteral("terminal"), QStringLiteral("terminal-1"));

    // Never an empty tree: a region always has exactly one root node.
    const QJsonObject expected = leaf(QString());
    QCOMPARE(compact(asObject(layouts.terminalTree())), compact(expected));

    const QJsonObject req = nextRequest();
    QCOMPARE(req.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("region")).toString(),
             QStringLiteral("terminal"));
    QCOMPARE(compact(req.value(QStringLiteral("params")).toObject()
                         .value(QStringLiteral("tree")).toObject()),
             compact(expected));

    // Splitting the placeholder REFILLS the region with a single pane rather
    // than branching it against a permanently dead half.
    //
    // The LABEL does not restart at 1, even though no "terminal-<n>" leaf is
    // left in the tree: the persisted counter remembers that "terminal-1" was
    // spent, so the user is not shown a number they just closed. That is all the
    // counter does now. What stops the new pane from adopting the closed pane's
    // still-running shell is the row id below, minted fresh for this leaf.
    QCOMPARE(layouts.splitPane(QStringLiteral("terminal"), QString(),
                               QStringLiteral("horizontal")),
             QStringLiteral("terminal-2"));
    // Published at once, with no row id yet - and, crucially, with no
    // "terminalLegacy" marker either, so the pane waits instead of resolving by
    // the label it happens to wear.
    QCOMPARE(compact(asObject(layouts.terminalTree())),
             compact(leaf(QStringLiteral("terminal-2"))));

    const QJsonObject mint = nextRequest();
    QCOMPARE(mint.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.createTerminalPane"));
    respondResult(mint.value(QStringLiteral("id")).toInt(),
                  terminalPaneRow(QStringLiteral("terminal-2")));

    const QJsonObject refillReq = nextRequest();
    QCOMPARE(refillReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    const QJsonObject bound = terminalLeaf(QStringLiteral("terminal-2"),
                                           rowIdFor(QStringLiteral("terminal-2")));
    QCOMPARE(compact(refillReq.value(QStringLiteral("params")).toObject()
                         .value(QStringLiteral("tree")).toObject()),
             compact(bound));
    QCOMPARE(compact(asObject(layouts.terminalTree())), compact(bound));
}

void TstSessionLayouts::ratiosPersistedAtNestedPath()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));

    const QJsonObject inner =
        split(QStringLiteral("vertical"),
              {leaf(QStringLiteral("viewer-2")), leaf(QStringLiteral("viewer-3"))},
              {1, 1});
    const QJsonObject root = split(QStringLiteral("horizontal"),
                                   {leaf(QStringLiteral("viewer-1")), inner},
                                   {1, 1});
    completeLoad(layouts, QStringLiteral("s1"), layoutRow(root),
                 QJsonValue(QJsonValue::Null));

    QSignalSpy viewerSpy(&layouts, &SessionLayouts::viewerTreeChanged);
    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);

    // ["1"] addresses root.children[1], the inner split.
    layouts.setRatios(QStringLiteral("viewer"), {QStringLiteral("1")},
                      {2.5, 7.5});
    QCOMPARE(errorSpy.count(), 0);
    // A drag must NOT re-publish the tree: QML already resized, and a new tree
    // object would destroy and rebuild every pane in the region.
    QCOMPARE(viewerSpy.count(), 0);

    const QJsonObject expected =
        split(QStringLiteral("horizontal"),
              {leaf(QStringLiteral("viewer-1")),
               split(QStringLiteral("vertical"),
                     {leaf(QStringLiteral("viewer-2")),
                      leaf(QStringLiteral("viewer-3"))},
                     {2.5, 7.5})},
              {1, 1});
    const QJsonObject req = nextRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    QCOMPARE(compact(req.value(QStringLiteral("params")).toObject()
                         .value(QStringLiteral("tree")).toObject()),
             compact(expected));
    // The cache QML would re-read carries the new ratios too.
    QCOMPARE(compact(asObject(layouts.viewerTree())), compact(expected));

    // Root ratios via the empty path, and rejections that change nothing.
    layouts.setRatios(QStringLiteral("viewer"), {}, {4.0, 6.0});
    const QJsonObject rootReq = nextRequest();
    QCOMPARE(rootReq.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("tree")).toObject()
                 .value(QStringLiteral("ratios")).toArray(),
             QJsonArray({4, 6}));

    layouts.setRatios(QStringLiteral("viewer"), {QStringLiteral("0")},
                      {1.0, 1.0}); // a leaf has no ratios
    QCOMPARE(errorSpy.count(), 1);
    layouts.setRatios(QStringLiteral("viewer"), {QStringLiteral("1")},
                      {1.0}); // wrong count
    QCOMPARE(errorSpy.count(), 2);
    layouts.setRatios(QStringLiteral("viewer"), {QStringLiteral("1")},
                      {0.0, 1.0}); // non-positive
    QCOMPARE(errorSpy.count(), 3);
    QVERIFY(noMoreRequests()); // no rejected edit reached the server
}

// Opening a file must be REMEMBERED, or a reopened session restores the right
// panes and every one of them blank. The write must also stay quiet: publishing
// a new tree would rebuild the region's delegates and destroy the very pane that
// just opened the file, so the record would undo what it recorded.
void TstSessionLayouts::paneUrlPersistsWithoutRebuildingPanes()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));

    const QJsonObject root = split(QStringLiteral("horizontal"),
                                   {leaf(QStringLiteral("viewer-1")),
                                    leaf(QStringLiteral("viewer-2"))},
                                   {1, 1});
    completeLoad(layouts, QStringLiteral("s1"), layoutRow(root),
                 QJsonValue(QJsonValue::Null));

    QSignalSpy viewerSpy(&layouts, &SessionLayouts::viewerTreeChanged);
    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);

    const QString url = QStringLiteral("codeharbor-internal://file/notes.md");
    layouts.setPaneUrl(QStringLiteral("viewer"), QStringLiteral("viewer-2"), url);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(viewerSpy.count(), 0);  // the pane showing it must survive

    const QJsonObject req = nextRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    const QJsonObject sent = req.value(QStringLiteral("params")).toObject()
                                 .value(QStringLiteral("tree")).toObject();
    QCOMPARE(sent.value(QStringLiteral("children")).toArray().at(1).toObject()
                 .value(QStringLiteral("url")).toString(), url);
    // The cache QML re-reads carries it too, so a reload restores the file.
    QCOMPARE(asObject(layouts.viewerTree()).value(QStringLiteral("children"))
                 .toArray().at(1).toObject().value(QStringLiteral("url")).toString(),
             url);

    // Every restored pane re-announces the url it was given. That echo is the
    // normal case and must not spend an RPC per pane on every session open.
    layouts.setPaneUrl(QStringLiteral("viewer"), QStringLiteral("viewer-2"), url);
    QVERIFY(noMoreRequests());

    // An unknown pane is reported, not silently dropped.
    layouts.setPaneUrl(QStringLiteral("viewer"), QStringLiteral("nope"), url);
    QCOMPARE(errorSpy.count(), 1);
}

void TstSessionLayouts::staleLoadReplyIsDropped()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));

    QSignalSpy loadedSpy(&layouts, &SessionLayouts::loaded);

    // Select s1, then switch to s2 before s1's replies come back.
    layouts.load(QStringLiteral("s1"));
    const QJsonObject staleViewer = nextRequest();
    const QJsonObject staleTerminal = nextRequest();
    QCOMPARE(staleViewer.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("devSessionId")).toString(),
             QStringLiteral("s1"));

    layouts.load(QStringLiteral("s2"));
    const QJsonObject freshViewer = nextRequest();
    const QJsonObject freshTerminal = nextRequest();
    QCOMPARE(freshViewer.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("devSessionId")).toString(),
             QStringLiteral("s2"));

    // s1's layouts arrive late. They belong to an abandoned session and must
    // not reach the trees (nor complete the s2 load).
    respondResult(staleViewer.value(QStringLiteral("id")).toInt(),
                  layoutRow(leaf(QStringLiteral("stale-1"))));
    respondResult(staleTerminal.value(QStringLiteral("id")).toInt(),
                  layoutRow(leaf(QStringLiteral("stale-2"))));
    QTest::qWait(100);
    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(layouts.viewerTree().isNull());
    QVERIFY(layouts.terminalTree().isNull());

    respondResult(freshViewer.value(QStringLiteral("id")).toInt(),
                  layoutRow(leaf(QStringLiteral("fresh-1"))));
    respondResult(freshTerminal.value(QStringLiteral("id")).toInt(),
                  layoutRow(leaf(QStringLiteral("fresh-2"))));
    QTRY_COMPARE(loadedSpy.count(), 1);
    QCOMPARE(loadedSpy.at(0).at(0).toString(), QStringLiteral("s2"));
    QCOMPARE(compact(asObject(layouts.viewerTree())),
             compact(leaf(QStringLiteral("fresh-1"))));
    // The terminal leaf came from the SERVER with no row id, so it is a
    // pre-migration leaf and is published with the marker that lets it resolve
    // by its label once.
    QCOMPARE(compact(asObject(layouts.terminalTree())),
             compact(legacyLeaf(QStringLiteral("fresh-2"))));
}

// The reverse of staleLoadReplyIsDropped(): here the LOAD is the stale one. A
// getLayout answer that crossed a layout edit on the wire describes the tree as
// it was BEFORE the edit, so applying it would silently undo the split the user
// just made - and the write that recorded the split would be the last thing the
// server heard, leaving screen and server disagreeing until the next reload.
//
// The path is not hypothetical: AppController reloads the SAME Dev Session after
// a reconnect and whenever the user re-picks the session they are already in, so
// a load and an edit routinely overlap. The guard is per region, so the region
// nobody touched still adopts whatever the server sent.
void TstSessionLayouts::editDuringAnInFlightLoadIsNotReverted()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));

    const QJsonObject storedViewer = leaf(QStringLiteral("viewer-1"));
    const QJsonObject storedTerminal =
        split(QStringLiteral("vertical"),
              {leaf(QStringLiteral("terminal-1")),
               leaf(QStringLiteral("terminal-2"))},
              {1, 1});
    completeLoad(layouts, QStringLiteral("s1"), layoutRow(storedViewer),
                 layoutRow(storedTerminal));

    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);
    QSignalSpy loadedSpy(&layouts, &SessionLayouts::loaded);

    // A deliberate reload of the session that is already open.
    layouts.load(QStringLiteral("s1"));
    const QJsonObject viewerGet = nextRequest();
    const QJsonObject terminalGet = nextRequest();
    QCOMPARE(viewerGet.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.getLayout"));
    QCOMPARE(terminalGet.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.getLayout"));

    // The user splits a viewer pane before either answer comes back.
    QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                               QStringLiteral("viewer-1"),
                               QStringLiteral("horizontal")),
             QStringLiteral("viewer-2"));
    const QJsonObject setReq = nextRequest();
    QCOMPARE(setReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    const QJsonObject edited = setReq.value(QStringLiteral("params")).toObject()
                                   .value(QStringLiteral("tree")).toObject();
    QCOMPARE(compact(edited),
             compact(split(QStringLiteral("horizontal"),
                           {leaf(QStringLiteral("viewer-1")),
                            leaf(QStringLiteral("viewer-2"))},
                           {1, 1})));
    respondResult(setReq.value(QStringLiteral("id")).toInt(), layoutRow(edited));

    // Only now do the pre-edit answers land. The terminal one describes a tree
    // the user has NOT touched (another client moved a pane) and must still be
    // adopted; the viewer one is history.
    const QJsonObject freshTerminal =
        split(QStringLiteral("horizontal"),
              {leaf(QStringLiteral("terminal-1")),
               leaf(QStringLiteral("terminal-9"))},
              {2, 3});
    respondResult(viewerGet.value(QStringLiteral("id")).toInt(),
                  layoutRow(storedViewer));
    respondResult(terminalGet.value(QStringLiteral("id")).toInt(),
                  layoutRow(freshTerminal));
    QTRY_COMPARE(loadedSpy.count(), 1);

    QCOMPARE(compact(asObject(layouts.viewerTree())), compact(edited));
    // Adopted verbatim, plus the pre-migration marker on each id-less leaf.
    QCOMPARE(compact(asObject(layouts.terminalTree())),
             compact(split(QStringLiteral("horizontal"),
                           {legacyLeaf(QStringLiteral("terminal-1")),
                            legacyLeaf(QStringLiteral("terminal-9"))},
                           {2, 3})));
    QCOMPARE(errorSpy.count(), 0);
    // Nothing was re-written: the stale answer neither reached the tree nor the
    // server.
    QVERIFY(noMoreRequests());
}

// The same collision on the branch that would do real damage. When the server
// has NO row for a region, a load reply adopts the region default and writes it
// back. Landing that on top of a tree QML has just authored would not merely
// revert it on screen - it would persist the default over it, so the edit is
// gone from the database too and a restart cannot recover it.
void TstSessionLayouts::aSeedingLoadReplyNeverOverwritesASavedTree()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));

    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);
    QSignalSpy loadedSpy(&layouts, &SessionLayouts::loaded);

    layouts.load(QStringLiteral("s1"));
    const QJsonObject viewerGet = nextRequest();
    const QJsonObject terminalGet = nextRequest();

    // QML authors and saves a viewer tree before the server has answered.
    const QJsonObject authored =
        split(QStringLiteral("horizontal"),
              {leaf(QStringLiteral("viewer-1")), leaf(QStringLiteral("viewer-2"))},
              {1, 1});
    layouts.saveTree(QStringLiteral("viewer"), authored.toVariantMap());
    const QJsonObject saveReq = nextRequest();
    QCOMPARE(saveReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    respondResult(saveReq.value(QStringLiteral("id")).toInt(), layoutRow(authored));

    // The server has a row for neither region.
    respondResult(viewerGet.value(QStringLiteral("id")).toInt(),
                  QJsonValue(QJsonValue::Null));
    respondResult(terminalGet.value(QStringLiteral("id")).toInt(),
                  QJsonValue(QJsonValue::Null));
    QTRY_COMPARE(loadedSpy.count(), 1);

    QCOMPARE(compact(asObject(layouts.viewerTree())), compact(authored));

    // The terminal region WAS seeded, so its two brand new panes mint their
    // rows before its layout may be written.
    for (const QString& label :
         {QStringLiteral("terminal-1"), QStringLiteral("terminal-2")}) {
        const QJsonObject mint = nextRequest();
        QCOMPARE(mint.value(QStringLiteral("method")).toString(),
                 QStringLiteral("workspace.createTerminalPane"));
        respondResult(mint.value(QStringLiteral("id")).toInt(),
                      terminalPaneRow(label));
    }

    // Exactly one further write reaches the server, and it is the TERMINAL
    // default: the viewer region was never seeded over.
    const QJsonObject seed = nextRequest();
    const QJsonObject seedParams = seed.value(QStringLiteral("params")).toObject();
    QCOMPARE(seed.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    QCOMPARE(seedParams.value(QStringLiteral("region")).toString(),
             QStringLiteral("terminal"));
    respondResult(seed.value(QStringLiteral("id")).toInt(),
                  layoutRow(seedParams.value(QStringLiteral("tree")).toObject()));
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(noMoreRequests());
}

void TstSessionLayouts::rpcErrorSurfacesWithoutCorruptingTree()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));
    completeLoad(layouts, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 QJsonValue(QJsonValue::Null));

    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);
    QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                               QStringLiteral("viewer-1"),
                               QStringLiteral("horizontal")),
             QStringLiteral("viewer-2"));
    const QJsonObject expected =
        split(QStringLiteral("horizontal"),
              {leaf(QStringLiteral("viewer-1")), leaf(QStringLiteral("viewer-2"))},
              {1, 1});

    const QJsonObject req = nextRequest();
    respondError(req.value(QStringLiteral("id")).toInt(), -32000,
                 QStringLiteral("layout write failed"));

    // The server message is forwarded verbatim (SPEC 10.3)...
    QTRY_COMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toString(),
             QStringLiteral("layout write failed"));
    // ...and the tree QML is rendering is untouched by the failure.
    QCOMPARE(compact(asObject(layouts.viewerTree())), compact(expected));

    // A getLayout failure leaves the region null rather than fabricating a
    // default leaf that the user could edit over their real layout.
    QSignalSpy loadedSpy(&layouts, &SessionLayouts::loaded);
    layouts.load(QStringLiteral("s2"));
    const QJsonObject viewerReq = nextRequest();
    const QJsonObject terminalReq = nextRequest();
    respondError(viewerReq.value(QStringLiteral("id")).toInt(), -32001,
                 QStringLiteral("no such dev session"));
    respondResult(terminalReq.value(QStringLiteral("id")).toInt(),
                  QJsonValue(QJsonValue::Null));
    QTRY_COMPARE(loadedSpy.count(), 1); // the load still completes
    QCOMPARE(errorSpy.count(), 2);
    QCOMPARE(errorSpy.at(1).at(0).toString(),
             QStringLiteral("no such dev session"));
    QVERIFY(layouts.viewerTree().isNull());
    QCOMPARE(compact(asObject(layouts.terminalTree())),
             compact(split(QStringLiteral("vertical"),
                           {leaf(QStringLiteral("terminal-1")),
                            leaf(QStringLiteral("terminal-2"))},
                           {1, 1})));
}

void TstSessionLayouts::invalidSavedTreeIsRejected()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));
    completeLoad(layouts, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 QJsonValue(QJsonValue::Null));

    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);
    QSignalSpy viewerSpy(&layouts, &SessionLayouts::viewerTreeChanged);

    // A split with mismatched ratios/children never reaches QML or the server.
    QVariantMap bogus;
    bogus.insert(QStringLiteral("type"), QStringLiteral("split"));
    bogus.insert(QStringLiteral("orientation"), QStringLiteral("horizontal"));
    bogus.insert(QStringLiteral("children"),
                 QVariantList{QVariantMap{{"type", "leaf"}, {"paneId", "a"}}});
    bogus.insert(QStringLiteral("ratios"), QVariantList{1.0, 1.0});
    layouts.saveTree(QStringLiteral("viewer"), bogus);
    QCOMPARE(errorSpy.count(), 1);

    layouts.saveTree(QStringLiteral("nowhere"), QVariant());
    QCOMPARE(errorSpy.count(), 2); // unknown region

    QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                               QStringLiteral("no-such-pane"),
                               QStringLiteral("horizontal")),
             QString());
    QCOMPARE(errorSpy.count(), 3);

    QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                               QStringLiteral("viewer-1"),
                               QStringLiteral("sideways")),
             QString()); // unknown orientation
    QCOMPARE(errorSpy.count(), 4);

    QCOMPARE(compact(asObject(layouts.viewerTree())),
             compact(leaf(QStringLiteral("viewer-1"))));
    QCOMPARE(viewerSpy.count(), 0);
    QVERIFY(noMoreRequests());

    // A well-formed QML-authored tree IS accepted and persisted (quietly: the
    // caller already holds it).
    const QJsonObject good =
        split(QStringLiteral("vertical"),
              {leaf(QStringLiteral("viewer-1")), leaf(QStringLiteral("viewer-9"))},
              {1, 3});
    layouts.saveTree(QStringLiteral("viewer"), good.toVariantMap());
    QCOMPARE(errorSpy.count(), 4);
    QCOMPARE(viewerSpy.count(), 0);
    const QJsonObject req = nextRequest();
    QCOMPARE(compact(req.value(QStringLiteral("params")).toObject()
                         .value(QStringLiteral("tree")).toObject()),
             compact(good));
    QCOMPARE(compact(asObject(layouts.viewerTree())), compact(good));
}

// The published tree is only useful if QML can read it with the very
// expressions ViewerRegion.qml/TerminalRegion.qml already use. This probe is a
// transcription of those accessors (isLeaf, paneId, orientation, children,
// ratioFor), so a shape regression fails here rather than silently rendering an
// empty region.
void TstSessionLayouts::publishedTreeIsConsumableFromQml()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));
    const QJsonObject inner =
        split(QStringLiteral("vertical"),
              {leaf(QStringLiteral("viewer-2")), leaf(QStringLiteral("viewer-3"))},
              {1, 3});
    const QJsonObject root = split(QStringLiteral("horizontal"),
                                   {leaf(QStringLiteral("viewer-1")), inner},
                                   {2, 5});
    completeLoad(layouts, QStringLiteral("s1"), layoutRow(root),
                 QJsonValue(QJsonValue::Null));

    static const char* kProbe = R"(
import QtQml
QtObject {
    property var node: null
    function isLeaf(n) { return !n || !n.children || n.children.length === 0; }
    function ratioFor(i, count) {
        if (count <= 0)
            return 1;
        const r = node && node.ratios ? node.ratios : null;
        if (r && r.length === count) {
            let sum = 0;
            for (let k = 0; k < count; ++k)
                sum += r[k] > 0 ? r[k] : 0;
            if (sum > 0 && r[i] > 0)
                return r[i] / sum;
        }
        return 1 / count;
    }
    property bool rootIsLeaf: isLeaf(node)
    property string rootPaneId: node && node.paneId ? node.paneId : ""
    property string orientation: node && node.orientation ? node.orientation : ""
    property int childCount: node && node.children ? node.children.length : 0
    property string firstChildPaneId: childCount > 0 && node.children[0].paneId
                                      ? node.children[0].paneId : ""
    property bool secondChildIsLeaf: childCount > 1 ? isLeaf(node.children[1])
                                                    : true
    property string grandchildPaneId:
        childCount > 1 && node.children[1].children
        && node.children[1].children.length > 1
            ? node.children[1].children[1].paneId : ""
    property real firstRatio: ratioFor(0, childCount)
}
)";

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(kProbe, QUrl(QStringLiteral("qrc:/tst_probe.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const std::unique_ptr<QObject> probe(component.create());
    QVERIFY(probe);

    probe->setProperty("node", layouts.viewerTree());
    QCOMPARE(probe->property("rootIsLeaf").toBool(), false);
    QCOMPARE(probe->property("orientation").toString(),
             QStringLiteral("horizontal"));
    QCOMPARE(probe->property("childCount").toInt(), 2);
    QCOMPARE(probe->property("firstChildPaneId").toString(),
             QStringLiteral("viewer-1"));
    QCOMPARE(probe->property("secondChildIsLeaf").toBool(), false);
    QCOMPARE(probe->property("grandchildPaneId").toString(),
             QStringLiteral("viewer-3"));
    QVERIFY(qFuzzyCompare(probe->property("firstRatio").toDouble(), 2.0 / 7.0));

    // The other region's seeded default reads as the two stacked terminals.
    probe->setProperty("node", layouts.terminalTree());
    QCOMPARE(probe->property("rootIsLeaf").toBool(), false);
    QCOMPARE(probe->property("orientation").toString(),
             QStringLiteral("vertical"));
    QCOMPARE(probe->property("childCount").toInt(), 2);
    QCOMPARE(probe->property("firstChildPaneId").toString(),
             QStringLiteral("terminal-1"));
    QCOMPARE(probe->property("secondChildIsLeaf").toBool(), true);
    QVERIFY(qFuzzyCompare(probe->property("firstRatio").toDouble(), 0.5));

    // A not-yet-loaded region hands QML a null node, which keeps it inert.
    SessionLayouts empty(m_db, m_uiState);
    probe->setProperty("node", empty.viewerTree());
    QVERIFY(probe->property("node").isNull());
    QCOMPARE(probe->property("rootIsLeaf").toBool(), true);
    QCOMPARE(probe->property("rootPaneId").toString(), QString());
}

// A pane id is spent for good. Closing a pane leaves its REMOTE tmux session
// running on purpose (TerminalRegion.qml detaches instead of killing, so a pane
// survives a disconnect), and the id IS that session's name
// ("ch_<devSessionId>_<paneId>", attached with `tmux new-session -A`). Minting
// the id again therefore does not create a new shell at all: it re-attaches the
// closed one, complete with its scrollback, its working directory and whatever
// was still running in it, and the working directory the new pane asked for is
// silently discarded (`-c <dir>` only applies when the session is created).
void TstSessionLayouts::splitAfterCloseNeverReusesThePaneId()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));
    const QJsonObject twoPanes =
        split(QStringLiteral("horizontal"),
              {leaf(QStringLiteral("viewer-1")), leaf(QStringLiteral("viewer-2"))},
              {1, 1});
    completeLoad(layouts, QStringLiteral("s1"), layoutRow(twoPanes),
                 layoutRow(leaf(QStringLiteral("terminal-1"))));

    layouts.closePane(QStringLiteral("viewer"), QStringLiteral("viewer-2"));
    QCOMPARE(compact(asObject(layouts.viewerTree())),
             compact(leaf(QStringLiteral("viewer-1"))));
    nextRequest(); // the close's setLayout

    // "viewer-2" is gone from the tree, so the highest suffix left is 1 - and
    // the old rule handed "viewer-2" straight back out.
    QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                               QStringLiteral("viewer-1"),
                               QStringLiteral("vertical")),
             QStringLiteral("viewer-3"));
    nextRequest();

    // And again: the pane that was just created is closed and the next id moves
    // on once more, so no amount of churn walks back over spent ids.
    layouts.closePane(QStringLiteral("viewer"), QStringLiteral("viewer-3"));
    nextRequest();
    QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                               QStringLiteral("viewer-1"),
                               QStringLiteral("vertical")),
             QStringLiteral("viewer-4"));
}

// The same rule when the region is emptied COMPLETELY. This is the worst case
// for the old numbering: with no "<region>-<n>" leaf left anywhere in the tree
// the highest suffix read as 0, so the next split restarted at 1 and attached
// the first pane's surviving shell.
void TstSessionLayouts::refillingAnEmptiedRegionNeverReusesThePaneId()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));
    const QJsonObject stacked = split(QStringLiteral("vertical"),
                                      {leaf(QStringLiteral("terminal-1")),
                                       leaf(QStringLiteral("terminal-2"))},
                                      {1, 1});
    completeLoad(layouts, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 layoutRow(stacked));

    layouts.closePane(QStringLiteral("terminal"), QStringLiteral("terminal-2"));
    nextRequest();
    layouts.closePane(QStringLiteral("terminal"), QStringLiteral("terminal-1"));
    nextRequest();
    // The placeholder leaf: no pane, and the only handle QML has on the region.
    QCOMPARE(compact(asObject(layouts.terminalTree())), compact(leaf(QString())));

    QCOMPARE(layouts.splitPane(QStringLiteral("terminal"), QString(),
                               QStringLiteral("horizontal")),
             QStringLiteral("terminal-3"));
    // A refilled region's pane is a NEW pane: it mints its own row rather than
    // resolving anything by the label it wears.
    const QJsonObject mint = nextRequest();
    QCOMPARE(mint.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.createTerminalPane"));
    respondResult(mint.value(QStringLiteral("id")).toInt(),
                  terminalPaneRow(QStringLiteral("terminal-3")));
    nextRequest(); // the setLayout that follows the last id landing

    QCOMPARE(layouts.splitPane(QStringLiteral("terminal"),
                               QStringLiteral("terminal-3"),
                               QStringLiteral("horizontal")),
             QStringLiteral("terminal-4"));
}

// The counter is per (Dev Session, region) and lives in the settings file, not
// in the objects: two Dev Sessions never consume each other's numbers, the two
// regions of one session number independently, and a relaunch does not hand out
// ids the previous run already spent (the tmux sessions those ids name outlive
// the client process - that is the whole point of tmux here).
void TstSessionLayouts::paneNumberingIsPerDevSessionAndSurvivesRestart()
{
    makePair();
    // An empty leaf is a legitimate stored tree - it is exactly what a region
    // whose last pane closed persists - so this is a region with NO pane, the
    // only shape in which the very first minted id is observable.
    const QJsonObject emptyRegion = leaf(QString());
    {
        SessionLayouts layouts(m_db, m_uiState);
        layouts.setServerId(QStringLiteral("srv-1"));
        completeLoad(layouts, QStringLiteral("s1"), layoutRow(emptyRegion),
                     layoutRow(emptyRegion));

        // A fresh Dev Session still starts at 1; nothing existing is renumbered.
        QCOMPARE(layouts.splitPane(QStringLiteral("viewer"), QString(),
                                   QStringLiteral("horizontal")),
                 QStringLiteral("viewer-1"));
        nextRequest();
        // Per REGION: the terminal region keeps its own counter instead of
        // continuing the viewer's.
        QCOMPARE(layouts.splitPane(QStringLiteral("terminal"), QString(),
                                   QStringLiteral("horizontal")),
                 QStringLiteral("terminal-1"));
        nextRequest();
        QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                                   QStringLiteral("viewer-1"),
                                   QStringLiteral("vertical")),
                 QStringLiteral("viewer-2"));
        nextRequest();

        // Per DEV SESSION: s2 starts its own numbering at 1, and spending it
        // leaves s1's counter alone (checked after the restart below).
        completeLoad(layouts, QStringLiteral("s2"), layoutRow(emptyRegion),
                     layoutRow(emptyRegion));
        QCOMPARE(layouts.splitPane(QStringLiteral("viewer"), QString(),
                                   QStringLiteral("horizontal")),
                 QStringLiteral("viewer-1"));
        nextRequest();
    }

    // RESTART: a new store over the SAME settings file and a new bridge, as a
    // relaunched application builds. s1 spent viewer-1 and viewer-2; the server
    // hands back a tree holding only viewer-1 (viewer-2 was closed in between),
    // so the tree alone would say "2 is free".
    delete m_uiState;
    m_uiState = new UiStateStore(settingsPath());
    SessionLayouts relaunched(m_db, m_uiState);
    relaunched.setServerId(QStringLiteral("srv-1"));
    completeLoad(relaunched, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 layoutRow(leaf(QStringLiteral("terminal-1"))));
    QCOMPARE(relaunched.splitPane(QStringLiteral("viewer"),
                                  QStringLiteral("viewer-1"),
                                  QStringLiteral("vertical")),
             QStringLiteral("viewer-3"));
    nextRequest();
    // s2's counter is its own and stands at 2, untouched by everything s1 did.
    completeLoad(relaunched, QStringLiteral("s2"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 layoutRow(emptyRegion));
    QCOMPARE(relaunched.splitPane(QStringLiteral("viewer"),
                                  QStringLiteral("viewer-1"),
                                  QStringLiteral("vertical")),
             QStringLiteral("viewer-2"));
}

// SELF-MIGRATION. A terminal leaf stored before layouts carried a row id has
// nothing but its slot label, so ch::TerminalFactory resolves it by that label
// once and reports the row it found back through bindTerminalPaneRow(). The id
// then goes into the leaf and is persisted, and the recyclable label is never
// used to name a shell again. It must happen exactly ONCE: the factory caches
// its answer and re-reports it on a reconnect, and a write per attach would be
// an RPC storm for nothing.
void TstSessionLayouts::legacyTerminalLeafIsBackfilledOnceAndPersisted()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));

    const QJsonObject storedTerminal =
        split(QStringLiteral("vertical"),
              {leaf(QStringLiteral("terminal-1")), leaf(QStringLiteral("terminal-2"))},
              {1, 1});
    completeLoad(layouts, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 layoutRow(storedTerminal));

    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);
    QSignalSpy terminalSpy(&layouts, &SessionLayouts::terminalTreeChanged);

    // Both leaves come back marked: they carry no row id and the SERVER is where
    // they came from, which is the only way a leaf earns permission to be
    // resolved by its label.
    QCOMPARE(compact(asObject(layouts.terminalTree())),
             compact(split(QStringLiteral("vertical"),
                           {legacyLeaf(QStringLiteral("terminal-1")),
                            legacyLeaf(QStringLiteral("terminal-2"))},
                           {1, 1})));

    layouts.bindTerminalPaneRow(QStringLiteral("s1"), QStringLiteral("terminal-2"),
                                QStringLiteral("row-abc"));

    const QJsonObject migrated =
        split(QStringLiteral("vertical"),
              {legacyLeaf(QStringLiteral("terminal-1")),
               terminalLeaf(QStringLiteral("terminal-2"), QStringLiteral("row-abc"))},
              {1, 1});
    const QJsonObject req = nextRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    const QJsonObject persisted = req.value(QStringLiteral("params")).toObject()
                                      .value(QStringLiteral("tree")).toObject();
    // The marker is a client-side statement and must never reach the server.
    QCOMPARE(compact(persisted),
             compact(split(QStringLiteral("vertical"),
                           {leaf(QStringLiteral("terminal-1")),
                            terminalLeaf(QStringLiteral("terminal-2"),
                                         QStringLiteral("row-abc"))},
                           {1, 1})));
    respondResult(req.value(QStringLiteral("id")).toInt(), layoutRow(persisted));
    QCOMPARE(compact(asObject(layouts.terminalTree())), compact(migrated));
    // Quiet: the pane is already attached to that very row, so republishing the
    // tree would only churn the region.
    QCOMPARE(terminalSpy.count(), 0);

    // ONCE. The same answer again, and an answer for a leaf that is already
    // bound, both cost nothing.
    layouts.bindTerminalPaneRow(QStringLiteral("s1"), QStringLiteral("terminal-2"),
                                QStringLiteral("row-abc"));
    layouts.bindTerminalPaneRow(QStringLiteral("s1"), QStringLiteral("terminal-2"),
                                QStringLiteral("row-different"));
    // Wrong Dev Session, and a pane closed while the lookup travelled: silent.
    layouts.bindTerminalPaneRow(QStringLiteral("s2"), QStringLiteral("terminal-1"),
                                QStringLiteral("row-elsewhere"));
    layouts.bindTerminalPaneRow(QStringLiteral("s1"), QStringLiteral("gone"),
                                QStringLiteral("row-gone"));
    QVERIFY(noMoreRequests());
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(compact(asObject(layouts.terminalTree())), compact(migrated));
}

// A mint that fails must not leave a pane the user can never bring up, and must
// not leave an id-less terminal leaf anywhere near the server: on a later load
// that leaf would be indistinguishable from a pre-migration one and would be
// resolved by its recyclable label. So the half-made pane is taken back out and
// the reason is surfaced.
void TstSessionLayouts::aFailedTerminalMintReportsAndTakesTheHalfMadePaneBack()
{
    makePair();
    SessionLayouts layouts(m_db, m_uiState);
    layouts.setServerId(QStringLiteral("srv-1"));
    const QJsonObject stored =
        terminalLeaf(QStringLiteral("terminal-1"), QStringLiteral("row-one"));
    completeLoad(layouts, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))), layoutRow(stored));

    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);
    QCOMPARE(layouts.splitPane(QStringLiteral("terminal"),
                               QStringLiteral("terminal-1"),
                               QStringLiteral("horizontal")),
             QStringLiteral("terminal-2"));

    const QJsonObject mint = nextRequest();
    QCOMPARE(mint.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.createTerminalPane"));
    respondError(mint.value(QStringLiteral("id")).toInt(), -32000,
                 QStringLiteral("terminal pane create failed"));

    QTRY_COMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toString(),
             QStringLiteral("terminal pane create failed"));
    // The split is undone: the branch collapses back onto the survivor.
    QTRY_COMPARE(compact(asObject(layouts.terminalTree())), compact(stored));

    // Only now does anything reach the server, and it is the corrected tree -
    // never one carrying the id-less leaf.
    const QJsonObject write = nextRequest();
    QCOMPARE(write.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    QCOMPARE(compact(write.value(QStringLiteral("params")).toObject()
                         .value(QStringLiteral("tree")).toObject()),
             compact(stored));
}

// THE regression the whole scheme exists to remove, end to end in the client.
//
// Client A has terminal-1 and terminal-2 and closes terminal-2. Closing keeps
// the row and the remote tmux session alive on purpose, so the shell is still
// running; the shared layout, though, no longer mentions terminal-2. Client B
// then splits. Its slot counter is client-LOCAL, it has never heard of the
// closed pane, and the tree it loaded tops out at terminal-1 - so it mints the
// label "terminal-2" all over again.
//
// Under the old scheme that pane resolved (devSession, "terminal-2"), found A's
// surviving row and attached A's closed shell: old scrollback, old working
// directory, whatever was still running. Here it must get a row of its own.
void TstSessionLayouts::aPaneCreatedAfterOneWithTheSameLabelWasClosedGetsItsOwnRow()
{
    makePair();

    // --- Client A ---------------------------------------------------------
    const QJsonObject closedRow =
        terminalLeaf(QStringLiteral("terminal-2"), QStringLiteral("row-A-terminal-2"));
    const QJsonObject survivor =
        terminalLeaf(QStringLiteral("terminal-1"), QStringLiteral("row-A-terminal-1"));
    {
        SessionLayouts clientA(m_db, m_uiState);
        clientA.setServerId(QStringLiteral("srv-1"));
        completeLoad(clientA, QStringLiteral("s1"),
                     layoutRow(leaf(QStringLiteral("viewer-1"))),
                     layoutRow(split(QStringLiteral("vertical"), {survivor, closedRow},
                                     {1, 1})));
        clientA.closePane(QStringLiteral("terminal"), QStringLiteral("terminal-2"));
        const QJsonObject afterClose = nextRequest();
        QCOMPARE(afterClose.value(QStringLiteral("method")).toString(),
                 QStringLiteral("workspace.setLayout"));
        // The row is NOT deleted - nothing in the client ever deletes one - so
        // the closed pane's shell and scrollback stay recoverable. Only the
        // LAYOUT forgets it.
        QCOMPARE(compact(afterClose.value(QStringLiteral("params")).toObject()
                             .value(QStringLiteral("tree")).toObject()),
                 compact(survivor));
        respondResult(afterClose.value(QStringLiteral("id")).toInt(),
                      layoutRow(survivor));
    }

    // --- Client B, a different machine ------------------------------------
    // A fresh settings file IS the second machine: the slot counter lives there
    // and is client-local, which is exactly why it cannot guard identity.
    m_settingsDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_settingsDir->isValid());
    delete m_uiState;
    m_uiState = new UiStateStore(settingsPath());

    SessionLayouts clientB(m_db, m_uiState);
    clientB.setServerId(QStringLiteral("srv-1"));
    completeLoad(clientB, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))), layoutRow(survivor));

    // B has no memory of the closed pane and the tree does not mention it, so
    // it hands out the very label A's surviving row still wears.
    QCOMPARE(clientB.splitPane(QStringLiteral("terminal"),
                               QStringLiteral("terminal-1"),
                               QStringLiteral("horizontal")),
             QStringLiteral("terminal-2"));

    // And that is now harmless, because the label is not what is asked for. The
    // pane MINTS a row. It does not resolve (devSession, "terminal-2"), which is
    // the call that would have returned A's closed row.
    const QJsonObject mint = nextRequest();
    QCOMPARE(mint.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.createTerminalPane"));
    QVERIFY(mint.value(QStringLiteral("method")).toString()
            != QStringLiteral("workspace.resolveTerminalPane"));
    respondResult(mint.value(QStringLiteral("id")).toInt(),
                  QJsonObject{{"id", "row-B-new"},
                              {"serverId", "srv-1"},
                              {"devSessionId", "s1"},
                              {"name", "terminal-2"},
                              {"tmuxTarget", "ch_s1_row-B-new"},
                              {"position", 1}});

    // A DIFFERENT row, and therefore a different tmux target: the new pane
    // cannot be attached to the shell A left running.
    const QJsonObject expected =
        split(QStringLiteral("horizontal"),
              {survivor,
               terminalLeaf(QStringLiteral("terminal-2"), QStringLiteral("row-B-new"))},
              {1, 1});
    QTRY_COMPARE(compact(asObject(clientB.terminalTree())), compact(expected));
    const QString newRow = asObject(clientB.terminalTree())
                               .value(QStringLiteral("children")).toArray().at(1)
                               .toObject().value(QStringLiteral("terminalPaneId")).toString();
    QCOMPARE(newRow, QStringLiteral("row-B-new"));
    QVERIFY(newRow != QStringLiteral("row-A-terminal-2"));
    // Same label, different identity - which is the whole point.
    QCOMPARE(asObject(clientB.terminalTree())
                 .value(QStringLiteral("children")).toArray().at(1)
                 .toObject().value(QStringLiteral("paneId")).toString(),
             closedRow.value(QStringLiteral("paneId")).toString());

    const QJsonObject write = nextRequest();
    QCOMPARE(write.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.setLayout"));
    QCOMPARE(compact(write.value(QStringLiteral("params")).toObject()
                         .value(QStringLiteral("tree")).toObject()),
             compact(expected));
}

// LIVE (docs/PLAN.md): the same flow against the REAL Node codeharbord reached
// over the real SSH fixture. Proves a layout split persists into the server's
// SQLite and comes back byte-identical to a freshly constructed bridge - the
// "relaunch and see the layout restored" half of the cold-start walkthrough.
void TstSessionLayouts::liveLayoutRoundTripOverSsh()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH not set; live SSH gate skipped");

    SshConnectionPool pool;
    CodeharbordClient client;
    AgentStatusMonitor monitor;
    SessionBootstrap boot(&pool, &client, &monitor);
    // Reconnect would fight the teardown at the end of the case.
    boot.setReconnectEnabled(false);
    QVERIFY2(boot.connectAndWireFromEnvironment(),
             "connectAndWireFromEnvironment failed");

    WorkspaceDb db(&client);
    const QString serverId =
        QStringLiteral("tst-sl-%1").arg(QCoreApplication::applicationPid());

    // A group and a Dev Session to hang the layouts off.
    std::optional<Group> group;
    std::optional<RpcError> groupErr;
    db.createGroup(CreateGroupParams{.serverId = ServerId{serverId},
                                     .name = QStringLiteral("tst_sessionlayouts")},
                   [&](std::optional<Group> g, std::optional<RpcError> e) {
                       group = g;
                       groupErr = e;
                   });
    QTRY_VERIFY_WITH_TIMEOUT(group.has_value() || groupErr.has_value(), 15000);
    QVERIFY2(!groupErr.has_value(), qPrintable(groupErr ? groupErr->message
                                                        : QString()));
    QVERIFY(group.has_value());

    std::optional<DevSession> session;
    std::optional<RpcError> sessionErr;
    db.createSession(
        CreateSessionParams{.serverId = ServerId{serverId},
                            .groupId = group->id,
                            .name = QStringLiteral("layout probe"),
                            .repositoryRoot = QStringLiteral("/tmp")},
        [&](std::optional<DevSession> s, std::optional<RpcError> e) {
            session = s;
            sessionErr = e;
        });
    QTRY_VERIFY_WITH_TIMEOUT(session.has_value() || sessionErr.has_value(), 15000);
    QVERIFY2(!sessionErr.has_value(), qPrintable(sessionErr ? sessionErr->message
                                                            : QString()));
    QVERIFY(session.has_value());
    const QString sessionId = session->id.value;

    SessionLayouts layouts(&db, m_uiState);
    layouts.setServerId(serverId);
    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);
    QSignalSpy loadedSpy(&layouts, &SessionLayouts::loaded);

    // A brand new Dev Session has no persisted layout: the region defaults,
    // which the load SEEDS into the server's database.
    layouts.load(sessionId);
    QTRY_VERIFY_WITH_TIMEOUT(loadedSpy.count() == 1, 15000);
    QCOMPARE(errorSpy.count(), 0);
    // The seeded terminal default: two stacked panes, each bound to a
    // `terminal_panes` row the SERVER minted. The ids are UUIDs codeharbord
    // chose, so the literal pins everything except them and they get their own
    // assertions below.
    const QJsonObject stacked = split(QStringLiteral("vertical"),
                                      {leaf(QStringLiteral("terminal-1")),
                                       leaf(QStringLiteral("terminal-2"))},
                                      {1, 1});
    QCOMPARE(compact(asObject(layouts.viewerTree())),
             compact(leaf(QStringLiteral("viewer-1"))));
    // The default is published before its rows are minted, so wait for the ids
    // rather than racing them.
    QTRY_VERIFY_WITH_TIMEOUT(!terminalRowIds(asObject(layouts.terminalTree()))
                                  .contains(QString()),
                             15000);
    QCOMPARE(compact(withoutRowIds(asObject(layouts.terminalTree()))),
             compact(stacked));
    const QStringList seededIds = terminalRowIds(asObject(layouts.terminalTree()));
    QCOMPARE(seededIds.size(), 2);
    // Two panes, two rows: a shared id would be two panes on ONE remote shell.
    QVERIFY(seededIds.at(0) != seededIds.at(1));

    // The seeded default really landed in the server's SQLite: a fresh bridge
    // (which is what a relaunched app is) reads the two stacked terminals back
    // instead of re-deriving them.
    SessionLayouts afterSeed(&db, m_uiState);
    afterSeed.setServerId(serverId);
    QSignalSpy afterSeedSpy(&afterSeed, &SessionLayouts::loaded);
    afterSeed.load(sessionId);
    QTRY_VERIFY_WITH_TIMEOUT(afterSeedSpy.count() == 1, 15000);
    QCOMPARE(compact(withoutRowIds(asObject(afterSeed.terminalTree()))),
             compact(stacked));
    // THE assertion this case exists for now: the ids come back IDENTICAL. The
    // relaunched app does not merely see two panes in the right shape, it sees
    // the same two terminals - the server-minted identity is what persisted,
    // and nothing was re-derived or re-minted on the way back. A fresh bridge
    // must also not mint anything of its own here: these leaves already have
    // ids, so it has nothing to create.
    QCOMPARE(terminalRowIds(asObject(afterSeed.terminalTree())), seededIds);

    // Split both regions; each split is a real workspace.setLayout write.
    QCOMPARE(layouts.splitPane(QStringLiteral("viewer"),
                               QStringLiteral("viewer-1"),
                               QStringLiteral("vertical")),
             QStringLiteral("viewer-2"));
    // terminal-1 and terminal-2 are the default panes, so the new one is 3.
    QCOMPARE(layouts.splitPane(QStringLiteral("terminal"),
                               QStringLiteral("terminal-1"),
                               QStringLiteral("horizontal")),
             QStringLiteral("terminal-3"));
    // The new leaf is published at once and its row is minted behind it; the
    // layout write is deliberately held back until the id lands, so waiting for
    // three bound leaves is also what makes the reload below deterministic.
    QTRY_VERIFY_WITH_TIMEOUT(terminalRowIds(asObject(layouts.terminalTree())).size() == 3
                                 && !terminalRowIds(asObject(layouts.terminalTree()))
                                         .contains(QString()),
                             15000);
    layouts.setRatios(QStringLiteral("viewer"), {}, {2.0, 3.0});
    const QByteArray viewerBefore = compact(asObject(layouts.viewerTree()));
    const QByteArray terminalBefore = compact(asObject(layouts.terminalTree()));
    // The split minted a THIRD row rather than reusing either default's.
    const QStringList afterSplitIds = terminalRowIds(asObject(layouts.terminalTree()));
    QCOMPARE(QSet<QString>(afterSplitIds.begin(), afterSplitIds.end()).size(), 3);

    // A fresh bridge stands in for a relaunched app. codeharbord serves the
    // stdio stream in order, so the writes above are already committed.
    SessionLayouts reloaded(&db, m_uiState);
    reloaded.setServerId(serverId);
    QSignalSpy reloadedErrorSpy(&reloaded, &SessionLayouts::error);
    QSignalSpy reloadedSpy(&reloaded, &SessionLayouts::loaded);
    reloaded.load(sessionId);
    QTRY_VERIFY_WITH_TIMEOUT(reloadedSpy.count() == 1, 15000);
    QCOMPARE(reloadedErrorSpy.count(), 0);
    QCOMPARE(compact(asObject(reloaded.viewerTree())), viewerBefore);
    QCOMPARE(compact(asObject(reloaded.terminalTree())), terminalBefore);
    QCOMPARE(errorSpy.count(), 0);

    // Leave the shared fixture database as we found it.
    bool cleaned = false;
    db.deleteGroup(group->id, [&](std::optional<RpcError>) { cleaned = true; });
    QTRY_VERIFY_WITH_TIMEOUT(cleaned, 15000);
    boot.disconnectSession();
}

QTEST_GUILESS_MAIN(TstSessionLayouts)
#include "tst_sessionlayouts.moc"
