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
    void rpcErrorSurfacesWithoutCorruptingTree();
    void invalidSavedTreeIsRejected();
    void publishedTreeIsConsumableFromQml();
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
    QByteArray m_rxBuffer;                // holds frames read but not yet popped
    static int s_seq;
};

int TstSessionLayouts::s_seq = 0;

void TstSessionLayouts::init()
{
    m_client = new CodeharbordClient;
    m_db = new WorkspaceDb(m_client);
    m_rxBuffer.clear();
}

void TstSessionLayouts::cleanup()
{
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
        if (m_serverSide->bytesAvailable() > 0
            || m_serverSide->waitForReadyRead(50))
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
    // region default and immediately persists it, so two stacked terminals
    // survive a restart instead of being re-derived until something else saves a
    // tree. Answer those writes here; the case that asserts on them is
    // terminalDefaultIsTwoStackedPanesAndIsPersisted().
    int seeds = 0;
    if (viewerTree.isNull())
        ++seeds;
    if (terminalTree.isNull())
        ++seeds;
    for (int i = 0; i < seeds; ++i) {
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
    SessionLayouts layouts(m_db);
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
    QCOMPARE(compact(asObject(layouts.terminalTree())),
             compact(split(QStringLiteral("vertical"),
                           {leaf(QStringLiteral("terminal-1")),
                            leaf(QStringLiteral("terminal-2"))},
                           {1, 1})));
    QCOMPARE(viewerSpy.count(), 1);
    QCOMPARE(terminalSpy.count(), 1);

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
    SessionLayouts layouts(m_db);
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
    const QJsonObject stacked = split(QStringLiteral("vertical"),
                                      {leaf(QStringLiteral("terminal-1")),
                                       leaf(QStringLiteral("terminal-2"))},
                                      {1, 1});
    QCOMPARE(compact(asObject(layouts.terminalTree())), compact(stacked));

    // Both defaults are WRITTEN, not merely held in memory. This is the whole
    // point: without the write, the first thing that saves a tree (a QML
    // fallback node, a ratio drag on the other region) decides what the session
    // looks like after a restart.
    QJsonObject terminalSeed;
    int seedCount = 0;
    for (int i = 0; i < 2; ++i) {
        const QJsonObject seed = nextRequest();
        QCOMPARE(seed.value(QStringLiteral("method")).toString(),
                 QStringLiteral("workspace.setLayout"));
        const QJsonObject params = seed.value(QStringLiteral("params")).toObject();
        QCOMPARE(params.value(QStringLiteral("serverId")).toString(),
                 QStringLiteral("srv-1"));
        QCOMPARE(params.value(QStringLiteral("devSessionId")).toString(),
                 QStringLiteral("s1"));
        if (params.value(QStringLiteral("region")).toString()
            == QStringLiteral("terminal"))
            terminalSeed = params.value(QStringLiteral("tree")).toObject();
        ++seedCount;
        respondResult(seed.value(QStringLiteral("id")).toInt(),
                      layoutRow(params.value(QStringLiteral("tree")).toObject()));
    }
    QCOMPARE(seedCount, 2);
    QCOMPARE(compact(terminalSeed), compact(stacked));
    QCOMPARE(errorSpy.count(), 0);

    // Idempotent: the seed created the row, so reopening the session loads it
    // and writes nothing at all.
    SessionLayouts reopened(m_db);
    reopened.setServerId(QStringLiteral("srv-1"));
    completeLoad(reopened, QStringLiteral("s1"),
                 layoutRow(leaf(QStringLiteral("viewer-1"))),
                 layoutRow(stacked));
    QCOMPARE(compact(asObject(reopened.terminalTree())), compact(stacked));
    QVERIFY(noMoreRequests());

    // Pane numbering continues past the second default pane instead of minting
    // a "terminal-2" that already exists.
    QCOMPARE(reopened.splitPane(QStringLiteral("terminal"),
                                QStringLiteral("terminal-2"),
                                QStringLiteral("horizontal")),
             QStringLiteral("terminal-3"));
}

void TstSessionLayouts::splitSerializesExactSetLayoutParams()
{
    makePair();
    SessionLayouts layouts(m_db);
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
    SessionLayouts layouts(m_db);
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
    SessionLayouts reloaded(m_db);
    reloaded.setServerId(QStringLiteral("srv-1"));
    completeLoad(reloaded, QStringLiteral("s1"), layoutRow(persisted),
                 QJsonValue(QJsonValue::Null));

    QCOMPARE(compact(asObject(reloaded.viewerTree())), beforeReload);
    QCOMPARE(compact(persisted), beforeReload);
}

void TstSessionLayouts::closeCollapsesBranchIntoSurvivor()
{
    makePair();
    SessionLayouts layouts(m_db);
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
    SessionLayouts layouts(m_db);
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
    // than branching it against a permanently dead half. The suffix restarts at
    // 1 because no "terminal-<n>" leaf is left in the tree.
    QCOMPARE(layouts.splitPane(QStringLiteral("terminal"), QString(),
                               QStringLiteral("horizontal")),
             QStringLiteral("terminal-1"));
    QCOMPARE(compact(asObject(layouts.terminalTree())),
             compact(leaf(QStringLiteral("terminal-1"))));
    const QJsonObject refillReq = nextRequest();
    QCOMPARE(compact(refillReq.value(QStringLiteral("params")).toObject()
                         .value(QStringLiteral("tree")).toObject()),
             compact(leaf(QStringLiteral("terminal-1"))));
}

void TstSessionLayouts::ratiosPersistedAtNestedPath()
{
    makePair();
    SessionLayouts layouts(m_db);
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
    SessionLayouts layouts(m_db);
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
    SessionLayouts layouts(m_db);
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
    QCOMPARE(compact(asObject(layouts.terminalTree())),
             compact(leaf(QStringLiteral("fresh-2"))));
}

void TstSessionLayouts::rpcErrorSurfacesWithoutCorruptingTree()
{
    makePair();
    SessionLayouts layouts(m_db);
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
    SessionLayouts layouts(m_db);
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
    SessionLayouts layouts(m_db);
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
    SessionLayouts empty(m_db);
    probe->setProperty("node", empty.viewerTree());
    QVERIFY(probe->property("node").isNull());
    QCOMPARE(probe->property("rootIsLeaf").toBool(), true);
    QCOMPARE(probe->property("rootPaneId").toString(), QString());
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

    SessionLayouts layouts(&db);
    layouts.setServerId(serverId);
    QSignalSpy errorSpy(&layouts, &SessionLayouts::error);
    QSignalSpy loadedSpy(&layouts, &SessionLayouts::loaded);

    // A brand new Dev Session has no persisted layout: the region defaults,
    // which the load SEEDS into the server's database.
    layouts.load(sessionId);
    QTRY_VERIFY_WITH_TIMEOUT(loadedSpy.count() == 1, 15000);
    QCOMPARE(errorSpy.count(), 0);
    const QJsonObject stacked = split(QStringLiteral("vertical"),
                                      {leaf(QStringLiteral("terminal-1")),
                                       leaf(QStringLiteral("terminal-2"))},
                                      {1, 1});
    QCOMPARE(compact(asObject(layouts.viewerTree())),
             compact(leaf(QStringLiteral("viewer-1"))));
    QCOMPARE(compact(asObject(layouts.terminalTree())), compact(stacked));

    // The seeded default really landed in the server's SQLite: a fresh bridge
    // (which is what a relaunched app is) reads the two stacked terminals back
    // instead of re-deriving them.
    SessionLayouts afterSeed(&db);
    afterSeed.setServerId(serverId);
    QSignalSpy afterSeedSpy(&afterSeed, &SessionLayouts::loaded);
    afterSeed.load(sessionId);
    QTRY_VERIFY_WITH_TIMEOUT(afterSeedSpy.count() == 1, 15000);
    QCOMPARE(compact(asObject(afterSeed.terminalTree())), compact(stacked));

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
    layouts.setRatios(QStringLiteral("viewer"), {}, {2.0, 3.0});
    const QByteArray viewerBefore = compact(asObject(layouts.viewerTree()));
    const QByteArray terminalBefore = compact(asObject(layouts.terminalTree()));

    // A fresh bridge stands in for a relaunched app. codeharbord serves the
    // stdio stream in order, so the writes above are already committed.
    SessionLayouts reloaded(&db);
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
