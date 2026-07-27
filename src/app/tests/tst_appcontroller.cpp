#include <QtTest/QtTest>

#include <QTemporaryDir>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QScopeGuard>
#include <QVariantMap>
#include <QPair>
#include <QByteArray>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QStandardPaths>

#include <cstring>
#include <functional>

#include "AppController.h"
#include "SessionLayouts.h"
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

// Every JSON-RPC request id the client has written since the last call, in
// order. activateSession() fires TWO getLayout requests through SessionLayouts,
// so takeRequest()'s first-line-only parse is not enough on its own.
QVector<int> takeRequestIds(FakeTransport& transport)
{
    QVector<int> ids;
    const QList<QByteArray> lines = transport.takeSent().split('\n');
    for (const QByteArray& line : lines) {
        if (line.trimmed().isEmpty())
            continue;
        ids.push_back(QJsonDocument::fromJson(line)
                          .object()
                          .value(QStringLiteral("id"))
                          .toInt());
    }
    return ids;
}

// A workspace.getLayout/setLayout success frame: a SessionLayout row whose tree
// is a single leaf (only "tree" is read back).
QByteArray layoutLeafFrame(int id, const QString& paneId)
{
    const QJsonObject tree{{"type", "leaf"}, {"paneId", paneId}};
    const QJsonObject row{{"id", "layout-1"}, {"tree", tree}};
    const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", id}, {"result", row}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// A `server.info` success frame.
QByteArray serverInfoFrame(int id, int schemaVersion, const QString& serverId,
                           const QString& version)
{
    QJsonObject info{{"name", "codeharbord"},
                     {"version", version},
                     {"schemaVersion", schemaVersion}};
    if (!serverId.isEmpty())
        info.insert(QStringLiteral("serverId"), serverId);
    const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", id}, {"result", info}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// A bootstrap whose handshake never leaves the process. connectPool() is where
// libssh would run the auth ladder, so `duringConnect` is the seam a test uses
// to be the pool: it fires exactly where SshConnectionPool::authenticate()
// would reach the credential callback AppController installed.
class FakeBootstrap : public SessionBootstrap {
public:
    using SessionBootstrap::SessionBootstrap;

    bool connectOk = false;
    int connectCalls = 0;
    std::function<void()> duringConnect;

    // adoptServerIdentity() hangs off wired(); a subclass may emit its own.
    void fireWired() { emit wired(); }

protected:
    bool probeEndpoint(const QString&, quint16, QString*) override
    {
        return true;
    }

    bool connectPool(const QString&, quint16, const QString&) override
    {
        ++connectCalls;
        if (duringConnect)
            duringConnect();
        return connectOk;
    }
};

// Everything AppController's connection surface needs, over throwaway paths.
struct ConnectFixture {
    QTemporaryDir dir;
    SshConnectionPool pool;
    CodeharbordClient client;
    AgentStatusMonitor monitor;
    FakeBootstrap boot{&pool, &client, &monitor};
    ServerProfiles profiles{dir.filePath(QStringLiteral("servers.ini"))};
    AppController controller{&client};
    QString profileId;

    ConnectFixture()
    {
        boot.setKnownHostsPath(dir.filePath(QStringLiteral("known_hosts")));
        controller.setConnection(&pool, &boot, &profiles, nullptr);
        profileId = profiles.addProfile(
            {{QStringLiteral("name"), QStringLiteral("box")},
             {QStringLiteral("host"), QStringLiteral("127.0.0.1")},
             {QStringLiteral("port"), 22},
             {QStringLiteral("user"), QStringLiteral("yichen")},
             {QStringLiteral("nodePath"), QStringLiteral("/usr/bin/node")},
             {QStringLiteral("repoRoot"), QStringLiteral("/srv/codeharbor")}});
    }

    // Every byte this fixture persists. The credential tests assert a secret
    // appears in none of it.
    QByteArray allPersistedBytes() const
    {
        QByteArray blob;
        QDirIterator it(dir.path(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile file(it.next());
            if (file.open(QIODevice::ReadOnly))
                blob += file.readAll();
        }
        return blob;
    }
};

} // namespace

class TstAppController : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
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
    void markSeenClearsFinishedUnseenBadge();
    void vanishedActiveSessionIsRetiredEverywhere();
    void staleOrFailedRefreshNeverRetiresActiveSession();
    void deletingActiveSessionRetiresItThroughChainedRefresh();
    void disconnectRetiresActiveSessionButStillRemembersIt();

    // Reaching a real server: the credential prompt (the pool's third auth rung
    // had no product-side answer) and the server-compatibility gate.
    void credentialCallbackIsInstalledAndParksInsteadOfBlocking();
    void submittedSecretIsSpentOnceAndNeverPersistedOrLogged();
    void cancellingTheCredentialPromptAbandonsTheAttemptCleanly();
    void serverOlderThanTheSchemaFloorIsRefusedWithBothVersions();
    void serverAtTheSchemaFloorIsAdoptedNormally();
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

// MANDATORY (markSeen semantics): a terminal reaching idle_unseen puts the row
// in FinishedUnseen (the blue "unseen completion" badge). Once the user views
// the session, markSeen(dev) clears the monitor's per-session unseen flag and
// its unseenChanged signal re-drives rebuildRows(). The monitor still reports
// the terminal's raw agent state as IdleUnseen, so without the AppController
// downgrade the row would stay stuck in FinishedUnseen and the badge would
// never clear. rebuildRows() must downgrade IdleUnseen -> Idle for the row when
// hasUnseen(dev) is false, so the FinishedUnseen badge is cleared. (The row
// falls to Disconnected here because the terminal's connection state — owned by
// the terminal workstream and merged separately — is Unloaded in this harness;
// the point under test is that the FinishedUnseen badge is gone.)
void TstAppController::markSeenClearsFinishedUnseenBadge()
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

    SessionsModel* model = controller.sessionsModel();
    auto sessionState = [model]() {
        const QModelIndex group = model->index(0, 0);
        const QModelIndex session = model->index(0, 0, group);
        return model->data(session, SessionsModel::RowStateRole).toInt();
    };

    // idle_unseen -> row FinishedUnseen (badge shown), session flagged unseen.
    agentTransport.deliver(agentEventLine(QStringLiteral("idle_unseen"),
                                          QStringLiteral("sess-1"),
                                          QStringLiteral("term-1")));
    QCOMPARE(sessionState(),
             static_cast<int>(SessionRowState::FinishedUnseen));
    QVERIFY(monitor.hasUnseen(QStringLiteral("sess-1")));

    // markSeen(dev) -> rebuild -> badge cleared. The monitor still holds the
    // terminal at IdleUnseen, but the row must no longer be FinishedUnseen.
    monitor.markSeen(QStringLiteral("sess-1"));
    QVERIFY(!monitor.hasUnseen(QStringLiteral("sess-1")));
    QVERIFY(sessionState() != static_cast<int>(SessionRowState::FinishedUnseen));
    QCOMPARE(sessionState(),
             static_cast<int>(SessionRowState::Disconnected));
    // The monitor's per-terminal raw state is unchanged (only the row is
    // downgraded); this is what proves the fix lives in rebuildRows, not the
    // monitor.
    QCOMPARE(monitor.stateFor(QStringLiteral("sess-1"), QStringLiteral("term-1")),
             static_cast<int>(AgentState::IdleUnseen));
}

// AppController's own UiStateStore is the REAL per-user QSettings (it is
// constructed with an empty ini path, exactly as main.cpp leaves it). The
// active-session cases below drive it for real, so redirect QSettings at the
// process level rather than writing into the developer's actual config.
void TstAppController::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

namespace {

// Shared setup for the active-session cases: a controller wired to a real
// SessionLayouts over its own WorkspaceDb (the main.cpp shape), serverId set
// BEFORE the transport so the setter's refresh() no-ops, with session "s1"
// active and BOTH region layouts loaded and editable.
struct ActiveSessionFixture {
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller{&client};
    SessionLayouts layouts{controller.workspaceDb()};

    ActiveSessionFixture()
    {
        controller.setConnection(nullptr, nullptr, nullptr, &layouts);
        controller.setServerId(QStringLiteral("srv"));
        // Hermetic: a previous run must not leave a remembered session behind.
        controller.uiState()->setActiveSession(QStringLiteral("srv"), QString());
        client.setTransport(&transport);
        transport.takeSent();
    }

    // Answer one workspace.list with a tree that holds group "g" + session "s1".
    void deliverTreeWithS1()
    {
        controller.refresh();
        transport.deliver(listWithTerminalFrame(
            takeRequest(transport).value(QStringLiteral("id")).toInt(),
            QStringLiteral("g"), QStringLiteral("s1"), QStringLiteral("t1")));
    }

    // Answer one workspace.list with group "g" and NO sessions: s1 is gone.
    void deliverTreeWithoutS1()
    {
        controller.refresh();
        transport.deliver(listResultFrame(
            takeRequest(transport).value(QStringLiteral("id")).toInt(),
            QStringLiteral("g")));
    }

    // Make s1 current and resolve both of its getLayout requests, so the
    // layouts are genuinely loaded (valid trees, canEdit() satisfied).
    void activateAndLoadS1()
    {
        controller.activateSession(QStringLiteral("s1"));
        const QVector<int> layoutIds = takeRequestIds(transport);
        for (int id : layoutIds)
            transport.deliver(layoutLeafFrame(id, QStringLiteral("viewer-1")));
    }
};

} // namespace

// The active Dev Session can be deleted out from under the shell by ANOTHER
// client: it simply stops appearing in the authoritative tree. Nothing else in
// the controller ever clears m_activeSessionId (restoreActiveSession only ever
// fills it in, and early-returns while it is non-empty), so before this it
// stayed "active" forever: the terminal region kept its dead devSessionId,
// SessionLayouts kept passing canEdit() and would keep writing
// workspace.setLayout rows for a Dev Session the server had deleted, and
// UiStateStore kept offering it to the next launch.
void TstAppController::vanishedActiveSessionIsRetiredEverywhere()
{
    ActiveSessionFixture f;
    f.deliverTreeWithS1();
    f.activateAndLoadS1();

    // Precondition: fully live. Layout edits for s1 are accepted and persisted.
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.layouts.devSessionId(), QStringLiteral("s1"));
    QVERIFY(!f.layouts.viewerTree().isNull());
    QVERIFY(!f.layouts.terminalTree().isNull());
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s1"));
    f.transport.takeSent();

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);
    f.deliverTreeWithoutS1();

    // Retired everywhere, and exactly once - not thrashed per refresh.
    QCOMPARE(f.controller.activeSessionId(), QString());
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(f.layouts.devSessionId(), QString());
    QVERIFY(f.layouts.viewerTree().isNull());
    QVERIFY(f.layouts.terminalTree().isNull());
    // Forgotten for THIS server only, so the next launch does not restore a
    // phantom; another server's memory is untouched.
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QString());

    // The corruption this guards: with no Dev Session selected, SessionLayouts
    // refuses the edit instead of writing setLayout under the dead id.
    QSignalSpy layoutErrorSpy(&f.layouts, &SessionLayouts::error);
    f.transport.takeSent();
    f.layouts.splitPane(QStringLiteral("viewer"), QStringLiteral("viewer-1"),
                        QStringLiteral("horizontal"));
    QCOMPARE(layoutErrorSpy.count(), 1);
    QVERIFY(f.transport.takeSent().isEmpty());

    // Idempotent: a second identical refresh is a no-op, no further signal.
    f.deliverTreeWithoutS1();
    QCOMPARE(activeSpy.count(), 1);
}

// The retirement above must fire ONLY on an authoritative answer. A superseded
// (stale-generation) list that happens to lack the session, and an RpcError -
// which means "we do not know", not "it is gone" - must both leave the active
// session completely alone. Getting this wrong turns a transient server hiccup
// into a silently closed Dev Session.
void TstAppController::staleOrFailedRefreshNeverRetiresActiveSession()
{
    ActiveSessionFixture f;
    f.deliverTreeWithS1();
    f.activateAndLoadS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    f.transport.takeSent();

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);

    // Two refreshes in flight; the OLDER one comes back without s1, after the
    // newer one already re-affirmed it. The generation guard must drop it
    // before it can retire anything.
    f.controller.refresh();
    const int staleId = takeRequest(f.transport).value(QStringLiteral("id")).toInt();
    f.controller.refresh();
    const int freshId = takeRequest(f.transport).value(QStringLiteral("id")).toInt();
    QVERIFY(staleId != freshId);
    f.transport.deliver(listWithTerminalFrame(freshId, QStringLiteral("g"),
                                              QStringLiteral("s1"),
                                              QStringLiteral("t1")));
    f.transport.deliver(listResultFrame(staleId, QStringLiteral("g")));

    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.layouts.devSessionId(), QStringLiteral("s1"));
    QCOMPARE(activeSpy.count(), 0);

    // An outright RPC failure is not evidence of deletion either.
    f.transport.takeSent();
    f.controller.refresh();
    f.transport.deliver(errorFrame(
        takeRequest(f.transport).value(QStringLiteral("id")).toInt(), -32000,
        QStringLiteral("workspace unavailable")));

    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.layouts.devSessionId(), QStringLiteral("s1"));
    QCOMPARE(activeSpy.count(), 0);
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s1"));
}

// The same retirement must happen on THIS client's own deletion, which reaches
// it by a different route: deleteSession chains refreshOnSuccess -> refresh(),
// so the tree that no longer holds the session is the chained one.
void TstAppController::deletingActiveSessionRetiresItThroughChainedRefresh()
{
    ActiveSessionFixture f;
    f.deliverTreeWithS1();
    f.activateAndLoadS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    f.transport.takeSent();

    f.controller.deleteSession(QStringLiteral("s1"));
    const int deleteId =
        takeRequest(f.transport).value(QStringLiteral("id")).toInt();
    const QJsonObject ack{{"jsonrpc", "2.0"}, {"id", deleteId}, {"result", true}};
    f.transport.deliver(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n');

    // The success chained a refresh; answer it with the post-delete tree.
    f.transport.deliver(listResultFrame(
        takeRequest(f.transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("g")));

    QCOMPARE(f.controller.activeSessionId(), QString());
    QCOMPARE(f.layouts.devSessionId(), QString());
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QString());
}

// Disconnect must land on a clear empty state, not a half-live shell still
// pointing at a Dev Session it can no longer reach. Leaving it active is not
// cosmetic: SessionLayouts keeps the devSessionId, keeps passing canEdit(),
// and a Split command after Disconnect mutates and republishes a tree whose
// setLayout fails - and since a reconnect never reloads a session that is
// still active, the next edit that DOES land writes that divergent tree over
// the real one. The session is unreachable, NOT gone, so unlike a deletion the
// remembered id must survive and be reopened by the reconnect.
void TstAppController::disconnectRetiresActiveSessionButStillRemembersIt()
{
    ActiveSessionFixture f;
    SshConnectionPool pool;
    AgentStatusMonitor monitor;
    SessionBootstrap bootstrap(&pool, &f.client, &monitor);
    f.controller.setConnection(&pool, &bootstrap, nullptr, &f.layouts);

    f.deliverTreeWithS1();
    f.activateAndLoadS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QVERIFY(!f.layouts.viewerTree().isNull());
    f.transport.takeSent();

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);
    f.controller.disconnectServer();

    QCOMPARE(f.controller.activeSessionId(), QString());
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(f.layouts.devSessionId(), QString());
    QVERIFY(f.layouts.viewerTree().isNull());
    QVERIFY(f.layouts.terminalTree().isNull());
    // The distinction from a deletion: the server still HAS this session, so
    // the next connect must reopen it.
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s1"));

    // A layout edit is now refused outright instead of diverging locally.
    QSignalSpy layoutErrorSpy(&f.layouts, &SessionLayouts::error);
    f.transport.takeSent();
    f.layouts.splitPane(QStringLiteral("viewer"), QStringLiteral("viewer-1"),
                        QStringLiteral("horizontal"));
    QCOMPARE(layoutErrorSpy.count(), 1);
    QVERIFY(f.transport.takeSent().isEmpty());

    // Reconnect: adoptServerIdentity re-drives refresh(), whose `refreshed`
    // runs restoreActiveSession. Because the id was cleared, it reopens s1 -
    // and reopening reloads BOTH regions from the server rather than reusing
    // the tree we were holding when the link went down.
    f.deliverTreeWithS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.layouts.devSessionId(), QStringLiteral("s1"));
    QCOMPARE(takeRequestIds(f.transport).size(), 2); // one getLayout per region
}

// SshConnectionPool authenticates agent -> default key -> credential callback,
// but setCredentialCallback() had NO non-test caller: a user with a
// passphrase-protected key, or no loaded agent (the stock desktop on Windows and
// macOS), dead-ended at "authentication failed" with nothing ever asking them
// for anything. This is that wiring, and it must not block: the pool calls the
// callback from inside the blocking libssh handshake on the GUI thread, so the
// attempt is REFUSED and the user asked afterwards — the same shape the
// host-key flow was rewritten into.
void TstAppController::credentialCallbackIsInstalledAndParksInsteadOfBlocking()
{
    ConnectFixture f;
    QVERIFY(!f.profileId.isEmpty());

    QVERIFY2(!f.pool.credentialCallback(),
             "nothing should be installed before a connect is started");

    bool asked = false;
    f.boot.duringConnect = [&f, &asked] {
        // Stand where SshConnectionPool::authenticate() stands.
        QVERIFY(f.pool.credentialCallback());
        asked = true;
        // An empty answer aborts THIS attempt, which is the whole point: the
        // handshake unwinds instead of a dialog being raised inside it.
        QCOMPARE(f.pool.credentialCallback()(QStringLiteral("yichen"),
                                             QStringLiteral("Password")),
                 QString());
    };

    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);
    QSignalSpy errorSpy(&f.controller, &AppController::error);
    f.controller.connectToProfile(f.profileId);

    QVERIFY(asked);
    QCOMPARE(promptSpy.count(), 1);
    QCOMPARE(promptSpy.at(0).at(0).toString(), QStringLiteral("yichen"));
    QCOMPARE(promptSpy.at(0).at(1).toString(), QStringLiteral("127.0.0.1"));
    QCOMPARE(promptSpy.at(0).at(2).toString(), QStringLiteral("Password"));
    QCOMPARE(f.controller.connectionState(), QStringLiteral("credential"));
    // Being asked for a password is not a fault; an error toast here would tell
    // the user something broke while the app is simply waiting on them.
    QCOMPARE(errorSpy.count(), 0);
}

// The secret is spent on exactly one attempt and then gone: not replayed by the
// reconnect ladder running through the same installed callback, not written to
// any file the app owns, and not logged. A passphrase landing in the config
// file would be a worse defect than the bug this fixes.
void TstAppController::submittedSecretIsSpentOnceAndNeverPersistedOrLogged()
{
    static const QString kSecret = QStringLiteral("correct-horse-battery-42");
    static QStringList captured;
    captured.clear();
    QtMessageHandler previous = qInstallMessageHandler(
        [](QtMsgType, const QMessageLogContext&, const QString& text) {
            captured << text;
        });
    const auto restoreHandler =
        qScopeGuard([previous] { qInstallMessageHandler(previous); });

    ConnectFixture f;
    f.boot.duringConnect = [&f] {
        f.pool.credentialCallback()(QStringLiteral("yichen"),
                                    QStringLiteral("Password"));
    };
    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 1);

    // The retry: this time the callback has the answer in hand.
    QStringList handedOver;
    f.boot.duringConnect = [&f, &handedOver] {
        handedOver << f.pool.credentialCallback()(QStringLiteral("yichen"),
                                                  QStringLiteral("Password"));
    };
    QSignalSpy errorSpy(&f.controller, &AppController::error);
    f.controller.submitCredential(kSecret);

    QCOMPARE(handedOver, QStringList{kSecret});

    // ONE SHOT. The callback outlives the attempt (SessionBootstrap's reconnect
    // ladder re-handshakes through it with nobody waiting), so a secret still
    // sitting in the capture would be replayed at whatever host it dials next.
    QCOMPARE(f.pool.credentialCallback()(QStringLiteral("yichen"),
                                         QStringLiteral("Password")),
             QString());
    // ...and that replay attempt must not arm a prompt nobody would answer.
    QCOMPARE(promptSpy.count(), 1);

    // A WRONG secret (connectOk stayed false) fails cleanly and says so, rather
    // than wedging the state machine or silently re-asking forever.
    QCOMPARE(f.controller.connectionState(), QStringLiteral("failed"));
    QCOMPARE(errorSpy.count(), 1);
    // Not wedged: the next connect really starts a new handshake.
    f.boot.duringConnect = {};
    f.boot.connectCalls = 0;
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(f.boot.connectCalls, 1);

    // Nothing the app persisted contains it — servers.ini above all, which is
    // the file a profile field would have landed in.
    const QByteArray persisted = f.allPersistedBytes();
    QVERIFY(!persisted.isEmpty());  // the profile store really was written
    QVERIFY2(!persisted.contains(kSecret.toUtf8()),
             "the secret reached a file on disk");
    // ...and no profile field carries it either, whatever the store looks like.
    const QVariantMap stored = f.profiles.profile(f.profileId);
    for (const QVariant& value : stored)
        QVERIFY(value.toString() != kSecret);

    for (const QString& line : captured)
        QVERIFY2(!line.contains(kSecret), qPrintable(line));
}

// Cancelling is an answer too: the parked attempt ends, nothing is retried, and
// the controller is left able to connect again.
void TstAppController::cancellingTheCredentialPromptAbandonsTheAttemptCleanly()
{
    ConnectFixture f;
    f.boot.duringConnect = [&f] {
        f.pool.credentialCallback()(QStringLiteral("yichen"),
                                    QStringLiteral("Password"));
    };
    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 1);

    const int callsBefore = f.boot.connectCalls;
    f.controller.submitCredential(QString());
    QCOMPARE(f.boot.connectCalls, callsBefore);  // no retry
    QCOMPARE(f.controller.connectionState(), QStringLiteral("disconnected"));

    // A stale sheet answering twice must not redial anything.
    f.controller.submitCredential(QStringLiteral("too-late"));
    QCOMPARE(f.boot.connectCalls, callsBefore);

    // And the next real connect is unaffected.
    f.boot.duringConnect = {};
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(f.boot.connectCalls, callsBefore + 1);
}

// ServerInfoResult::schemaVersion was parsed and never checked. A client one
// release ahead of its codeharbord got an empty serverId, keyed the workspace to
// "", and showed an EMPTY SIDEBAR over a healthy SSH session with no
// explanation. Version skew is the default state under manual deployment.
void TstAppController::serverOlderThanTheSchemaFloorIsRefusedWithBothVersions()
{
    ConnectFixture f;
    FakeTransport transport;
    f.client.setTransport(&transport);
    QSignalSpy errorSpy(&f.controller, &AppController::error);

    f.boot.fireWired();
    const QJsonObject request = takeRequest(transport);
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QStringLiteral("server.info"));

    // schema 3: the release before serverId existed, so it reports none.
    transport.deliver(serverInfoFrame(request.value(QStringLiteral("id")).toInt(),
                                      3, QString(), QStringLiteral("0.0.9")));

    QCOMPARE(errorSpy.count(), 1);
    const QString message = errorSpy.at(0).at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("3")), qPrintable(message));
    QVERIFY2(message.contains(QString::number(
                 AppController::kMinimumServerSchemaVersion)),
             qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("0.0.9")), qPrintable(message));

    // Refused, not silently continued: no identity adopted, and crucially no
    // workspace.list for the empty serverId — that call IS the empty sidebar.
    QCOMPARE(f.controller.serverId(), QString());
    QVERIFY(takeRequestIds(transport).isEmpty());

    // The link is dropped rather than left half-alive, one event-loop turn
    // later (we were inside the client's own response callback).
    QTRY_COMPARE(f.controller.connectionState(), QStringLiteral("failed"));
    QCOMPARE(f.controller.connectionError(), message);
}

// Control: a server AT the floor is adopted and drives the sidebar as before,
// so the gate refuses old servers rather than all of them.
void TstAppController::serverAtTheSchemaFloorIsAdoptedNormally()
{
    ConnectFixture f;
    FakeTransport transport;
    f.client.setTransport(&transport);
    QSignalSpy errorSpy(&f.controller, &AppController::error);

    f.boot.fireWired();
    const QJsonObject request = takeRequest(transport);
    transport.deliver(
        serverInfoFrame(request.value(QStringLiteral("id")).toInt(),
                        AppController::kMinimumServerSchemaVersion,
                        QStringLiteral("srv-1"), QStringLiteral("1.0.0")));

    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(f.controller.serverId(), QStringLiteral("srv-1"));
    const QJsonObject listRequest = takeRequest(transport);
    QCOMPARE(listRequest.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));
}

QTEST_GUILESS_MAIN(TstAppController)
#include "tst_appcontroller.moc"
