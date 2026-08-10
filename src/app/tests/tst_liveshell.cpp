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
#include "GroupPaletteService.h"
#include "EditorFactory.h"
#include "SessionBootstrap.h"
#include "SessionsModel.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"
#include "UiStateStore.h"
#include "ServerProfiles.h"
#include "SessionState.h"
#include "SessionLayouts.h"
#include "TerminalController.h"
#include "TerminalFactory.h"
#include "WindowChromeNative.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
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
#include <QQmlComponent>
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
// A pane coming all the way up: SSH channel, cold tmux server, login shell,
// then the packaged xterm.js page mounting inside a headless Chromium.
constexpr int kPaneTimeoutMs = 90000;
// One runJavaScript round trip into an already-loaded page.
constexpr int kProbeTimeoutMs = 20000;

// Reads a pane's own renderer the way tst_terminalpage does — from the PAGE,
// not from the bridge — because "the pane is blank" is a claim about what
// xterm has on screen and nothing else can answer it. Instantiated once
// against the same engine Main.qml is loaded into, so `runJavaScript` reaches
// the very WebEngineView the production pane created.
constexpr auto kPaneProbeQml = R"QML(
import QtQuick

QtObject {
    id: probe

    property string jsResult: ""
    property bool jsFinished: false

    // TerminalPaneView keeps its WebEngineView inside an unnamed Loader, so it
    // is found structurally rather than by an objectName a test would have to
    // add to production code.
    function findView(item) {
        if (!item)
            return null
        if (typeof item.runJavaScript === "function")
            return item
        if (item.item) {
            var nested = findView(item.item)
            if (nested)
                return nested
        }
        for (var i = 0; i < item.children.length; ++i) {
            var found = findView(item.children[i])
            if (found)
                return found
        }
        return null
    }

    function evalJs(pane, script) {
        probe.jsFinished = false
        probe.jsResult = ""
        var view = findView(pane)
        if (!view) {
            probe.jsResult = "NO_VIEW"
            probe.jsFinished = true
            return
        }
        view.runJavaScript(script, function(result) {
            probe.jsResult = (result === undefined || result === null)
                             ? "" : String(result)
            probe.jsFinished = true
        })
    }

    // The region's pane cache is the only place a pane Item can be addressed
    // by its layout slot label; the panes themselves are re-parented around
    // the tree and carry no objectName.
    function paneFor(region, paneId) {
        if (!region || !region.paneCache)
            return null
        return region.paneCache[paneId] ? region.paneCache[paneId] : null
    }

    function paneIds(region) {
        if (!region || !region.paneCache)
            return ""
        var out = []
        for (var key in region.paneCache)
            out.push(key)
        out.sort()
        return out.join(",")
    }

    // EXACTLY what the pane header's split button does, in the same order:
    // report focus, then raise the request. Going through the pane means the
    // whole production relay runs — TerminalRegion stamps it with the pane's
    // session and generation and Main.qml turns it into
    // SessionLayouts::splitPaneForSession.
    function requestSplit(pane, orientation) {
        pane.paneActivated(pane.paneId)
        pane.splitRequested(pane.paneId, orientation)
    }

    function findByName(item, name) {
        if (!item)
            return null
        if (item.objectName === name)
            return item
        var kids = item.children
        for (var i = 0; i < kids.length; ++i) {
            var found = findByName(kids[i], name)
            if (found)
                return found
        }
        return null
    }

    // Press the pane's own recovery control, whichever one is on screen: the
    // banner's Retry while the renderer is up, the placeholder's Connect while
    // it is not. Driving the CONTROL rather than a function keeps the test
    // pinned to what the user can actually do.
    function clickRecovery(pane) {
        var control = findByName(pane, "terminalRetryButton")
        if (!control || control.visible !== true)
            control = findByName(pane, "terminalConnectButton")
        if (!control)
            return "NO_CONTROL"
        control.clicked()
        return "CLICKED"
    }
}
)QML";

// The page's own status strip, written synchronously by the bundle's
// setConnectionState().
constexpr auto kJsStatus = R"JS(
(function () {
    try {
        var el = document.querySelector(".ch-terminal-status");
        if (!el) return "NO_STATUS";
        return el.dataset.state + "|" + el.textContent;
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// xterm's rendered screen, out of the page's diagnostics hook. The hook only
// exists once mountTerminal() has run, so "NO_HOOK" is itself evidence: the
// document loaded but never mounted.
constexpr auto kJsScreenText = R"JS(
(function () {
    try {
        if (typeof window.codeharborTerminalDiagnostics !== "function")
            return "NO_HOOK";
        var text = window.codeharborTerminalDiagnostics().screenText;
        return typeof text === "string" ? text : "NO_SCREEN";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// What the user is actually looking at, as opposed to what xterm is holding.
// The buffer can carry a whole screenful of remote output while the element
// rendering it has no size — a pane like that shows nothing and takes no
// clicks, which is precisely "blank and can't be interacted with", and every
// property the PANE publishes still reads healthy. `screenText` alone cannot
// see it; the grid and the surface rectangle can.
constexpr auto kJsGrid = R"JS(
(function () {
    try {
        if (typeof window.codeharborTerminalDiagnostics !== "function")
            return "NO_HOOK";
        var d = window.codeharborTerminalDiagnostics();
        var s = d.surfaceCssPixels || { width: 0, height: 0 };
        // What is actually PAINTED. Under the DOM renderer the glyphs live in
        // .xterm-rows; under a canvas renderer that element is gone and the
        // canvas rectangle is the only measurable evidence. Both are reported
        // so a pane whose buffer is full while its output is nowhere on screen
        // is distinguishable from one that is genuinely fine.
        var rowsEl = document.querySelector(".xterm-rows");
        var painted = rowsEl ? rowsEl.textContent.replace(/\u00a0/g, " ").trim().length
                             : -1;
        var canvas = d.canvasCssPixels || { width: 0, height: 0 };
        var drawable = d.cols > 0 && d.rows > 0 && s.width > 0 && s.height > 0
                    && (painted > 0 || canvas.width > 0);
        return (drawable ? "OK " : "DEGENERATE ")
             + "cols=" + d.cols + " rows=" + d.rows
             + " surface=" + s.width + "x" + s.height
             + " paintedChars=" + painted
             + " canvas=" + canvas.width + "x" + canvas.height
             + " renderer=" + d.renderer;
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// Whether the WebChannel handshake delivered the bridge. The bundle says so on
// screen when it did not, but this asks the page directly so the three failure
// modes — never loaded, loaded without a bridge, mounted but never attached —
// can be told apart without reading tea leaves out of the status text.
constexpr auto kJsBridge = R"JS(
(function () {
    try {
        return "transport=" + (typeof qt !== "undefined" && qt.webChannelTransport
                               ? "yes" : "no")
             + " hook=" + (typeof window.codeharborTerminalDiagnostics === "function"
                           ? "yes" : "no");
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// A real Enter keystroke on xterm's input target.
constexpr auto kJsPressEnter = R"JS(
(function () {
    try {
        var textarea = document.querySelector(".xterm-helper-textarea");
        if (!textarea) return "NO_TEXTAREA";
        textarea.focus();
        textarea.dispatchEvent(new KeyboardEvent("keydown", {
            key: "Enter", code: "Enter", keyCode: 13, which: 13,
            bubbles: true, cancelable: true
        }));
        return "SENT";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// A real paste into xterm's input target: the page's own data path, so the
// bytes reach the controller through bridge.sendInput() exactly as a typed
// line does.
constexpr auto kJsPasteTemplate = R"JS(
(function () {
    try {
        var textarea = document.querySelector(".xterm-helper-textarea");
        if (!textarea) return "NO_TEXTAREA";
        textarea.focus();
        var data = new DataTransfer();
        data.setData("text/plain", %1);
        textarea.dispatchEvent(new ClipboardEvent("paste", {
            clipboardData: data, bubbles: true, cancelable: true
        }));
        return "PASTED";
    } catch (e) { return "ERR:" + e; }
})()
)JS";


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
        : device(pool)
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
    // (e) The SECOND terminal of a Dev Session — and the one a split creates —
    //     must come up exactly like the first.
    void liveTerminalsAfterTheFirstComeUp();

private:
    bool waitForRefresh(int previousCount, int timeoutMs = kOpTimeoutMs);
    QString configFilePath() const;
    QString readConfigFile() const;
    bool launchRealApp(const QString& knownHostsPath, QString* output);
    // Everything a blank pane could be blocked on, in one line, read off the
    // pane's own published properties.
    static QString describePane(QObject* pane);
    // One runJavaScript round trip into `pane`'s renderer, driven to completion.
    QString paneJs(QObject* probe, QObject* pane, const QString& script,
                   int timeoutMs = kProbeTimeoutMs);
    // Type `line` into the pane FROM THE PAGE until `needle` shows up on
    // xterm's screen. Retyped because the first keystrokes can land before the
    // shell inside a freshly created tmux session has readline up.
    bool paneScreenShows(QObject* probe, QObject* pane, const QString& line,
                         const QString& needle, int timeoutMs);
    // The four claims the existing live pane gate makes about the FIRST
    // terminal, made about an arbitrary one. Returns an empty string when the
    // pane is live and a diagnosis when it is not.
    QString whyPaneIsNotLive(QObject* probe, QObject* pane, const QString& marker);

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

    // Unique per run: every row created below is recorded by its server id.
    // Cleanup never sweeps by a test-shaped name or by a broad server query.
    QString m_serverId;
    QString m_prefix;
    QStringList m_createdGroupIds;

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
    QStringList cleanupFailures;
    if (!m_createdGroupIds.isEmpty()) {
        if (m_client.transport() == nullptr) {
            cleanupFailures
                << QStringLiteral("SSH transport is down; groups not deleted: %1")
                       .arg(m_createdGroupIds.join(QStringLiteral(", ")));
        } else {
            m_freshView.reset();
            for (const QString& groupId : m_createdGroupIds) {
                RawRpc removed;
                if (!removed.call(m_client, QStringLiteral("workspace.deleteGroup"),
                                  {{QStringLiteral("id"), groupId}})) {
                    cleanupFailures
                        << QStringLiteral("group %1: %2")
                               .arg(groupId,
                                    removed.diagnostic(
                                        QStringLiteral("workspace.deleteGroup")));
                }
            }
        }
    }

    m_refreshedSpy.reset();
    m_controller.reset();
    m_bootstrap.reset();
    m_pool.disconnectFromHost();

    QVERIFY2(cleanupFailures.isEmpty(),
             qPrintable(QStringLiteral(
                            "live workspace cleanup failed; rows may remain: %1")
                            .arg(cleanupFailures.join(QStringLiteral(" | ")))));
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
    m_createdGroupIds << m_alphaGroupId;

    RawRpc beta;
    QVERIFY2(beta.call(m_client, QStringLiteral("workspace.createGroup"),
                       {{QStringLiteral("serverId"), m_serverId},
                        {QStringLiteral("name"), m_prefix + QStringLiteral("-beta")},
                        {QStringLiteral("collapsed"), true}}),
             qPrintable(beta.diagnostic(QStringLiteral("createGroup"))));
    m_betaGroupId = beta.result.toObject().value(QStringLiteral("id")).toString();
    m_createdGroupIds << m_betaGroupId;

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
    m_createdGroupIds << crudGroupId;

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
    ch::WindowChromeNative windowChrome;
    ch::GroupPaletteService groupPalette;

    QStringList qmlWarnings;
    QQmlApplicationEngine engine;
    connect(&engine, &QQmlEngine::warnings, &engine,
            [&qmlWarnings](const QList<QQmlError>& warnings) {
                for (const QQmlError& error : warnings)
                    qmlWarnings.append(error.toString());
            });
    engine.rootContext()->setContextProperty(QStringLiteral("groupPalette"),
                                             &groupPalette);
    engine.rootContext()->setContextProperty(QStringLiteral("windowChrome"),
                                             &windowChrome);
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

// ---------------------------------------------------------------------------
// (e) EVERY terminal, not just the first one.
//
// The existing live gates each bring ONE terminal up: tst_terminalpage builds a
// single pane by hand, and tst_coldstart drives the first TerminalPaneView it
// finds under the region. A Dev Session opens with TWO panes
// (SessionLayouts::defaultTree), a split adds a third, and switching Dev
// Sessions builds a fresh pair — none of which any gate has ever looked at.
//
// This case makes the SAME four claims about those panes that the single-pane
// gate makes about its one: the page's status strip reaches "ready", xterm has
// content on screen, and a line pasted INTO THE PAGE reaches the remote shell
// and echoes back. The panes are created the way the UI creates them — the
// seeded default, the pane header's own split request, and a Dev Session
// switch — never by calling a pane function directly.
// ---------------------------------------------------------------------------
void TstLiveShell::liveTerminalsAfterTheFirstComeUp()
{
    if (!m_live)
        QSKIP("CH_LIVE_SSH is not set; live SSH cases skipped");

    const QString runTag = QStringLiteral("%1").arg(QDateTime::currentMSecsSinceEpoch()
                                                    % 1000000);

    // The seed is created further down, once the controller has adopted the
    // SERVER's own identity: main.cpp's connection surface answers server.info
    // and re-keys the workspace, so rows written under this gate's synthetic
    // server id would be invisible to the shell it is about to drive.

    // src/app/main.cpp's object graph, on the SSH session initTestCase already
    // wired. Same members, same wiring, same context property names, and the
    // real qrc Main.qml on top — a terminal that only comes up in a
    // hand-assembled host is not evidence about the shipped one.
    // The controller initTestCase built and loaded, given the connection
    // surface main.cpp injects. A SECOND AppController on the same client
    // never saw these rows: this one is the shell the other cases already
    // proved reads the authoritative workspace.
    AppController& controller = *m_controller;
    ch::ViewerProfiles profiles(&m_client);
    ch::ViewerModel viewers(&m_client);
    viewers.setProfiles(&profiles);
    ch::ServerProfiles serverProfiles;
    ch::SessionLayouts layouts(controller.workspaceDb(), controller.uiState());
    ch::EditorFactory editorFactory(&m_client);
    ch::TerminalFactory terminalFactory(&m_pool);
    ch::WindowChromeNative windowChrome;
    ch::GroupPaletteService groupPalette;

    controller.setConnection(&m_pool, m_bootstrap.get(), &serverProfiles, &layouts);
    terminalFactory.setWorkspace(controller.workspaceDb());
    connect(&controller, &AppController::serverIdChanged, &terminalFactory,
            [&controller, &terminalFactory]() {
                terminalFactory.setServerId(controller.serverId());
            });
    terminalFactory.setAgentMonitor(&m_monitor);
    connect(&terminalFactory, &ch::TerminalFactory::paneRowResolved, &layouts,
            &ch::SessionLayouts::bindTerminalPaneRow);
    controller.setEditorFactory(&editorFactory);
    controller.setTerminalFactory(&terminalFactory);

    // Every diagnostic is stamped with the step that was running, because an
    // unattributed list of identical strings is what makes a live failure
    // unreadable: "the target was not resolved" during a deliberate teardown
    // and the same sentence while a brand new pane is coming up are opposite
    // verdicts.
    QString phase = QStringLiteral("setup");
    QStringList appErrors;
    connect(&controller, &AppController::error, this,
            [&appErrors, &phase](const QString& text) {
                appErrors << QStringLiteral("[%1] %2").arg(phase, text);
            });
    QStringList factoryErrors;
    connect(&terminalFactory, &ch::TerminalFactory::error, this,
            [&factoryErrors, &phase](ch::TerminalController*, const QString& text) {
                factoryErrors << QStringLiteral("[%1] %2").arg(phase, text);
            });
    QStringList layoutErrors;
    connect(&layouts, &ch::SessionLayouts::error, this,
            [&layoutErrors, &phase](const QString& text) {
                layoutErrors << QStringLiteral("[%1] %2").arg(phase, text);
            });

    // setConnection() re-adopts the server's own identity out of server.info,
    // exactly as a live launch does, and re-keys the workspace to it. Every row
    // below therefore belongs to THAT id, and so do the `terminal_panes` rows
    // the panes will resolve their tmux targets from.
    QTRY_VERIFY_WITH_TIMEOUT(controller.serverId() != m_serverId
                                 && !controller.serverId().isEmpty(),
                             kRpcTimeoutMs);
    const QString serverId = controller.serverId();
    terminalFactory.setServerId(serverId);

    RawRpc group;
    QVERIFY2(group.call(m_client, QStringLiteral("workspace.createGroup"),
                        {{QStringLiteral("serverId"), serverId},
                         {QStringLiteral("name"), m_prefix + QStringLiteral("-terminals")}}),
             qPrintable(group.diagnostic(QStringLiteral("createGroup"))));
    const QString groupId = group.result.toObject().value(QStringLiteral("id")).toString();
    QVERIFY(!groupId.isEmpty());
    m_createdGroupIds << groupId;

    const auto makeSession = [this, &serverId, &groupId](const QString& name) {
        RawRpc session;
        if (!session.call(m_client, QStringLiteral("workspace.createSession"),
                          {{QStringLiteral("serverId"), serverId},
                           {QStringLiteral("groupId"), groupId},
                           {QStringLiteral("name"), name},
                           {QStringLiteral("repositoryRoot"), m_repo}})) {
            return QString();
        }
        return session.result.toObject().value(QStringLiteral("id")).toString();
    };
    const QString sessionOne = makeSession(m_prefix + QStringLiteral("-termdev-1"));
    QVERIFY(!sessionOne.isEmpty());
    const QString sessionTwo = makeSession(m_prefix + QStringLiteral("-termdev-2"));
    QVERIFY(!sessionTwo.isEmpty());
    const QString sessionThree = makeSession(m_prefix + QStringLiteral("-termdev-3"));
    QVERIFY(!sessionThree.isEmpty());

    // activateSession() refuses an id the authoritative cache does not hold, so
    // the load that carries these two rows has to have landed first.
    const auto sessionVisible = [&controller](const QString& id) {
        const SessionsModel* model = controller.sessionsModel();
        for (int g = 0; g < model->rowCount(); ++g) {
            const QModelIndex groupIndex = model->index(g, 0);
            for (int s = 0; s < model->rowCount(groupIndex); ++s) {
                if (model->data(model->index(s, 0, groupIndex), SessionsModel::IdRole)
                        .toString() == id) {
                    return true;
                }
            }
        }
        return false;
    };
    m_lastError.clear();
    const int refreshBase = m_refreshedSpy->count();
    controller.refresh();
    QVERIFY2(waitForRefresh(refreshBase, kRpcTimeoutMs), qPrintable(m_lastError));
    QVERIFY2(sessionVisible(sessionOne) && sessionVisible(sessionTwo)
                 && sessionVisible(sessionThree),
             qPrintable(renderModel(*controller.sessionsModel())
                            .join(QLatin1Char('\n'))));

    QStringList qmlWarnings;
    QQmlApplicationEngine engine;
    connect(&engine, &QQmlEngine::warnings, &engine,
            [&qmlWarnings](const QList<QQmlError>& warnings) {
                for (const QQmlError& error : warnings)
                    qmlWarnings.append(error.toString());
            });
    engine.rootContext()->setContextProperty(QStringLiteral("groupPalette"), &groupPalette);
    engine.rootContext()->setContextProperty(QStringLiteral("windowChrome"), &windowChrome);
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("viewers"), &viewers);
    engine.rootContext()->setContextProperty(QStringLiteral("agentMonitor"), &m_monitor);
    engine.rootContext()->setContextProperty(QStringLiteral("editorFactory"), &editorFactory);
    engine.rootContext()->setContextProperty(QStringLiteral("terminalFactory"),
                                             &terminalFactory);
    engine.loadFromModule("CodeHarbor", "Main");
    QVERIFY2(!engine.rootObjects().isEmpty(),
             qPrintable(QStringLiteral("Main.qml did not load:\n%1")
                            .arg(qmlWarnings.join(QLatin1Char('\n')))));

    QObject* root = engine.rootObjects().constFirst();
    auto* window = qobject_cast<QQuickWindow*>(root);
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));

    QQmlComponent probeComponent(&engine);
    probeComponent.setData(QByteArray(kPaneProbeQml),
                           QUrl(QStringLiteral("qrc:/tst_liveshell/paneprobe.qml")));
    QVERIFY2(!probeComponent.isError(), qPrintable(probeComponent.errorString()));
    std::unique_ptr<QObject> probe(probeComponent.create());
    QVERIFY2(probe != nullptr, qPrintable(probeComponent.errorString()));

    QQmlContext* context = qmlContext(root);
    QVERIFY(context != nullptr);
    QQmlExpression regionExpression(context, root, QStringLiteral("terminalRegion"));
    QObject* terminalRegion = regionExpression.evaluate().value<QObject*>();
    QVERIFY2(terminalRegion != nullptr, qPrintable(regionExpression.error().toString()));

    QObject* probeObject = probe.get();
    const auto paneFor = [probeObject, terminalRegion](const QString& paneId) {
        QVariant result;
        if (!QMetaObject::invokeMethod(probeObject, "paneFor", Q_RETURN_ARG(QVariant, result),
                                       Q_ARG(QVariant, QVariant::fromValue(terminalRegion)),
                                       Q_ARG(QVariant, paneId))) {
            return static_cast<QObject*>(nullptr);
        }
        return result.value<QObject*>();
    };
    const auto cachedPaneIds = [probeObject, terminalRegion]() {
        QVariant result;
        QMetaObject::invokeMethod(probeObject, "paneIds", Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, QVariant::fromValue(terminalRegion)));
        return result.toString();
    };
    // The production close path: it ends the remote tmux session AND removes
    // the pane, so nothing this case created is left running on the fixture.
    const auto killPane = [&paneFor](const QString& paneId) {
        if (QObject* pane = paneFor(paneId))
            QMetaObject::invokeMethod(pane, "killSession");
    };

    // ---- the first Dev Session's seeded pair ------------------------------
    phase = QStringLiteral("session 1 open");
    controller.activateSession(sessionOne);
    QTRY_COMPARE_WITH_TIMEOUT(controller.activeSessionId(), sessionOne, kOpTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(terminalRegion->property("devSessionId").toString(),
                              sessionOne, kOpTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(terminalRegion->property("workingDir").toString(),
                              m_repo, kOpTimeoutMs);
    // SessionLayouts::defaultTree seeds terminal-1 above terminal-2, so a brand
    // new Dev Session has a second terminal before the user does anything.
    QTRY_VERIFY_WITH_TIMEOUT(paneFor(QStringLiteral("terminal-1")) != nullptr
                                 && paneFor(QStringLiteral("terminal-2")) != nullptr,
                             kOpTimeoutMs);
    qInfo().noquote() << "session 1 pane cache:" << cachedPaneIds();

    // The FIRST pane is the control. If this one fails the fixture or the probe
    // is at fault, not the thing under test.
    QString verdict = whyPaneIsNotLive(probeObject, paneFor(QStringLiteral("terminal-1")),
                                       runTag + QStringLiteral("A"));
    QVERIFY2(verdict.isEmpty(),
             qPrintable(QStringLiteral("the FIRST terminal of a new Dev Session %1 | "
                                       "factory=%2 layouts=%3")
                            .arg(verdict, factoryErrors.join(QLatin1Char(' ')),
                                 layoutErrors.join(QLatin1Char(' ')))));
    qInfo().noquote() << "terminal-1:" << describePane(paneFor(QStringLiteral("terminal-1")));

    // ...and the SECOND, which no gate has ever looked at.
    verdict = whyPaneIsNotLive(probeObject, paneFor(QStringLiteral("terminal-2")),
                               runTag + QStringLiteral("B"));
    qInfo().noquote() << "terminal-2:" << describePane(paneFor(QStringLiteral("terminal-2")));
    QVERIFY2(verdict.isEmpty(),
             qPrintable(QStringLiteral("the SECOND terminal of a new Dev Session %1 | "
                                       "factory=%2 layouts=%3")
                            .arg(verdict, factoryErrors.join(QLatin1Char(' ')),
                                 layoutErrors.join(QLatin1Char(' ')))));

    // ---- a pane created by the pane header's own split --------------------
    phase = QStringLiteral("session 1 split");
    QVERIFY(QMetaObject::invokeMethod(
        probeObject, "requestSplit",
        Q_ARG(QVariant, QVariant::fromValue(paneFor(QStringLiteral("terminal-1")))),
        Q_ARG(QVariant, QStringLiteral("vertical"))));
    QTRY_VERIFY_WITH_TIMEOUT(paneFor(QStringLiteral("terminal-3")) != nullptr, kOpTimeoutMs);
    qInfo().noquote() << "after split, pane cache:" << cachedPaneIds();
    verdict = whyPaneIsNotLive(probeObject, paneFor(QStringLiteral("terminal-3")),
                               runTag + QStringLiteral("C"));
    qInfo().noquote() << "terminal-3:" << describePane(paneFor(QStringLiteral("terminal-3")));
    QVERIFY2(verdict.isEmpty(),
             qPrintable(QStringLiteral("the terminal a SPLIT created %1 | factory=%2 layouts=%3")
                            .arg(verdict, factoryErrors.join(QLatin1Char(' ')),
                                 layoutErrors.join(QLatin1Char(' ')))));

    // ---- a SECOND Dev Session, opened the way the sidebar opens one --------
    // Switching sessions destroys the pane Items, and with them every handle
    // to their remote shells, so this session's terminals are ended first.
    phase = QStringLiteral("session 1 teardown");
    killPane(QStringLiteral("terminal-1"));
    killPane(QStringLiteral("terminal-2"));
    killPane(QStringLiteral("terminal-3"));
    QTest::qWait(1000);

    phase = QStringLiteral("session 2 open");
    controller.activateSession(sessionTwo);
    QTRY_COMPARE_WITH_TIMEOUT(terminalRegion->property("devSessionId").toString(),
                              sessionTwo, kOpTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(paneFor(QStringLiteral("terminal-1")) != nullptr
                                 && paneFor(QStringLiteral("terminal-2")) != nullptr,
                             kOpTimeoutMs);
    qInfo().noquote() << "session 2 pane cache:" << cachedPaneIds();
    for (const QString& paneId :
         {QStringLiteral("terminal-1"), QStringLiteral("terminal-2")}) {
        verdict = whyPaneIsNotLive(probeObject, paneFor(paneId),
                                   runTag + QStringLiteral("D") + paneId.right(1));
        qInfo().noquote() << "session 2" << paneId << ":" << describePane(paneFor(paneId));
        QVERIFY2(verdict.isEmpty(),
                 qPrintable(QStringLiteral("%1 of a SECOND Dev Session %2 | "
                                           "factory=%3 layouts=%4")
                                .arg(paneId, verdict, factoryErrors.join(QLatin1Char(' ')),
                                     layoutErrors.join(QLatin1Char(' ')))));
    }
    phase = QStringLiteral("session 2 teardown");
    killPane(QStringLiteral("terminal-1"));
    killPane(QStringLiteral("terminal-2"));
    QTest::qWait(1000);

    // ---- splitting inside a session whose layout is already STORED ---------
    // Everything above happened in a tree this process had just seeded. A
    // returning user's session is the other case: the tree comes back off the
    // server with row ids already in it, and ch::SessionLayouts marks id-less
    // leaves as pre-migration on that path. Re-activating sessionOne is that
    // load, and the split that follows is the user's first action in it.
    phase = QStringLiteral("session 1 reopen");
    controller.activateSession(sessionOne);
    QTRY_COMPARE_WITH_TIMEOUT(terminalRegion->property("devSessionId").toString(),
                              sessionOne, kOpTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(paneFor(QStringLiteral("terminal-1")) != nullptr
                                 && paneFor(QStringLiteral("terminal-3")) != nullptr,
                             kOpTimeoutMs);
    qInfo().noquote() << "reopened session 1 pane cache:" << cachedPaneIds();

    phase = QStringLiteral("split into a stored layout");
    QVERIFY(QMetaObject::invokeMethod(
        probeObject, "requestSplit",
        Q_ARG(QVariant, QVariant::fromValue(paneFor(QStringLiteral("terminal-2")))),
        Q_ARG(QVariant, QStringLiteral("horizontal"))));
    QTRY_VERIFY_WITH_TIMEOUT(paneFor(QStringLiteral("terminal-4")) != nullptr, kOpTimeoutMs);
    verdict = whyPaneIsNotLive(probeObject, paneFor(QStringLiteral("terminal-4")),
                               runTag + QStringLiteral("E"));
    qInfo().noquote() << "terminal-4 (split of a stored layout):"
                      << describePane(paneFor(QStringLiteral("terminal-4")));
    QVERIFY2(verdict.isEmpty(),
             qPrintable(QStringLiteral("a terminal split into a STORED layout %1 | "
                                       "factory=%2 layouts=%3")
                            .arg(verdict, factoryErrors.join(QLatin1Char(' ')),
                                 layoutErrors.join(QLatin1Char(' ')))));

    // ---- the pane a CLOSE-then-SPLIT produces ------------------------------
    // Closing a pane frees its slot in the tree; the next split is the user's
    // very next action. The pane it produces must reach a shell of its OWN and
    // never the closed pane's still-running one.
    phase = QStringLiteral("close then split");
    const QString retiredTarget =
        paneFor(QStringLiteral("terminal-4"))->property("tmuxTarget").toString();
    QVERIFY(!retiredTarget.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(paneFor(QStringLiteral("terminal-4")), "closeAndKill"));
    QTRY_VERIFY_WITH_TIMEOUT(paneFor(QStringLiteral("terminal-4")) == nullptr, kOpTimeoutMs);

    // The label the split hands out is not predictable — ch::SessionLayouts
    // burns a suffix per split rather than recycling one — so the new pane is
    // found as the id the cache gained.
    const QStringList beforeSplit = cachedPaneIds().split(QLatin1Char(','));
    QVERIFY(QMetaObject::invokeMethod(
        probeObject, "requestSplit",
        Q_ARG(QVariant, QVariant::fromValue(paneFor(QStringLiteral("terminal-1")))),
        Q_ARG(QVariant, QStringLiteral("horizontal"))));
    QString afterClosePane;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const QStringList now = cachedPaneIds().split(QLatin1Char(','));
            for (const QString& id : now) {
                if (!id.isEmpty() && !beforeSplit.contains(id)) {
                    afterClosePane = id;
                    return true;
                }
            }
            return false;
        }(),
        kOpTimeoutMs);
    verdict = whyPaneIsNotLive(probeObject, paneFor(afterClosePane),
                               runTag + QStringLiteral("F"));
    qInfo().noquote() << afterClosePane << "(split after a close):"
                      << describePane(paneFor(afterClosePane));
    QVERIFY2(verdict.isEmpty(),
             qPrintable(QStringLiteral("the terminal a split after a CLOSE created %1 | "
                                       "factory=%2 layouts=%3")
                            .arg(verdict, factoryErrors.join(QLatin1Char(' ')),
                                 layoutErrors.join(QLatin1Char(' ')))));
    QVERIFY2(paneFor(afterClosePane)->property("tmuxTarget").toString() != retiredTarget,
             "the new pane adopted the closed pane's remote session");

    const QStringList sessionOnePanes = cachedPaneIds().split(QLatin1Char(','));
    phase = QStringLiteral("session 1 final teardown");
    for (const QString& paneId : sessionOnePanes)
        killPane(paneId);
    QTest::qWait(1000);

    // ---- splits that RACE the mints ----------------------------------------
    // Every phase above let a pane come all the way up before the next one was
    // created. A user does not: they open a Dev Session and split it at once,
    // while the seeded pair's `terminal_panes` rows are still on the wire and
    // the panes are still showing "Setting up this terminal on the server…".
    phase = QStringLiteral("raced session open");
    controller.activateSession(sessionThree);
    QTRY_COMPARE_WITH_TIMEOUT(terminalRegion->property("devSessionId").toString(),
                              sessionThree, kOpTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(paneFor(QStringLiteral("terminal-1")) != nullptr, kOpTimeoutMs);
    // No settling wait: the split is requested against a pane that may not yet
    // have an identity, let alone a shell.
    phase = QStringLiteral("raced splits");
    QVERIFY(QMetaObject::invokeMethod(
        probeObject, "requestSplit",
        Q_ARG(QVariant, QVariant::fromValue(paneFor(QStringLiteral("terminal-1")))),
        Q_ARG(QVariant, QStringLiteral("vertical"))));
    QTRY_VERIFY_WITH_TIMEOUT(paneFor(QStringLiteral("terminal-3")) != nullptr, kOpTimeoutMs);
    // ...and again, immediately, on the pane that split just produced.
    QVERIFY(QMetaObject::invokeMethod(
        probeObject, "requestSplit",
        Q_ARG(QVariant, QVariant::fromValue(paneFor(QStringLiteral("terminal-3")))),
        Q_ARG(QVariant, QStringLiteral("horizontal"))));
    QTRY_VERIFY_WITH_TIMEOUT(paneFor(QStringLiteral("terminal-4")) != nullptr, kOpTimeoutMs);
    qInfo().noquote() << "raced session pane cache:" << cachedPaneIds();

    const QStringList racedPanes = cachedPaneIds().split(QLatin1Char(','));
    for (const QString& paneId : racedPanes) {
        verdict = whyPaneIsNotLive(probeObject, paneFor(paneId),
                                   runTag + QStringLiteral("G") + paneId.right(1));
        qInfo().noquote() << "raced" << paneId << ":" << describePane(paneFor(paneId));
        QVERIFY2(verdict.isEmpty(),
                 qPrintable(QStringLiteral("%1, created while the mints were still in "
                                           "flight, %2 | factory=%3 layouts=%4")
                                .arg(paneId, verdict, factoryErrors.join(QLatin1Char(' ')),
                                     layoutErrors.join(QLatin1Char(' ')))));
    }

    // ---- a pane whose remote session was killed ----------------------------
    // A killed pane says "Session killed. Connect to start a new one." Both
    // halves of that sentence are the user's only way out of a dead terminal,
    // and both are checked here: nothing may bring the session back behind
    // their back, and the pane's own control must bring a new one up. A pane
    // that fails this is blank, takes no keyboard, and cannot be recovered by
    // anything the user can reach.
    phase = QStringLiteral("kill then recover");
    QObject* killed = paneFor(QStringLiteral("terminal-4"));
    QVERIFY(killed != nullptr);
    const qsizetype errorsBeforeKill = factoryErrors.size();
    QVERIFY(QMetaObject::invokeMethod(killed, "killSession"));
    QTest::qWait(3000);
    qInfo().noquote() << "after kill:" << describePane(killed);
    // Both halves are collected rather than asserted one at a time, so a run
    // against a broken pane reports the whole failure instead of stopping at
    // its first symptom.
    QStringList killVerdicts;
    if (factoryErrors.size() != errorsBeforeKill) {
        killVerdicts << QStringLiteral("the killed pane went on attacking a target the "
                                       "factory no longer authorises: %1")
                            .arg(factoryErrors.mid(errorsBeforeKill)
                                     .join(QLatin1Char(' ')));
    }

    QVariant clicked;
    QVERIFY(QMetaObject::invokeMethod(probeObject, "clickRecovery",
                                      Q_RETURN_ARG(QVariant, clicked),
                                      Q_ARG(QVariant, QVariant::fromValue(killed))));
    QCOMPARE(clicked.toString(), QStringLiteral("CLICKED"));
    verdict = whyPaneIsNotLive(probeObject, killed, runTag + QStringLiteral("H"));
    qInfo().noquote() << "after recovery:" << describePane(killed);
    if (!verdict.isEmpty()) {
        killVerdicts << QStringLiteral("the pane's own reconnect control did not bring "
                                       "the terminal back: %1")
                            .arg(verdict);
    }
    QVERIFY2(killVerdicts.isEmpty(),
             qPrintable(killVerdicts.join(QStringLiteral("\n  | "))
                        + QStringLiteral("\n  | factory=%1")
                              .arg(factoryErrors.mid(errorsBeforeKill)
                                       .join(QLatin1Char(' ')))));

    // Teardown is expected to be noisy: kill() clears a pane's authorization,
    // and a resolution still travelling for a pane being torn down is reported
    // as a failure. Anything raised while a pane was coming UP is not noise.
    QStringList realErrors;
    for (const QString& text : factoryErrors + layoutErrors) {
        if (!text.contains(QStringLiteral("teardown]")))
            realErrors << text;
    }
    qInfo().noquote() << "all terminal diagnostics:\n"
                      << (factoryErrors + layoutErrors).join(QLatin1Char('\n'));

    phase = QStringLiteral("raced teardown");
    for (const QString& paneId : racedPanes)
        killPane(paneId);
    QTest::qWait(1000);

    QVERIFY2(realErrors.isEmpty(), qPrintable(realErrors.join(QLatin1Char('\n'))));
    QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join(QLatin1Char('\n'))));
}

QString TstLiveShell::describePane(QObject* pane)
{
    if (!pane)
        return QStringLiteral("<the region minted no pane Item for this leaf>");
    const auto flag = [pane](const char* name) {
        return pane->property(name).toBool() ? QStringLiteral("yes") : QStringLiteral("no");
    };
    return QStringLiteral("paneId=%1 terminalPaneId=\"%2\" legacy=%3 tmuxTarget=\"%4\" "
                          "pageLoaded=%5 attached=%6 connectionState=%7 "
                          "identityStalled=%8 live=%9")
               .arg(pane->property("paneId").toString(),
                    pane->property("terminalPaneId").toString(), flag("terminalLegacy"),
                    pane->property("tmuxTarget").toString(), flag("pageLoaded"),
                    flag("attached"), pane->property("connectionState").toString(),
                    flag("identityStalled"), flag("live"))
           + QStringLiteral(" statusText=\"%1\" size=%2x%3 visible=%4")
                 .arg(pane->property("statusText").toString())
                 .arg(pane->property("width").toReal())
                 .arg(pane->property("height").toReal())
                 .arg(flag("visible"));
}

QString TstLiveShell::paneJs(QObject* probe, QObject* pane, const QString& script,
                             int timeoutMs)
{
    if (!pane)
        return QStringLiteral("NO_PANE");
    if (!QMetaObject::invokeMethod(probe, "evalJs",
                                   Q_ARG(QVariant, QVariant::fromValue(pane)),
                                   Q_ARG(QVariant, script))) {
        return QStringLiteral("NO_PROBE");
    }
    if (!QTest::qWaitFor([probe] { return probe->property("jsFinished").toBool(); },
                         timeoutMs)) {
        return QStringLiteral("JS_TIMEOUT");
    }
    return probe->property("jsResult").toString();
}

bool TstLiveShell::paneScreenShows(QObject* probe, QObject* pane, const QString& line,
                                   const QString& needle, int timeoutMs)
{
    // The pasted text is a JS string literal, so it is quoted by the JSON
    // writer rather than by hand: the command carries shell quoting of its own.
    const QByteArray quoted =
        QJsonDocument(QJsonArray{line}).toJson(QJsonDocument::Compact);
    const QString paste = QString::fromLatin1(kJsPasteTemplate)
                              .arg(QString::fromUtf8(quoted.mid(1, quoted.size() - 2)));

    QElapsedTimer clock;
    clock.start();
    qint64 nextPaste = 0;
    QString screen;
    while (clock.elapsed() < timeoutMs) {
        if (clock.elapsed() >= nextPaste) {
            const QString pasted = paneJs(probe, pane, paste);
            if (pasted != QStringLiteral("PASTED")) {
                qWarning().noquote() << "paste probe failed:" << pasted;
                return false;
            }
            // Only the INSERTION is a paste: readline's bracketed paste treats
            // a pasted newline as literal text, so the submit is a real keydown.
            const QString submitted = paneJs(probe, pane, QString::fromLatin1(kJsPressEnter));
            if (submitted != QStringLiteral("SENT")) {
                qWarning().noquote() << "enter probe failed:" << submitted;
                return false;
            }
            nextPaste = clock.elapsed() + 5000;
        }
        QTest::qWait(250);
        screen = paneJs(probe, pane, QString::fromLatin1(kJsScreenText));
        if (screen.contains(needle))
            return true;
    }
    qWarning().noquote() << "screen never showed" << needle << "last:" << screen.right(400);
    return false;
}

QString TstLiveShell::whyPaneIsNotLive(QObject* probe, QObject* pane, const QString& marker)
{
    if (!pane)
        return QStringLiteral("has no pane Item at all");

    // 0. GEOMETRY, before anything else. Every property below can read
    // perfectly healthy on a pane the user cannot see and cannot click: a leaf
    // that never received a size along its split axis renders nothing and
    // swallows no input, which is exactly "blank and can't be interacted
    // with". The bundle also fits xterm to the element, so a zero-extent pane
    // sizes its remote PTY to nothing.
    if (!QTest::qWaitFor(
            [pane] {
                return pane->property("width").toReal() > 0
                       && pane->property("height").toReal() > 0
                       && pane->property("visible").toBool();
            },
            kOpTimeoutMs)) {
        return QStringLiteral("has no extent on screen: ") + describePane(pane);
    }

    // 1. The PTY. The pane attaches ITSELF once its identity and session
    // context arrive; nothing here pokes attachNow().
    if (!QTest::qWaitFor([pane] { return pane->property("attached").toBool(); },
                         kPaneTimeoutMs)) {
        return QStringLiteral("never attached a shell: ") + describePane(pane);
    }
    if (pane->property("tmuxTarget").toString().isEmpty())
        return QStringLiteral("attached without a server-minted tmux target: ")
               + describePane(pane);

    // 2. The renderer's document.
    if (!QTest::qWaitFor([pane] { return pane->property("pageLoaded").toBool(); },
                         kPaneTimeoutMs)) {
        return QStringLiteral("the packaged xterm.js page never loaded: ") + describePane(pane);
    }

    // 3. The bridge and the mount. Asked of the page directly, because "blank"
    // has three completely different causes — never loaded, loaded without a
    // WebChannel bridge, mounted but never fed — and they have different fixes.
    QString bridge;
    if (!QTest::qWaitFor(
            [this, probe, pane, &bridge] {
                bridge = paneJs(probe, pane, QString::fromLatin1(kJsBridge));
                return bridge.contains(QStringLiteral("hook=yes"));
            },
            kPaneTimeoutMs)) {
        return QStringLiteral("the page never mounted a terminal (%1): ").arg(bridge)
               + describePane(pane);
    }

    // 4. The page's own status strip, which is what the user reads.
    QString status;
    if (!QTest::qWaitFor(
            [this, probe, pane, &status] {
                status = paneJs(probe, pane, QString::fromLatin1(kJsStatus));
                return status.contains(ch::toString(ch::TerminalState::Ready));
            },
            kPaneTimeoutMs)) {
        return QStringLiteral("the page's status strip never reached ready (last \"%1\"): ")
                   .arg(status)
               + describePane(pane);
    }

    // 5. Content on xterm's screen: a blank pane is exactly the absence of this.
    QString screen;
    if (!QTest::qWaitFor(
            [this, probe, pane, &screen] {
                screen = paneJs(probe, pane, QString::fromLatin1(kJsScreenText));
                return !screen.trimmed().isEmpty()
                       && screen != QStringLiteral("NO_HOOK")
                       && screen != QStringLiteral("NO_SCREEN");
            },
            kPaneTimeoutMs)) {
        return QStringLiteral("xterm's screen stayed empty (\"%1\"): ").arg(screen)
               + describePane(pane);
    }

    // 5b. ...and the grid that carries it has a size. This is the difference
    // between "xterm holds a screenful" and "the user can see one".
    QString grid;
    if (!QTest::qWaitFor(
            [this, probe, pane, &grid] {
                grid = paneJs(probe, pane, QString::fromLatin1(kJsGrid));
                return grid.startsWith(QStringLiteral("OK "));
            },
            kPaneTimeoutMs)) {
        return QStringLiteral("the terminal has no drawable grid (%1): ").arg(grid)
               + describePane(pane);
    }
    qInfo().noquote() << "grid" << pane->property("paneId").toString() << grid;

    // 6. Keyboard. printf splices the marker on the REMOTE side, so xterm
    // echoing our own keystrokes cannot satisfy it.
    const QString command = QStringLiteral("printf '%s_%s\\n' CH_LIVEPANE ") + marker;
    const QString needle = QStringLiteral("CH_LIVEPANE_") + marker;
    if (!paneScreenShows(probe, pane, command, needle, kPaneTimeoutMs)) {
        return QStringLiteral("a keystroke driven from the page never reached the shell "
                              "(no %1 on screen): ")
                   .arg(needle)
               + describePane(pane);
    }
    return {};
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
