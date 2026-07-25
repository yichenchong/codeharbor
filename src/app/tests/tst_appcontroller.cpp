#include <QtTest/QtTest>

#include <QTemporaryDir>
#include <QPair>
#include <QByteArray>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>

#include <cstring>

#include "AppController.h"
#include "UiStateStore.h"
#include "WorkspaceDb.h"
#include "WorkspaceTypes.h"
#include "CodeharbordClient.h"
#include "SessionsModel.h"
#include "AgentStatusMonitor.h"
#include "AgentEvent.h"
#include "SessionState.h"

using namespace ch;

namespace {

// Minimal in-process QIODevice standing in for the RPC transport. The app test
// target deliberately does not link Qt6::Network, so QLocalSocket is out; this
// captures the client's writes verbatim (takeSent) and injects server->client
// frames via deliver(). Opened Unbuffered so the client's write() reaches
// writeData() immediately and readyRead dispatch is synchronous (no event
// loop), letting a test control response ordering exactly.
class FakeTransport : public QIODevice {
public:
    explicit FakeTransport(QObject* parent = nullptr) : QIODevice(parent)
    {
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return m_incoming.size() + QIODevice::bytesAvailable();
    }

    // Inject one server->client frame and dispatch it synchronously.
    void deliver(const QByteArray& frame)
    {
        m_incoming.append(frame);
        emit readyRead();
    }

    // Consume everything the client has written since the last call.
    QByteArray takeSent()
    {
        const QByteArray sent = m_sent;
        m_sent.clear();
        return sent;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_incoming.size());
        if (n > 0) {
            std::memcpy(data, m_incoming.constData(), static_cast<size_t>(n));
            m_incoming.remove(0, n);
        }
        return n;
    }

    qint64 writeData(const char* data, qint64 len) override
    {
        m_sent.append(data, len);
        return len;
    }

private:
    QByteArray m_incoming;
    QByteArray m_sent;
};

// Parse the single JSON-RPC request the client just wrote.
QJsonObject takeRequest(FakeTransport& transport)
{
    const QByteArray sent = transport.takeSent();
    const qsizetype newline = sent.indexOf('\n');
    const QByteArray line = newline >= 0 ? sent.left(newline) : sent;
    return QJsonDocument::fromJson(line).object();
}

// A workspace.list success frame carrying exactly one group of the given name.
QByteArray listResultFrame(int id, const QString& groupName)
{
    const QJsonArray groups{QJsonObject{{"id", groupName}, {"name", groupName}}};
    const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", id}, {"result", groups}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// A workspace.list success frame carrying one group with one session that owns a
// single terminal pane, so the AppController caches terminalPanes that
// rebuildRows() can merge live agent state onto.
QByteArray listWithTerminalFrame(int id, const QString& groupName,
                                 const QString& sessionId,
                                 const QString& terminalId)
{
    const QJsonObject terminal{{"id", terminalId}, {"devSessionId", sessionId}};
    const QJsonObject session{{"id", sessionId},
                              {"name", sessionId},
                              {"terminalPanes", QJsonArray{terminal}}};
    const QJsonObject group{{"id", groupName},
                            {"name", groupName},
                            {"sessions", QJsonArray{session}}};
    const QJsonObject resp{{"jsonrpc", "2.0"},
                           {"id", id},
                           {"result", QJsonArray{group}}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// One framed (newline-terminated) AgentEvent JSONL line for the monitor.
QByteArray agentEventLine(const QString& state, const QString& dev,
                          const QString& term)
{
    const QJsonObject o{{"version", 1},
                        {"timestamp", "2026-07-25T00:00:00.000Z"},
                        {"harness", "generic"},
                        {"devSessionId", dev},
                        {"terminalId", term},
                        {"state", state},
                        {"event", "tick"}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

// A JSON-RPC error frame for the given id.
QByteArray errorFrame(int id, int code, const QString& message)
{
    const QJsonObject err{{"code", code}, {"message", message}};
    const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", id}, {"error", err}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace

class TstAppController : public QObject {
    Q_OBJECT

private slots:
    void toGroupRowsMapsNestedNodes();
    void toGroupRowsEmptyIsEmpty();
    void uiStateStorePersistsAcrossInstances();
    void uiStateStoreDocumentedDefaults();
    void toGroupRowsSubtitleHandlesTrailingSlashAndEmpty();
    void uiStateStoreDistinctAndSpecialIds();
    void uiStateStoreRegionWidthsPersistWithoutPerCallSync();
    void setServerIdRefreshesForNewServer();
    void setServerIdUnchangedDoesNotRefresh();
    void refreshUpdatesModelAndEmitsRefreshed();
    void staleRefreshResultDoesNotClobberNewer();
    void refreshErrorEmitsErrorVerbatim();
    void mutationSuccessChainsRefresh();
    void refreshResultAfterControllerDestroyedIsNoop();
    void agentMonitorMergesStateIntoSidebar();
    void refreshDoesNotWipeAgentDerivedState();
};

// Two GroupNodes with sessions map to GroupRows preserving order, with the
// session subtitle set to the basename of repositoryRoot.
void TstAppController::toGroupRowsMapsNestedNodes()
{
    GroupNode g1;
    g1.group.id = GroupId{QStringLiteral("g1")};
    g1.group.name = QStringLiteral("Work");

    SessionNode s1;
    s1.session.id = DevSessionId{QStringLiteral("s1")};
    s1.session.name = QStringLiteral("codeharbor");
    s1.session.repositoryRoot = QStringLiteral("/home/u/proj");

    SessionNode s2;
    s2.session.id = DevSessionId{QStringLiteral("s2")};
    s2.session.name = QStringLiteral("docs");
    s2.session.repositoryRoot = QStringLiteral("/home/u/manual");
    g1.sessions = {s1, s2};

    GroupNode g2;
    g2.group.id = GroupId{QStringLiteral("g2")};
    g2.group.name = QStringLiteral("Personal");

    SessionNode s3;
    s3.session.id = DevSessionId{QStringLiteral("s3")};
    s3.session.name = QStringLiteral("dotfiles");
    s3.session.repositoryRoot = QStringLiteral("/home/u/config");
    g2.sessions = {s3};

    const QVector<GroupRow> rows = AppController::toGroupRows({g1, g2});

    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).group.name, QStringLiteral("Work"));
    QCOMPARE(rows.at(1).group.name, QStringLiteral("Personal"));

    QCOMPARE(rows.at(0).sessions.size(), 2);
    QCOMPARE(rows.at(0).sessions.at(0).session.name, QStringLiteral("codeharbor"));
    QCOMPARE(rows.at(0).sessions.at(0).subtitle, QStringLiteral("proj"));
    QCOMPARE(rows.at(0).sessions.at(1).subtitle, QStringLiteral("manual"));
    QVERIFY(rows.at(0).sessions.at(0).terminals.isEmpty());

    QCOMPARE(rows.at(1).sessions.size(), 1);
    QCOMPARE(rows.at(1).sessions.at(0).session.name, QStringLiteral("dotfiles"));
    QCOMPARE(rows.at(1).sessions.at(0).subtitle, QStringLiteral("config"));
}

void TstAppController::toGroupRowsEmptyIsEmpty()
{
    QVERIFY(AppController::toGroupRows({}).isEmpty());
}

// A fresh store over the same .ini file reads back exactly what a previous
// instance wrote — proving persistence via QSettings' flush on destruction
// (setRegionWidths no longer sync()s per call, to avoid handle-drag jank).
void TstAppController::uiStateStorePersistsAcrossInstances()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("ui.ini"));

    {
        UiStateStore store(iniPath);
        store.setRegionWidths(200, 0, 400);
        store.setSelectedPane(QStringLiteral("s1"), QStringLiteral("p9"));
    }

    UiStateStore reopened(iniPath);
    QCOMPARE(reopened.sidebarWidth(), 200);
    QCOMPARE(reopened.viewerWidth(), 0);
    QCOMPARE(reopened.terminalWidth(), 400);
    QCOMPARE(reopened.selectedPane(QStringLiteral("s1")), QStringLiteral("p9"));
}

// Documented defaults when nothing has been written.
void TstAppController::uiStateStoreDocumentedDefaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("empty.ini"));

    UiStateStore store(iniPath);
    QCOMPARE(store.sidebarWidth(), 260);
    QCOMPARE(store.viewerWidth(), 0);
    QCOMPARE(store.terminalWidth(), 520);
    QVERIFY(store.selectedPane(QStringLiteral("unknown")).isEmpty());
}

// repositoryRoot basenames: a trailing slash (or several), a "." segment, and
// relative paths still yield the final path component; an empty or root path
// yields an empty subtitle. Guards the QFileInfo trailing-slash pitfall
// (QFileInfo("/a/b/").fileName() == "").
void TstAppController::toGroupRowsSubtitleHandlesTrailingSlashAndEmpty()
{
    GroupNode g;
    g.group.id = GroupId{QStringLiteral("g")};

    const QVector<QPair<QString, QString>> cases = {
        {QStringLiteral("/home/u/proj/"), QStringLiteral("proj")},
        {QStringLiteral("/home/u/proj//"), QStringLiteral("proj")},
        {QStringLiteral("/home/u/./proj"), QStringLiteral("proj")},
        {QStringLiteral("relative/"), QStringLiteral("relative")},
        {QString(), QString()},
        {QStringLiteral("/"), QString()},
    };
    for (const auto& c : cases) {
        SessionNode s;
        s.session.id = DevSessionId{c.first};
        s.session.repositoryRoot = c.first;
        g.sessions.push_back(s);
    }

    const QVector<GroupRow> rows = AppController::toGroupRows({g});
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).sessions.size(), cases.size());
    for (qsizetype i = 0; i < cases.size(); ++i)
        QCOMPARE(rows.at(0).sessions.at(i).subtitle, cases.at(i).second);
}

// Distinct devSessionIds address independent panes (no key collision), and ids
// containing separator/special characters round-trip intact across a reopen.
void TstAppController::uiStateStoreDistinctAndSpecialIds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("panes.ini"));

    {
        UiStateStore store(iniPath);
        store.setSelectedPane(QStringLiteral("s1"), QStringLiteral("viewer"));
        store.setSelectedPane(QStringLiteral("s2"), QStringLiteral("terminal"));
        // Separator/space-bearing ids must not collide or corrupt the key.
        store.setSelectedPane(QStringLiteral("srv/grp:has space"),
                              QStringLiteral("editor"));
    }

    UiStateStore reopened(iniPath);
    QCOMPARE(reopened.selectedPane(QStringLiteral("s1")), QStringLiteral("viewer"));
    QCOMPARE(reopened.selectedPane(QStringLiteral("s2")), QStringLiteral("terminal"));
    QCOMPARE(reopened.selectedPane(QStringLiteral("srv/grp:has space")),
             QStringLiteral("editor"));
}

// setRegionWidths no longer calls QSettings::sync() on every invocation (a
// handle drag fires it repeatedly; a synchronous disk write per pixel caused
// jank). Simulate a drag with many writes, then prove the final values still
// persist to a fresh instance via the destructor flush — no explicit sync.
void TstAppController::uiStateStoreRegionWidthsPersistWithoutPerCallSync()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("drag.ini"));

    {
        UiStateStore store(iniPath);
        // Rapid intermediate writes, as during a handle drag.
        for (int w = 180; w <= 300; ++w)
            store.setRegionWidths(w, 0, 640 - w);
        // Final settled widths.
        store.setRegionWidths(300, 0, 340);
        // No explicit sync() here: the destructor at end of scope flushes.
    }

    UiStateStore reopened(iniPath);
    QCOMPARE(reopened.sidebarWidth(), 300);
    QCOMPARE(reopened.viewerWidth(), 0);
    QCOMPARE(reopened.terminalWidth(), 340);
}

// setServerId to a new value must reload the sidebar from that server: nothing
// else re-drives refresh() on a server change, so the property setter must.
void TstAppController::setServerIdRefreshesForNewServer()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.setServerId(QStringLiteral("srv-x"));

    const QJsonObject req = takeRequest(transport);
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));
    QCOMPARE(req.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-x"));

    QSignalSpy refreshedSpy(&controller, &AppController::refreshed);
    transport.deliver(listResultFrame(req.value(QStringLiteral("id")).toInt(),
                                      QStringLiteral("Alpha")));

    QCOMPARE(refreshedSpy.count(), 1);
    SessionsModel* model = controller.sessionsModel();
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->data(model->index(0, 0), SessionsModel::NameRole).toString(),
             QStringLiteral("Alpha"));
}

// Re-setting the current serverId is a no-op: no serverIdChanged, no refresh.
void TstAppController::setServerIdUnchangedDoesNotRefresh()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.setServerId(QStringLiteral("srv-x"));
    QVERIFY(!transport.takeSent().isEmpty()); // the first set drove a refresh

    controller.setServerId(QStringLiteral("srv-x"));
    QVERIFY(transport.takeSent().isEmpty()); // unchanged -> no second refresh
}

// A plain refresh() maps the server tree into the model and signals refreshed().
void TstAppController::refreshUpdatesModelAndEmitsRefreshed()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    QSignalSpy refreshedSpy(&controller, &AppController::refreshed);
    controller.refresh();
    transport.deliver(listResultFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("Only")));

    QCOMPARE(refreshedSpy.count(), 1);
    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
}

// Concurrent mutations chain several refreshes; the client routes responses by
// id, so replies can arrive out of order. The newest refresh must win no matter
// the arrival order — here the stale reply is delivered LAST and must be dropped
// (without the generation guard it would clobber the model with stale rows).
void TstAppController::staleRefreshResultDoesNotClobberNewer()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.refresh();
    const int firstId =
        takeRequest(transport).value(QStringLiteral("id")).toInt();
    controller.refresh();
    const int secondId =
        takeRequest(transport).value(QStringLiteral("id")).toInt();
    QVERIFY(firstId != secondId);

    // Deliver the newer request's result first, then the older/stale one.
    transport.deliver(listResultFrame(secondId, QStringLiteral("Fresh")));
    transport.deliver(listResultFrame(firstId, QStringLiteral("Stale")));

    SessionsModel* model = controller.sessionsModel();
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->data(model->index(0, 0), SessionsModel::NameRole).toString(),
             QStringLiteral("Fresh"));
}

// A refresh RpcError is forwarded verbatim via error() and leaves the model as-is.
void TstAppController::refreshErrorEmitsErrorVerbatim()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    QSignalSpy errorSpy(&controller, &AppController::error);
    controller.refresh();
    const int id = takeRequest(transport).value(QStringLiteral("id")).toInt();
    transport.deliver(errorFrame(id, -32000, QStringLiteral("kaboom")));

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toString(), QStringLiteral("kaboom"));
    QCOMPARE(controller.sessionsModel()->rowCount(), 0);
}

// A successful mutation chains an authoritative refresh that updates the model.
void TstAppController::mutationSuccessChainsRefresh()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.createGroup(QStringLiteral("New"));
    const QJsonObject createReq = takeRequest(transport);
    QCOMPARE(createReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.createGroup"));

    // Ack the create; the controller must then chain a workspace.list refresh.
    const QJsonObject created{{"id", "g1"}, {"name", "New"}};
    const QJsonObject ack{{"jsonrpc", "2.0"},
                          {"id", createReq.value(QStringLiteral("id")).toInt()},
                          {"result", created}};
    transport.deliver(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n');

    const QJsonObject listReq = takeRequest(transport);
    QCOMPARE(listReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));
    transport.deliver(listResultFrame(
        listReq.value(QStringLiteral("id")).toInt(), QStringLiteral("New")));

    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
}

// A late response after the controller is destroyed must be a no-op: the shared
// client keeps the pending callback alive past our lifetime, and the QPointer
// guard on every callback makes the delayed dispatch safe (no use-after-free).
void TstAppController::refreshResultAfterControllerDestroyedIsNoop()
{
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);

    int pendingId = 0;
    {
        AppController controller(&client);
        controller.refresh();
        pendingId =
            takeRequest(transport).value(QStringLiteral("id")).toInt();
        // controller goes out of scope here with the list request pending.
    }

    // Must not touch the destroyed controller.
    transport.deliver(listResultFrame(pendingId, QStringLiteral("Late")));
    QVERIFY(true); // reaching here without a crash is the assertion
}

// A live agent state fed to the monitor must surface in the sidebar: after a
// refresh populates a session with a terminal pane, an agentStateChanged event
// re-derives the row and its aggregate reflects the new AgentState. Before any
// event the terminal's agent is Unknown and its connection defaults to
// Unloaded, so the aggregate is Disconnected.
void TstAppController::agentMonitorMergesStateIntoSidebar()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    // A real monitor over its own in-process transport (no SSH needed).
    FakeTransport agentTransport;
    AgentStatusMonitor monitor;
    monitor.setTransport(&agentTransport);
    controller.setAgentMonitor(&monitor);

    controller.refresh();
    transport.deliver(listWithTerminalFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("G"), QStringLiteral("sess-1"), QStringLiteral("term-1")));

    SessionsModel* model = controller.sessionsModel();
    auto sessionState = [model]() {
        const QModelIndex group = model->index(0, 0);
        const QModelIndex session = model->index(0, 0, group);
        return model->data(session, SessionsModel::RowStateRole).toInt();
    };
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Disconnected));

    // Feed a running state for that (session, terminal); the monitor's
    // agentStateChanged drives rebuildRows() and the badge appears.
    agentTransport.deliver(agentEventLine(QStringLiteral("running"),
                                          QStringLiteral("sess-1"),
                                          QStringLiteral("term-1")));
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Running));
}

// A subsequent refresh with the SAME tree must not wipe the agent-derived
// badge: rebuildRows() always re-reads the monitor (the source of truth), so
// the row state persists across a workspace refresh.
void TstAppController::refreshDoesNotWipeAgentDerivedState()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    FakeTransport agentTransport;
    AgentStatusMonitor monitor;
    monitor.setTransport(&agentTransport);
    controller.setAgentMonitor(&monitor);

    controller.refresh();
    transport.deliver(listWithTerminalFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("G"), QStringLiteral("sess-1"), QStringLiteral("term-1")));

    agentTransport.deliver(agentEventLine(QStringLiteral("waiting_input"),
                                          QStringLiteral("sess-1"),
                                          QStringLiteral("term-1")));

    SessionsModel* model = controller.sessionsModel();
    auto sessionState = [model]() {
        const QModelIndex group = model->index(0, 0);
        const QModelIndex session = model->index(0, 0, group);
        return model->data(session, SessionsModel::RowStateRole).toInt();
    };
    QCOMPARE(sessionState(),
             static_cast<int>(SessionRowState::WaitingForInput));

    // Another full refresh with an identical tree; without re-reading the
    // monitor this would reset terminals to empty and wipe the badge.
    controller.refresh();
    transport.deliver(listWithTerminalFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("G"), QStringLiteral("sess-1"), QStringLiteral("term-1")));

    QCOMPARE(sessionState(),
             static_cast<int>(SessionRowState::WaitingForInput));
}

QTEST_GUILESS_MAIN(TstAppController)
#include "tst_appcontroller.moc"
