// LIVE gate for the application shell (SPEC 4.1/4.2, workstream U).
//
// Everything here runs against the real thing: a real sshd, a real remote
// `codeharbord` speaking JSON-RPC over a real SSH channel, the real SQLite
// workspace database on the host, the real `codeharbor` GUI binary started
// twice as an actual OS process, and the real qrc Main.qml. No fake
// transports, no in-process stand-ins.
//
// The SSH-backed cases QSKIP unless CH_LIVE_SSH is set, so the default suite
// stays green on a machine with no fixture. The QML region-width case needs no
// fixture and always runs.
//
// What this proves that tst_appcontroller (fake QIODevice transport) cannot:
//   * the sidebar model is populated from bytes a *different* process wrote
//     into the authoritative database;
//   * every AppController mutation is durable server-side — each one is
//     re-read through a second, independent codeharbord process, so a purely
//     local model update would fail the assertion;
//   * UiStateStore's region widths survive a genuine process boundary (the
//     in-process QSettings cache cannot mask it);
//   * the shipped GUI binary really connects over SSH, builds its whole QML
//     tree, and leaves the stored region widths ALONE — including when they
//     cannot be honoured by the current window, which is what used to destroy
//     them (Main.qml persists on drag end only);
//   * the real Main.qml restores those widths into the live layout, and a real
//     handle drag writes the new ones back;
//   * a server-side failure reaches AppController::error verbatim.

#include "AgentStatusMonitor.h"
#include "AppController.h"
#include "CodeharbordClient.h"
#include "EditorFactory.h"
#include "SessionBootstrap.h"
#include "SessionsModel.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"
#include "UiStateStore.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMetaObject>
#include <QModelIndex>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlExpression>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QtQuickControls2/QQuickStyle>
#include <QtTest/QtTest>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include <QSettings>
#include <memory>
#include <optional>

using ch::AgentStatusMonitor;
using ch::AppController;
using ch::CodeharbordClient;
using ch::RpcError;
using ch::SessionBootstrap;
using ch::SessionsModel;
using ch::SshChannelDevice;
using ch::SshConnectionPool;
using ch::UiStateStore;

namespace {

// A cold remote `node` start also type-strips the TypeScript entry point.
constexpr int kRpcTimeoutMs = 60000;
// A warm round trip (the codeharbord process is already up).
constexpr int kOpTimeoutMs = 30000;
// A headless WebEngine launch of the full GUI binary, plus its quit delay.
constexpr int kAppTimeoutMs = 120000;
// How long the relaunched GUI binary is left running before it is asked to
// quit: enough for WebEngine init, the QML load and the layout to settle.
constexpr int kAppQuitAfterMs = 8000;

// (sidebar, terminal) region widths. A named alias because a bare
// QPair<int, int> cannot be passed through QCOMPARE (the comma splits the
// macro's argument list).
using WidthPair = QPair<int, int>;

// Organisation/application names of the shipped binary (main.cpp). The test
// process adopts them so its production UiStateStore and the relaunched GUI
// binary address the very same QSettings file.
const char kOrganization[] = "CodeHarbor";
const char kApplication[] = "CodeHarbor";

QString env(const char* key)
{
    return qEnvironmentVariable(key);
}

// One JSON-RPC round trip driven to completion on the caller's event loop.
// Used for out-of-band reads/writes that must NOT go through AppController, so
// the controller's own behaviour is never assumed by the thing checking it.
struct RawRpc {
    QJsonValue result;
    std::optional<RpcError> error;
    bool done = false;

    bool call(CodeharbordClient& client, const QString& method,
              const QJsonObject& params, int timeoutMs = kOpTimeoutMs)
    {
        client.call(method, params,
                    [this](QJsonValue value, std::optional<RpcError> err) {
                        result = value;
                        error = err;
                        done = true;
                    });
        if (!QTest::qWaitFor([this] { return done; }, timeoutMs))
            return false;
        return !error.has_value();
    }

    QString diagnostic(const QString& method) const
    {
        if (!done)
            return method + QStringLiteral(": no response within timeout");
        if (error)
            return method + QStringLiteral(": rpc error %1 %2")
                                .arg(error->code)
                                .arg(error->message);
        return QString();
    }
};

// A completely independent view of the same authoritative database: its own
// codeharbord process, on its own SSH channel, with its own RPC client,
// WorkspaceDb and sidebar model. Anything this view can see was written
// through to the server — a mutation that only touched the primary
// controller's in-memory rows is invisible here.
struct FreshView {
    SshChannelDevice device;
    CodeharbordClient client;
    AppController controller;
    QString stderrText;

    explicit FreshView(SshConnectionPool* pool)
        : device(pool, SshConnectionPool::ChannelKind::Rpc)
        , client()
        , controller(&client)
    {
        QObject::connect(&device, &SshChannelDevice::channelError, &device,
                         [this](const QString& text) { stderrText += text; });
    }

    ~FreshView()
    {
        // Same order SessionBootstrap::unwire() uses in production: CLOSE the
        // channel first, THEN detach the client. Closing is what makes
        // CodeharbordClient fail any in-flight call with a transport error;
        // detaching first tears the transport away while a request is still
        // outstanding, which is a sequence the shipped code deliberately avoids.
        device.closeChannel();
        client.setTransport(nullptr);
    }

    bool start(const QString& command, const QString& serverId)
    {
        if (!device.startExec(command))
            return false;
        client.setTransport(&device);
        controller.setServerId(serverId); // triggers the first load
        return true;
    }
};

// Flat, printable rendering of the sidebar model: "group[/session]" per row
// with the roles the sidebar actually binds to. Both an assertion target and
// the evidence this gate prints.
QStringList renderModel(const SessionsModel& model)
{
    QStringList lines;
    for (int g = 0; g < model.rowCount(); ++g) {
        const QModelIndex group = model.index(g, 0);
        lines << QStringLiteral("group[%1] name=%2 id=%3 collapsed=%4 isGroup=%5")
                     .arg(g)
                     .arg(model.data(group, SessionsModel::NameRole).toString(),
                          model.data(group, SessionsModel::IdRole).toString())
                     .arg(model.data(group, SessionsModel::CollapsedRole).toBool())
                     .arg(model.data(group, SessionsModel::IsGroupRole).toBool());
        for (int s = 0; s < model.rowCount(group); ++s) {
            const QModelIndex session = model.index(s, 0, group);
            lines << QStringLiteral(
                         "  session[%1] name=%2 subtitle=%3 id=%4 groupId=%5 isGroup=%6")
                         .arg(s)
                         .arg(model.data(session, SessionsModel::NameRole).toString(),
                              model.data(session, SessionsModel::SubtitleRole).toString(),
                              model.data(session, SessionsModel::IdRole).toString(),
                              model.data(session, SessionsModel::GroupIdRole).toString())
                         .arg(model.data(session, SessionsModel::IsGroupRole).toBool());
        }
    }
    return lines;
}

// Row index of the group with this name, or -1.
int groupRowNamed(const SessionsModel& model, const QString& name)
{
    for (int g = 0; g < model.rowCount(); ++g) {
        if (model.data(model.index(g, 0), SessionsModel::NameRole).toString() == name)
            return g;
    }
    return -1;
}

QStringList sessionNames(const SessionsModel& model, int groupRow)
{
    QStringList names;
    const QModelIndex group = model.index(groupRow, 0);
    for (int s = 0; s < model.rowCount(group); ++s)
        names << model.data(model.index(s, 0, group), SessionsModel::NameRole).toString();
    return names;
}

QStringList sessionIds(const SessionsModel& model, int groupRow)
{
    QStringList ids;
    const QModelIndex group = model.index(groupRow, 0);
    for (int s = 0; s < model.rowCount(group); ++s)
        ids << model.data(model.index(s, 0, group), SessionsModel::IdRole).toString();
    return ids;
}

// Child mode of this same binary: a second real OS process that writes region
// widths through the production object graph (AppController -> UiStateStore ->
// QSettings native scope) and exits. Selected by CH_LIVESHELL_WRITE_WIDTHS so
// the parent can prove the values crossed a process boundary rather than being
// served from this process's QSettings cache.
int runWidthWriter(int argc, char* argv[], const QByteArray& spec)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QString::fromLatin1(kOrganization));
    QCoreApplication::setApplicationName(QString::fromLatin1(kApplication));

    const QList<QByteArray> parts = spec.split(',');
    if (parts.size() != 3) {
        qCritical("CH_LIVESHELL_WRITE_WIDTHS must be sidebar,viewer,terminal");
        return 2;
    }

    // No transport is wired: this child touches client-local UI state only.
    CodeharbordClient client;
    AppController controller(&client);
    controller.uiState()->setRegionWidths(parts.at(0).toInt(), parts.at(1).toInt(),
                                          parts.at(2).toInt());
    // Returning destroys the controller, its UiStateStore and its QSettings,
    // which is what flushes the values to disk.
    return 0;
}

} // namespace

class TstLiveShell : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void liveReadPathPopulatesSidebar();          // (a)
    void liveCrudRoundTripPersistsServerSide();   // (b)
    void regionWidthsSurviveRealRelaunch();       // (c)
    void serverFailureReachesErrorSignal();       // (d)
    void qmlRestoresAndPersistsRegionWidths();    // (c), the QML half

private:
    bool waitForRefresh(int previousCount, int timeoutMs = kOpTimeoutMs);
    QString configFilePath() const;
    QString readConfigFile() const;
    bool launchRealApp(const QString& knownHostsPath, QString* output);

    SshConnectionPool m_pool;
    CodeharbordClient m_client;
    AgentStatusMonitor m_monitor;
    std::unique_ptr<SessionBootstrap> m_bootstrap;
    std::unique_ptr<AppController> m_controller;
    std::unique_ptr<QSignalSpy> m_refreshedSpy;
    std::unique_ptr<FreshView> m_freshView;
    QTemporaryDir m_scratch;

    // False when CH_LIVE_SSH is unset (or libssh is missing): the SSH-backed
    // cases QSKIP, the QML case still runs.
    bool m_live = false;

    QString m_host;
    quint16 m_port = 0;
    QString m_user;
    QString m_node;
    QString m_repo;
    QString m_configHome;
    QString m_rpcCommand;

    // Unique per run: the workspace database is shared with whatever else uses
    // this host, and every row carries a server_id (SPEC 3.5). Scoping the run
    // to its own serverId keeps it invisible to (and isolated from) real data,
    // and makes cleanupTestCase's "delete every group of this serverId" exact.
    QString m_serverId;
    QString m_prefix;

    // Latest AppController::error message, cleared before each operation that
    // is expected to succeed so a server-side failure fails fast and verbatim.
    QString m_lastError;

    // Ids seeded out-of-band in (a) and reused by later cases.
    QString m_alphaGroupId;
    QString m_betaGroupId;
    QString m_sessionOneId;
    QString m_sessionTwoId;
};

void TstLiveShell::initTestCase()
{
    QVERIFY(m_scratch.isValid());

    // Linux's native QSettings path follows XDG_CONFIG_HOME. macOS and
    // Windows ignore that variable, so use the native path those child
    // processes share. The CI hosts are disposable, and using the native
    // path is required because QStandardPaths test mode is process-local.
#if defined(Q_OS_LINUX)
    m_configHome = m_scratch.filePath(QStringLiteral("config"));
    QVERIFY(QDir().mkpath(m_configHome));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_configHome));
    QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
             m_configHome);
#else
    // ...but only the LIVE cases spawn child processes. With no fixture the
    // one case that still runs, qmlRestoresAndPersistsRegionWidths(), lives
    // entirely in this process AND WRITES region widths through the production
    // native-scope store - which off Linux is the developer's own
    // CodeHarbor settings. Test mode is process-local, so it is exactly right
    // here and exactly wrong for the live cases; tst_coldstart draws the same
    // line for the same reason.
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH")
        || !SshConnectionPool::libsshAvailable())
        QStandardPaths::setTestModeEnabled(true);
    m_configHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QVERIFY(QDir().mkpath(m_configHome));
#endif

    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH")) {
        qInfo("CH_LIVE_SSH is not set; the SSH-backed cases will skip");
        return;
    }
    if (!SshConnectionPool::libsshAvailable()) {
        qInfo("built without libssh; the SSH-backed cases will skip");
        return;
    }
    m_live = true;

    m_host = env("CH_LIVE_HOST");
    m_port = static_cast<quint16>(env("CH_LIVE_PORT").toUInt());
    m_user = env("CH_LIVE_USER");
    m_node = env("CH_LIVE_NODE");
    m_repo = env("CH_LIVE_REPO");
    QVERIFY2(!m_host.isEmpty() && m_port != 0 && !m_user.isEmpty()
                 && !m_node.isEmpty() && !m_repo.isEmpty(),
             "CH_LIVE_HOST/PORT/USER/NODE/REPO must all be set");

    m_serverId = QStringLiteral("live-shell-%1-%2")
                     .arg(QCoreApplication::applicationPid())
                     .arg(QDateTime::currentMSecsSinceEpoch());
    m_prefix = QStringLiteral("chlive-%1").arg(QDateTime::currentMSecsSinceEpoch());
    m_rpcCommand = SessionBootstrap::rpcCommand(m_node, m_repo);

    // The production bootstrap: pool connect -> codeharbord over an Rpc channel
    // -> client transport; bridge over an AgentStatus channel -> monitor.
    m_bootstrap = std::make_unique<SessionBootstrap>(&m_pool, &m_client, &m_monitor);
    QString bootstrapError;
    connect(m_bootstrap.get(), &SessionBootstrap::error, this,
            [&bootstrapError](const QString& text) {
                bootstrapError += text + QLatin1Char('\n');
            });
    const QString knownHosts = env("CH_LIVE_KNOWN_HOSTS").isEmpty()
                                   ? m_scratch.filePath(QStringLiteral("known_hosts"))
                                   : env("CH_LIVE_KNOWN_HOSTS");
    m_bootstrap->setKnownHostsPath(knownHosts);
    // Headless gate against a throwaway fixture: there is no user interface here
    // to approve the fixture's host key, so accepting it unasked has to be opted
    // into explicitly. Without this, attemptWire() refuses to connect at all
    // (SPEC 12.1) — which is exactly what an attended build must do.
    m_bootstrap->setTrustUnknownHostKeys(true);
    QVERIFY2(m_bootstrap->connectAndWire(m_host, m_port, m_user, m_node, m_repo),
             qPrintable(bootstrapError));
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY(m_client.transport() == m_bootstrap->rpcDevice());

    // Prove the wired transport really reaches codeharbord before any shell
    // assertion depends on it (and warm the remote node process).
    RawRpc info;
    QVERIFY2(info.call(m_client, QStringLiteral("server.info"), {}, kRpcTimeoutMs),
             qPrintable(info.diagnostic(QStringLiteral("server.info"))));
    qInfo() << "server.info:"
            << QJsonDocument(info.result.toObject()).toJson(QJsonDocument::Compact);
    QCOMPARE(info.result.toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("codeharbord"));

    // The shell as main.cpp builds it.
    m_controller = std::make_unique<AppController>(&m_client);
    m_controller->setAgentMonitor(&m_monitor);
    connect(m_controller.get(), &AppController::error, this,
            [this](const QString& message) { m_lastError = message; });
    m_refreshedSpy = std::make_unique<QSignalSpy>(m_controller.get(),
                                                  &AppController::refreshed);

    // Setting the server id drives the initial load; this run's workspace is
    // empty, which is exactly the starting state (a) then fills out-of-band.
    m_controller->setServerId(m_serverId);
    QVERIFY2(waitForRefresh(0, kRpcTimeoutMs), qPrintable(m_lastError));
    QCOMPARE(m_controller->sessionsModel()->rowCount(), 0);
}

void TstLiveShell::cleanupTestCase()
{
    // Remove everything this run created: deleteGroup cascades its sessions.
    if (m_client.transport() != nullptr && !m_serverId.isEmpty()) {
        m_freshView.reset();
        RawRpc listed;
        if (listed.call(m_client, QStringLiteral("workspace.list"),
                        {{QStringLiteral("serverId"), m_serverId}})) {
            const QJsonArray groups = listed.result.toArray();
            for (const QJsonValue& group : groups) {
                RawRpc removed;
                removed.call(m_client, QStringLiteral("workspace.deleteGroup"),
                             {{QStringLiteral("id"),
                               group.toObject().value(QStringLiteral("id")).toString()}});
            }
        }
        RawRpc verify;
        if (verify.call(m_client, QStringLiteral("workspace.list"),
                        {{QStringLiteral("serverId"), m_serverId}})) {
            qInfo() << "cleanup: rows left for" << m_serverId << "="
                    << verify.result.toArray().size();
        }
    }

    m_refreshedSpy.reset();
    m_controller.reset();
    m_bootstrap.reset();
    m_pool.disconnectFromHost();
}

bool TstLiveShell::waitForRefresh(int previousCount, int timeoutMs)
{
    // Give up as soon as the server reports a failure rather than burning the
    // whole timeout on an operation that will never refresh.
    const bool settled = QTest::qWaitFor(
        [this, previousCount] {
            return m_refreshedSpy->count() > previousCount || !m_lastError.isEmpty();
        },
        timeoutMs);
    if (!settled)
        m_lastError = QStringLiteral("timed out waiting for AppController::refreshed");
    return m_lastError.isEmpty() && m_refreshedSpy->count() > previousCount;
}

QString TstLiveShell::configFilePath() const
{
    // Ask QSettings for the platform-native file rather than assuming the
    // Linux INI layout. macOS stores NativeFormat differently, while the
    // production UiStateStore uses this exact constructor.
    return QSettings(QStringLiteral("CodeHarbor"), QStringLiteral("CodeHarbor"))
        .fileName();
}

QString TstLiveShell::readConfigFile() const
{
    QSettings settings(QStringLiteral("CodeHarbor"), QStringLiteral("CodeHarbor"));
    settings.sync();
    return QStringLiteral("layout/sidebarWidth=%1\nlayout/terminalWidth=%2")
        .arg(settings.value(QStringLiteral("layout/sidebarWidth")).toInt())
        .arg(settings.value(QStringLiteral("layout/terminalWidth")).toInt());
}

// ---------------------------------------------------------------------------
// (a) LIVE READ PATH: rows written by another process show up in the sidebar.
// ---------------------------------------------------------------------------
void TstLiveShell::liveReadPathPopulatesSidebar()
{
    if (!m_live)
        QSKIP("CH_LIVE_SSH is not set; live SSH cases skipped");
    // Seed out-of-band, through the raw RPC surface rather than AppController,
    // so this case exercises the read path only.
    RawRpc alpha;
    QVERIFY2(alpha.call(m_client, QStringLiteral("workspace.createGroup"),
                        {{QStringLiteral("serverId"), m_serverId},
                         {QStringLiteral("name"), m_prefix + QStringLiteral("-alpha")}}),
             qPrintable(alpha.diagnostic(QStringLiteral("createGroup"))));
    m_alphaGroupId = alpha.result.toObject().value(QStringLiteral("id")).toString();
    QVERIFY(!m_alphaGroupId.isEmpty());

    RawRpc beta;
    QVERIFY2(beta.call(m_client, QStringLiteral("workspace.createGroup"),
                       {{QStringLiteral("serverId"), m_serverId},
                        {QStringLiteral("name"), m_prefix + QStringLiteral("-beta")},
                        {QStringLiteral("collapsed"), true}}),
             qPrintable(beta.diagnostic(QStringLiteral("createGroup"))));
    m_betaGroupId = beta.result.toObject().value(QStringLiteral("id")).toString();

    RawRpc one;
    QVERIFY2(one.call(m_client, QStringLiteral("workspace.createSession"),
                      {{QStringLiteral("serverId"), m_serverId},
                       {QStringLiteral("groupId"), m_alphaGroupId},
                       {QStringLiteral("name"), m_prefix + QStringLiteral("-one")},
                       {QStringLiteral("repositoryRoot"), QStringLiteral("/srv/repos/alpha")}}),
             qPrintable(one.diagnostic(QStringLiteral("createSession"))));
    m_sessionOneId = one.result.toObject().value(QStringLiteral("id")).toString();

    RawRpc two;
    QVERIFY2(two.call(m_client, QStringLiteral("workspace.createSession"),
                      {{QStringLiteral("serverId"), m_serverId},
                       {QStringLiteral("groupId"), m_alphaGroupId},
                       {QStringLiteral("name"), m_prefix + QStringLiteral("-two")},
                       // Trailing slash: the subtitle is the basename either way.
                       {QStringLiteral("repositoryRoot"), QStringLiteral("/srv/repos/beta/")}}),
             qPrintable(two.diagnostic(QStringLiteral("createSession"))));
    m_sessionTwoId = two.result.toObject().value(QStringLiteral("id")).toString();

    m_lastError.clear();
    const int before = m_refreshedSpy->count();
    m_controller->refresh();
    QVERIFY2(waitForRefresh(before, kRpcTimeoutMs), qPrintable(m_lastError));

    const SessionsModel* model = m_controller->sessionsModel();
    qInfo().noquote() << "sidebar after live load:\n"
                      << renderModel(*model).join(QLatin1Char('\n'));

    QCOMPARE(model->rowCount(), 2);

    const QModelIndex alphaIndex = model->index(0, 0);
    QCOMPARE(model->data(alphaIndex, SessionsModel::NameRole).toString(),
             m_prefix + QStringLiteral("-alpha"));
    QCOMPARE(model->data(alphaIndex, SessionsModel::IdRole).toString(), m_alphaGroupId);
    QCOMPARE(model->data(alphaIndex, SessionsModel::GroupIdRole).toString(), m_alphaGroupId);
    QVERIFY(model->data(alphaIndex, SessionsModel::IsGroupRole).toBool());
    QVERIFY(!model->data(alphaIndex, SessionsModel::CollapsedRole).toBool());

    const QModelIndex betaIndex = model->index(1, 0);
    QCOMPARE(model->data(betaIndex, SessionsModel::NameRole).toString(),
             m_prefix + QStringLiteral("-beta"));
    QCOMPARE(model->data(betaIndex, SessionsModel::IdRole).toString(), m_betaGroupId);
    QVERIFY2(model->data(betaIndex, SessionsModel::CollapsedRole).toBool(),
             "the server's collapsed flag must reach the sidebar");
    QCOMPARE(model->rowCount(betaIndex), 0);

    QCOMPARE(model->rowCount(alphaIndex), 2);
    const QModelIndex first = model->index(0, 0, alphaIndex);
    QCOMPARE(model->data(first, SessionsModel::NameRole).toString(),
             m_prefix + QStringLiteral("-one"));
    QCOMPARE(model->data(first, SessionsModel::SubtitleRole).toString(),
             QStringLiteral("alpha"));
    QCOMPARE(model->data(first, SessionsModel::IdRole).toString(), m_sessionOneId);
    QCOMPARE(model->data(first, SessionsModel::GroupIdRole).toString(), m_alphaGroupId);
    QVERIFY(!model->data(first, SessionsModel::IsGroupRole).toBool());

    const QModelIndex second = model->index(1, 0, alphaIndex);
    QCOMPARE(model->data(second, SessionsModel::NameRole).toString(),
             m_prefix + QStringLiteral("-two"));
    QCOMPARE(model->data(second, SessionsModel::SubtitleRole).toString(),
             QStringLiteral("beta"));
    QCOMPARE(model->data(second, SessionsModel::IdRole).toString(), m_sessionTwoId);
}

// ---------------------------------------------------------------------------
// (b) LIVE CRUD: every AppController mutation is durable server-side.
// ---------------------------------------------------------------------------
void TstLiveShell::liveCrudRoundTripPersistsServerSide()
{
    if (!m_live)
        QSKIP("CH_LIVE_SSH is not set; live SSH cases skipped");
    QVERIFY2(!m_alphaGroupId.isEmpty(), "case (a) must have seeded the workspace");
    SessionsModel* model = m_controller->sessionsModel();

    // A second codeharbord process reading the same database. Everything below
    // is confirmed here before it counts as persisted.
    m_freshView = std::make_unique<FreshView>(&m_pool);
    QVERIFY2(m_freshView->start(m_rpcCommand, m_serverId),
             qPrintable(QStringLiteral("second codeharbord failed to start: %1")
                            .arg(m_freshView->stderrText)));
    SessionsModel* remoteModel = m_freshView->controller.sessionsModel();
    QTRY_VERIFY_WITH_TIMEOUT(remoteModel->rowCount() == 2, kRpcTimeoutMs);

    // --- createGroup --------------------------------------------------------
    const QString crudName = m_prefix + QStringLiteral("-crud");
    m_lastError.clear();
    int before = m_refreshedSpy->count();
    m_controller->createGroup(crudName);
    QVERIFY2(waitForRefresh(before, kRpcTimeoutMs), qPrintable(m_lastError));
    QCOMPARE(model->rowCount(), 3);
    const int crudRow = groupRowNamed(*model, crudName);
    QCOMPARE(crudRow, 2);
    const QString crudGroupId =
        model->data(model->index(crudRow, 0), SessionsModel::IdRole).toString();
    QVERIFY(!crudGroupId.isEmpty());

    // --- createSession ------------------------------------------------------
    const QString serviceName = m_prefix + QStringLiteral("-svc");
    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->createSession(crudGroupId, serviceName,
                                QStringLiteral("/srv/repos/gamma"));
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QCOMPARE(model->rowCount(model->index(crudRow, 0)), 1);
    const QModelIndex serviceIndex = model->index(0, 0, model->index(crudRow, 0));
    QCOMPARE(model->data(serviceIndex, SessionsModel::NameRole).toString(), serviceName);
    QCOMPARE(model->data(serviceIndex, SessionsModel::SubtitleRole).toString(),
             QStringLiteral("gamma"));
    const QString serviceId =
        model->data(serviceIndex, SessionsModel::IdRole).toString();
    QVERIFY(!serviceId.isEmpty());

    // --- duplicateSession ---------------------------------------------------
    // The server copies the row under a fresh id and appends it (SPEC 4.2), so
    // the group gains a second, identically named session with a different id.
    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->duplicateSession(serviceId);
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QCOMPARE(model->rowCount(model->index(crudRow, 0)), 2);
    const QModelIndex copyIndex = model->index(1, 0, model->index(crudRow, 0));
    QCOMPARE(model->data(copyIndex, SessionsModel::NameRole).toString(), serviceName);
    QCOMPARE(model->data(copyIndex, SessionsModel::SubtitleRole).toString(),
             QStringLiteral("gamma"));
    const QString copyId = model->data(copyIndex, SessionsModel::IdRole).toString();
    QVERIFY(!copyId.isEmpty());
    QVERIFY2(copyId != serviceId, "the duplicate must be a new row, not an alias");

    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->deleteSession(copyId);
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QCOMPARE(sessionIds(*model, crudRow), QStringList{serviceId});

    // --- rename session + group, collapse group -----------------------------
    const QString renamed = m_prefix + QStringLiteral("-svc-renamed");
    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->renameSession(serviceId, renamed);
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QCOMPARE(model->data(model->index(0, 0, model->index(crudRow, 0)),
                         SessionsModel::NameRole)
                 .toString(),
             renamed);

    const QString crudRenamed = m_prefix + QStringLiteral("-crud2");
    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->renameGroup(crudGroupId, crudRenamed);
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QCOMPARE(groupRowNamed(*model, crudRenamed), crudRow);

    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->setGroupCollapsed(crudGroupId, true);
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QVERIFY(model->data(model->index(crudRow, 0), SessionsModel::CollapsedRole).toBool());

    // --- move the session into the alpha group ------------------------------
    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->moveSession(serviceId, m_alphaGroupId, 0);
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QCOMPARE(model->rowCount(model->index(crudRow, 0)), 0);
    QCOMPARE(model->rowCount(model->index(0, 0)), 3);
    QVERIFY(sessionIds(*model, 0).contains(serviceId));
    QCOMPARE(model->data(model->index(sessionIds(*model, 0).indexOf(serviceId), 0,
                                      model->index(0, 0)),
                         SessionsModel::GroupIdRole)
                 .toString(),
             m_alphaGroupId);
    // Membership, not placement: the server stores the requested position
    // without re-packing the target group's existing rows, so this row and the
    // group's first session both sit at position 0 and `ORDER BY position, id`
    // breaks the tie by UUID. Asserting a row index here would be asserting a
    // coin flip, so the observed order is only recorded.
    qInfo().noquote() << "order after moveSession(position=0), landed at index"
                      << sessionIds(*model, 0).indexOf(serviceId) << "of"
                      << model->rowCount(model->index(0, 0)) << ":\n"
                      << sessionNames(*model, 0).join(QLatin1Char('\n'));

    // --- reorder the alpha group's sessions ---------------------------------
    const QStringList reordered{m_sessionTwoId, serviceId, m_sessionOneId};
    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->reorderSessions(m_alphaGroupId, reordered);
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QCOMPARE(sessionIds(*model, 0), reordered);

    // --- reorder the groups -------------------------------------------------
    const QStringList groupOrder{crudGroupId, m_betaGroupId, m_alphaGroupId};
    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->reorderGroups(groupOrder);
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QStringList observedGroups;
    for (int g = 0; g < model->rowCount(); ++g)
        observedGroups << model->data(model->index(g, 0), SessionsModel::IdRole).toString();
    QCOMPARE(observedGroups, groupOrder);

    qInfo().noquote() << "sidebar after mutations:\n"
                      << renderModel(*model).join(QLatin1Char('\n'));

    // --- the mutations must be on the SERVER, not just in this model --------
    QSignalSpy remoteRefreshed(&m_freshView->controller, &AppController::refreshed);
    m_freshView->controller.refresh();
    QVERIFY2(remoteRefreshed.wait(kRpcTimeoutMs),
             qPrintable(QStringLiteral("second codeharbord did not answer: %1")
                            .arg(m_freshView->stderrText)));
    qInfo().noquote() << "sidebar re-read through a SECOND codeharbord process:\n"
                      << renderModel(*remoteModel).join(QLatin1Char('\n'));

    QCOMPARE(renderModel(*remoteModel), renderModel(*model));
    QCOMPARE(remoteModel->rowCount(), 3);
    QCOMPARE(groupRowNamed(*remoteModel, crudRenamed), 0);
    QVERIFY(remoteModel->data(remoteModel->index(0, 0), SessionsModel::CollapsedRole)
                .toBool());
    QCOMPARE(sessionIds(*remoteModel, 2), reordered);
    QVERIFY(sessionNames(*remoteModel, 2).contains(renamed));

    // --- delete, and confirm the delete is durable too ----------------------
    m_lastError.clear();
    before = m_refreshedSpy->count();
    m_controller->deleteSession(serviceId);
    QVERIFY2(waitForRefresh(before), qPrintable(m_lastError));
    QVERIFY(!sessionIds(*model, 2).contains(serviceId));
    QCOMPARE(model->rowCount(model->index(2, 0)), 2);

    const int remoteBefore = remoteRefreshed.count();
    m_freshView->controller.refresh();
    QVERIFY(QTest::qWaitFor([&] { return remoteRefreshed.count() > remoteBefore; },
                            kRpcTimeoutMs));
    QVERIFY2(!sessionIds(*remoteModel, 2).contains(serviceId),
             "deleteSession must remove the row server-side, not only locally");
    QCOMPARE(renderModel(*remoteModel), renderModel(*model));
}

// ---------------------------------------------------------------------------
// (c) PERSISTENCE ACROSS A REAL RELAUNCH.
//
// Main.qml persists region widths ONLY when a handle drag finishes. A launch
// therefore performs no write at all, and the contract a launch can prove is
// the one that used to be violated: the stored widths come out untouched —
// including when the current window cannot honour them, which is exactly the
// case that used to overwrite them with the layout-clamped value (a stored
// 888 px terminal came back as 331 px after a single 1440 px launch).
//
// That the widths are actually RESTORED into the layout is proved in
// qmlRestoresAndPersistsRegionWidths(), which can see the live item geometry;
// from outside the process a correct launch is now indistinguishable from one
// that ignored the stored values, since neither writes anything.
// ---------------------------------------------------------------------------
void TstLiveShell::regionWidthsSurviveRealRelaunch()
{
    if (!m_live)
        QSKIP("CH_LIVE_SSH is not set; live SSH cases skipped");

    // Widths that fit the shipped 1440x900 window beside the viewer region's
    // 320 px minimum, and widths that cannot possibly fit it.
    constexpr int kFitSidebar = 341;
    constexpr int kFitTerminal = 462;
    constexpr int kOversizedSidebar = 777;
    constexpr int kOversizedTerminal = 888;

    // Write the widths from a REAL second OS process, through the production
    // graph (AppController -> UiStateStore -> QSettings native scope), so the
    // values genuinely cross a process boundary instead of being served from
    // this process's QSettings cache.
    const auto writeWidthsInChildProcess = [this](int sidebar, int terminal,
                                                  QString* diagnostic) {
        QProcessEnvironment writerEnv = QProcessEnvironment::systemEnvironment();
        writerEnv.insert(QStringLiteral("XDG_CONFIG_HOME"), m_configHome);
        writerEnv.insert(QStringLiteral("CH_LIVESHELL_WRITE_WIDTHS"),
                         QStringLiteral("%1,0,%2").arg(sidebar).arg(terminal));
        QProcess writer;
        writer.setProcessEnvironment(writerEnv);
        writer.setProcessChannelMode(QProcess::MergedChannels);
        writer.start(QCoreApplication::applicationFilePath(), {});
        if (!writer.waitForStarted(15000) || !writer.waitForFinished(60000)) {
            *diagnostic = writer.errorString();
            return false;
        }
        *diagnostic = QString::fromUtf8(writer.readAll());
        return writer.exitStatus() == QProcess::NormalExit && writer.exitCode() == 0;
    };

    // What the shipped app reads on startup: a brand new production store.
    const auto storedWidths = [] {
        CodeharbordClient offline;
        AppController reopened(&offline);
        return WidthPair(reopened.uiState()->sidebarWidth(),
                               reopened.uiState()->terminalWidth());
    };

    // --- launch #1: widths that fit -----------------------------------------
    QString writerOutput;
    QVERIFY2(writeWidthsInChildProcess(kFitSidebar, kFitTerminal, &writerOutput),
             qPrintable(writerOutput));
    QVERIFY2(QFileInfo::exists(configFilePath()), qPrintable(configFilePath()));
    qInfo().noquote() << "config written by the writer process ("
                      << configFilePath() << "):\n"
                      << readConfigFile();
    QCOMPARE(storedWidths(), WidthPair(kFitSidebar, kFitTerminal));

    const QString knownHostsOne = m_scratch.filePath(QStringLiteral("app1_known_hosts"));
    QString outputOne;
    const QDateTime beforeFirst = QFileInfo(configFilePath()).lastModified();
    // exitCode 0 also means the QML tree was created: main.cpp exits -1 on
    // QQmlApplicationEngine::objectCreationFailed.
    QVERIFY2(launchRealApp(knownHostsOne, &outputOne), qPrintable(outputOne));

    // It really spoke SSH: SessionBootstrap only writes this file after the
    // fixture's host key was accepted on a successful connection.
    QVERIFY2(QFileInfo::exists(knownHostsOne),
             qPrintable(QStringLiteral("the app never completed an SSH handshake; "
                                       "output:\n%1")
                            .arg(outputOne)));
    qInfo().noquote() << "known_hosts written by launch #1:\n"
                      << [&] {
                             QFile f(knownHostsOne);
                             return f.open(QIODevice::ReadOnly)
                                        ? QString::fromUtf8(f.readAll()).trimmed()
                                        : QString();
                         }();

    qInfo().noquote() << "config after real launch #1:\n" << readConfigFile();
    QCOMPARE(storedWidths(), WidthPair(kFitSidebar, kFitTerminal));
    QCOMPARE(QFileInfo(configFilePath()).lastModified(), beforeFirst);

    // --- launch #2: widths the window CANNOT honour -------------------------
    // 777 + 888 leaves the viewer region far below its 320 px minimum in a
    // 1440 px window, so SplitView clamps the terminal region on restore. The
    // stored value must survive that clamp untouched.
    QVERIFY2(writeWidthsInChildProcess(kOversizedSidebar, kOversizedTerminal,
                                       &writerOutput),
             qPrintable(writerOutput));
    QCOMPARE(storedWidths(), WidthPair(kOversizedSidebar, kOversizedTerminal));

    const QString knownHostsTwo = m_scratch.filePath(QStringLiteral("app2_known_hosts"));
    QString outputTwo;
    const QDateTime beforeSecond = QFileInfo(configFilePath()).lastModified();
    QVERIFY2(launchRealApp(knownHostsTwo, &outputTwo), qPrintable(outputTwo));
    QVERIFY(QFileInfo::exists(knownHostsTwo));

    qInfo().noquote() << "config after real launch #2 (oversized widths):\n"
                      << readConfigFile();
    QVERIFY2(storedWidths() == WidthPair(kOversizedSidebar, kOversizedTerminal),
             "a launch clobbered stored region widths with layout-clamped values");
    QCOMPARE(QFileInfo(configFilePath()).lastModified(), beforeSecond);
}

// Run the shipped GUI binary headless against the live fixture. It is asked to
// quit through its own event loop (LD_PRELOAD shim) after kAppQuitAfterMs so
// its normal shutdown path — including the QSettings flush — actually runs.
bool TstLiveShell::launchRealApp(const QString& knownHostsPath, QString* output)
{
    QProcessEnvironment appEnv = QProcessEnvironment::systemEnvironment();
    appEnv.insert(QStringLiteral("XDG_CONFIG_HOME"), m_configHome);
    appEnv.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    appEnv.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    // NB: no --single-process. It is fatal the moment a second
    // QWebEngineProfile exists (QFATAL "Single mode supports only single
    // profile."), and the viewer stack creates two by design (SPEC 7.3,
    // "Browser Profiles": one sandboxed profile for external sites, one
    // privileged profile for internal content).
    appEnv.insert(QStringLiteral("QTWEBENGINE_CHROMIUM_FLAGS"),
                  QStringLiteral("--disable-gpu --no-sandbox --disable-dev-shm-usage"));
    appEnv.insert(QStringLiteral("LD_PRELOAD"), QStringLiteral(CH_LIVESHELL_QUIT_SHIM));
    appEnv.insert(QStringLiteral("CH_QUIT_AFTER_MS"), QString::number(kAppQuitAfterMs));
    appEnv.insert(QStringLiteral("CH_LIVE_SSH"), QStringLiteral("1"));
    appEnv.insert(QStringLiteral("CH_LIVE_HOST"), m_host);
    appEnv.insert(QStringLiteral("CH_LIVE_PORT"), QString::number(m_port));
    appEnv.insert(QStringLiteral("CH_LIVE_USER"), m_user);
    appEnv.insert(QStringLiteral("CH_LIVE_NODE"), m_node);
    appEnv.insert(QStringLiteral("CH_LIVE_REPO"), m_repo);
    appEnv.insert(QStringLiteral("CH_LIVE_KNOWN_HOSTS"), knownHostsPath);
    appEnv.remove(QStringLiteral("CH_LIVESHELL_WRITE_WIDTHS"));

    QProcess app;
    app.setProcessEnvironment(appEnv);
    app.setProcessChannelMode(QProcess::MergedChannels);
    app.start(QStringLiteral(CH_LIVESHELL_APP), {});
    if (!app.waitForStarted(30000)) {
        *output = QStringLiteral("could not start %1: %2")
                      .arg(QStringLiteral(CH_LIVESHELL_APP), app.errorString());
        return false;
    }
    if (!app.waitForFinished(kAppTimeoutMs)) {
        app.kill();
        app.waitForFinished(5000);
        *output = QStringLiteral("%1 never exited: %2")
                      .arg(QStringLiteral(CH_LIVESHELL_APP),
                           QString::fromUtf8(app.readAll()));
        return false;
    }
    *output = QStringLiteral("exitStatus=%1 exitCode=%2 output:\n%3")
                  .arg(app.exitStatus() == QProcess::NormalExit
                           ? QStringLiteral("normal")
                           : QStringLiteral("crash"))
                  .arg(app.exitCode())
                  .arg(QString::fromUtf8(app.readAll()));
    return app.exitStatus() == QProcess::NormalExit && app.exitCode() == 0;
}

// ---------------------------------------------------------------------------
// (d) ERROR SURFACING: a real server-side failure reaches AppController::error.
// ---------------------------------------------------------------------------
void TstLiveShell::serverFailureReachesErrorSignal()
{
    if (!m_live)
        QSKIP("CH_LIVE_SSH is not set; live SSH cases skipped");
    SessionsModel* model = m_controller->sessionsModel();
    const QStringList before = renderModel(*model);

    QSignalSpy errorSpy(m_controller.get(), &AppController::error);
    const int refreshedBefore = m_refreshedSpy->count();
    m_lastError.clear();

    const QString bogusGroup = QStringLiteral("no-such-group-%1")
                                   .arg(QDateTime::currentMSecsSinceEpoch());
    m_controller->createSession(bogusGroup, m_prefix + QStringLiteral("-orphan"),
                                QStringLiteral("/srv/repos/nope"));

    QVERIFY2(errorSpy.wait(kOpTimeoutMs),
             "a mutation against a bogus parent id must not fail silently");
    QCOMPARE(errorSpy.count(), 1);
    const QString message = errorSpy.at(0).at(0).toString();
    qInfo().noquote() << "AppController::error =" << message;
    // Forwarded verbatim from the server (SPEC 10.3), not a client-side
    // substitute: it names the table and the id we sent.
    QVERIFY2(message.contains(QStringLiteral("groups not found")),
             qPrintable(message));
    QVERIFY2(message.contains(bogusGroup), qPrintable(message));

    // A failed mutation refreshes nothing and leaves the sidebar untouched.
    QCOMPARE(m_refreshedSpy->count(), refreshedBefore);
    QCOMPARE(renderModel(*model), before);

    // And nothing was half-created server-side: a real reload agrees.
    m_lastError.clear();
    const int refreshBase = m_refreshedSpy->count();
    m_controller->refresh();
    QVERIFY2(waitForRefresh(refreshBase), qPrintable(m_lastError));
    QCOMPARE(renderModel(*model), before);

    // Same for an update against an unknown row.
    QSignalSpy renameErrors(m_controller.get(), &AppController::error);
    const QString bogusSession = QStringLiteral("no-such-session-%1")
                                     .arg(QDateTime::currentMSecsSinceEpoch());
    m_controller->renameSession(bogusSession, m_prefix + QStringLiteral("-ghost"));
    QVERIFY(renameErrors.wait(kOpTimeoutMs));
    const QString renameMessage = renameErrors.at(0).at(0).toString();
    qInfo().noquote() << "AppController::error =" << renameMessage;
    QVERIFY2(renameMessage.contains(QStringLiteral("session not found")),
             qPrintable(renameMessage));
    QCOMPARE(renderModel(*model), before);
}

// ---------------------------------------------------------------------------
// (c, QML half) The real Main.qml must restore the stored widths into the live
// layout, persist new ones when a handle drag finishes, and persist NOTHING
// when the layout alone changes a region's width.
//
// This is the half a relaunch can no longer show from the outside, and it is
// the regression gate for the clobbering bug: before Main.qml persisted on
// drag end only, the window-shrink step below would have overwritten the
// stored widths with the squeezed ones.
//
// Needs no fixture (region widths are client-local, SPEC 4.1), so it also runs
// in the default suite.
// ---------------------------------------------------------------------------
void TstLiveShell::qmlRestoresAndPersistsRegionWidths()
{
    constexpr int kStoredSidebar = 352;
    constexpr int kStoredTerminal = 471;
    constexpr int kDragDelta = 60;

    {
        UiStateStore seed; // native scope, redirected to m_configHome
        seed.setRegionWidths(kStoredSidebar, 0, kStoredTerminal);
    }

    // main.cpp's object graph. No transport is wired: the shell's region widths
    // are client-local, so no server takes part in restoring or persisting them.
    CodeharbordClient client;
    AgentStatusMonitor monitor;
    AppController controller(&client);
    controller.setAgentMonitor(&monitor);
    ch::ViewerProfiles profiles(&client);
    ch::ViewerModel viewers(&client);
    viewers.setProfiles(&profiles);
    ch::EditorFactory editorFactory(&client);

    QStringList qmlWarnings;
    QQmlApplicationEngine engine;
    connect(&engine, &QQmlEngine::warnings, &engine,
            [&qmlWarnings](const QList<QQmlError>& warnings) {
                for (const QQmlError& error : warnings)
                    qmlWarnings.append(error.toString());
            });
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("viewers"), &viewers);
    engine.rootContext()->setContextProperty(QStringLiteral("agentMonitor"), &monitor);
    engine.rootContext()->setContextProperty(QStringLiteral("editorFactory"),
                                             &editorFactory);
    engine.loadFromModule("CodeHarbor", "Main");
    QVERIFY2(!engine.rootObjects().isEmpty(),
             qPrintable(QStringLiteral("Main.qml did not load:\n%1")
                            .arg(qmlWarnings.join(QLatin1Char('\n')))));

    QObject* root = engine.rootObjects().constFirst();
    auto* window = qobject_cast<QQuickWindow*>(root);
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QSignalSpy frameSpy(window, &QQuickWindow::frameSwapped);
    QVERIFY(frameSpy.isValid());

    // Read through the component's own context so the assertions address
    // exactly the items Main.qml names by id.
    QQmlContext* context = qmlContext(root);
    QVERIFY(context != nullptr);
    const auto evalReal = [context, root](const char* expression) {
        QQmlExpression expr(context, root, QString::fromLatin1(expression));
        return expr.evaluate().toReal();
    };
    const auto evalObject = [context, root](const char* expression) {
        QQmlExpression expr(context, root, QString::fromLatin1(expression));
        return expr.evaluate().value<QObject*>();
    };

    // --- 1. restore ---------------------------------------------------------
    // Layout settles asynchronously (Component.onCompleted -> polish), so this
    // is the one place a QTRY is required rather than a plain compare.
    QTRY_COMPARE(qRound(evalReal("sidebarRegion.width")), kStoredSidebar);
    QCOMPARE(qRound(evalReal("terminalRegion.width")), kStoredTerminal);
    qInfo().noquote() << QStringLiteral(
                             "Main.qml restored sidebar=%1 terminal=%2 from the store")
                             .arg(evalReal("sidebarRegion.width"))
                             .arg(evalReal("terminalRegion.width"));
    // Force a rendered layout before changing a region's attached width.
    window->update();
    QTRY_VERIFY(frameSpy.count() > 0);

    // --- 2. a completed resize persists the new widths ---------------------
    // The offscreen QPA platform cannot deliver a SplitView handle drag
    // reliably. Drive the attached width through the QML API, then emit the
    // same signal a completed user drag emits. This exercises Main.qml's
    // resizingChanged -> persistRegionWidths wiring without depending on
    // platform input synthesis.
    QObject* outer = evalObject("outer");
    QVERIFY(outer != nullptr);
    const int intendedSidebar = kStoredSidebar + kDragDelta;
    QQmlExpression setSidebarWidth(
        context, root,
        QStringLiteral("sidebarRegion.SplitView.preferredWidth = %1")
            .arg(intendedSidebar));
    setSidebarWidth.evaluate();
    QVERIFY2(!setSidebarWidth.hasError(),
             qPrintable(setSidebarWidth.error().toString()));
    QTRY_COMPARE(qRound(evalReal("sidebarRegion.width")), intendedSidebar);
    QVERIFY2(QMetaObject::invokeMethod(outer, "resizingChanged"),
             "Main.qml did not wire SplitView resizingChanged");

    const int persistedSidebar = qRound(evalReal("sidebarRegion.width"));
    const int persistedTerminal = qRound(evalReal("terminalRegion.width"));
    {
        UiStateStore stored;
        QCOMPARE(stored.sidebarWidth(), persistedSidebar);
        QCOMPARE(stored.terminalWidth(), persistedTerminal);
    }
    // ...and it reached disk, not just the in-process QSettings cache.
    QTest::qWait(300);
    const QString afterDrag = readConfigFile();
    qInfo().noquote() << "config after persisting region widths:\n" << afterDrag;
    QVERIFY2(afterDrag.contains(QStringLiteral("sidebarWidth=%1").arg(persistedSidebar)),
             qPrintable(afterDrag));

    // --- 3. a layout-driven width change persists NOTHING -------------------
    const QDateTime beforeResize = QFileInfo(configFilePath()).lastModified();
    window->setWidth(900); // forces SplitView to squeeze the regions
    QTRY_VERIFY(qRound(evalReal("sidebarRegion.width")) != persistedSidebar
                || qRound(evalReal("terminalRegion.width")) != persistedTerminal);
    QTest::qWait(500); // longer than any debounce could plausibly be
    qInfo().noquote() << QStringLiteral(
                             "after shrinking the window to 900: sidebar=%1 terminal=%2")
                             .arg(evalReal("sidebarRegion.width"))
                             .arg(evalReal("terminalRegion.width"));

    {
        UiStateStore stored;
        QVERIFY2(stored.sidebarWidth() == persistedSidebar
                     && stored.terminalWidth() == persistedTerminal,
                 qPrintable(QStringLiteral("a layout-driven resize overwrote the "
                                           "stored widths: %1/%2 became %3/%4")
                                .arg(persistedSidebar)
                                .arg(persistedTerminal)
                                .arg(stored.sidebarWidth())
                                .arg(stored.terminalWidth())));
    }
    QCOMPARE(QFileInfo(configFilePath()).lastModified(), beforeResize);
    QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join(QLatin1Char('\n'))));
}

int main(int argc, char* argv[])
{
    const QByteArray widths = qgetenv("CH_LIVESHELL_WRITE_WIDTHS");
    if (!widths.isEmpty())
        return runWidthWriter(argc, argv, widths);

    // Mirrors src/app/main.cpp's setup order: the custom URL scheme and
    // WebEngine must both be initialised before the GUI application, because
    // the QML case instantiates the real application tree.
    ch::ViewerProfiles::registerUrlScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QString::fromLatin1(kOrganization));
    QGuiApplication::setApplicationName(QString::fromLatin1(kApplication));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    TstLiveShell testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_liveshell.moc"
