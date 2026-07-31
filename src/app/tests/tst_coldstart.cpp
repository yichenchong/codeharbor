// THE COLD-START ACCEPTANCE GATE (docs/PLAN.md, SPEC 4.1/4.5/5.1/8.1/12.1).
//
// Every other live gate starts from a machine that is already connected: it
// builds a SessionBootstrap by hand, hands it CH_LIVE_* out of the environment,
// and asserts on one subsystem. This one starts where a real person starts —
// a freshly installed client with an EMPTY config directory that has never
// heard of a server — and walks the whole way to working:
//
//   1. first run                -> the connect sheet is on screen, not a dead shell
//   2. add a server             -> through the sheet's own fields and buttons
//   3. host key                 -> unknown key prompted, accepted, PERSISTED
//   4. workspace                -> group + Dev Session, activated from the sidebar
//   5. terminal                 -> the pane in the tree runs a REAL remote shell
//   6. editor                   -> a real remote file edited and saved to disk
//   7. relaunch                 -> auto-connect, same session, same layout, same widths
//
// Ground rules that make this a gate rather than another integration test:
//
//   * The object graph is EXACTLY the one src/app/main.cpp builds — same
//     objects, same wiring order, same context property names — and the QML is
//     the real qrc:/qt/qml/CodeHarbor/Main.qml. Nothing is stubbed.
//   * The app is driven through its UI objects: the sheet's fields and its
//     Save/Connect/Accept buttons, the sidebar's sessionActivated signal, the
//     pane's own controller/bridge. Not through private C++ shortcuts.
//   * NO CH_LIVE_* VARIABLE REACHES THE APP. main() below reads them once and
//     then qunsetenv()s every one of them, so the graph built afterwards cannot
//     fall back on the environment for anything: the host, port, user, node
//     path and repo root can only come from the profile the test typed into the
//     sheet, and the known_hosts store can only be the default one under the
//     fresh config dir. If any of that were still env-driven, this file would
//     fail to connect at all.
//   * QSettings/QStandardPaths are redirected to a throwaway XDG_CONFIG_HOME
//     before anything exists, so the developer's ~/.config/CodeHarbor is never
//     read or written.
//   * Remote effects are verified OUT OF BAND: a plain SSH exec channel running
//     `cat`/`tmux has-session`, never the same RPC path that produced them.
//   * A step that catches the product misbehaving REPORTS it and keeps walking.
//     Steps 3 and 4 observe their hardest verdict early (was the unknown host
//     key prompted for? did the sidebar draw the row?) and assert it at the END
//     of the step, so one defect costs one verdict instead of hiding the four
//     behind it. Every step is independent for the same reason: a red step 4
//     must not turn steps 5-7 into "unknown".
//   * A marker that proves the remote ran something is built so the pane
//     ECHOING our keystrokes cannot produce it (step 5). An assertion a local
//     echo can satisfy proves nothing about the far end.
//
// Skipped wholesale unless CH_LIVE_SSH is set (initTestCase QSKIPs, which skips
// every case), so the default suite stays green on a machine with no fixture.

#include "AgentStatusMonitor.h"
#include "AppController.h"
#include "CodeharbordClient.h"
#include "EditorController.h"
#include "EditorFactory.h"
#include "Notifier.h"
#include "ServerProfiles.h"
#include "SessionBootstrap.h"
#include "SessionLayouts.h"
#include "SessionsModel.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"
#include "TerminalBridge.h"
#include "TerminalController.h"
#include "TerminalFactory.h"
#include "UiStateStore.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonObject>
#include <QJsonValue>
#include <QJSValue>
#include <QMetaObject>
#include <QModelIndex>
#include <QPoint>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantMap>
#include <QtQuickControls2/QQuickStyle>
#include <QtTest/QtTest>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include <functional>
#include <memory>
#include <optional>

namespace {

// A cold remote `node` start also type-strips the TypeScript entry point.
constexpr int kConnectTimeoutMs = 90000;
// A warm RPC round trip.
constexpr int kOpTimeoutMs = 30000;
// SSH handshake + cold tmux server + login shell inside the pane.
constexpr int kAttachTimeoutMs = 60000;
// Typing a line into a live pane and seeing the result come back.
constexpr int kCommandTimeoutMs = 45000;
// A one-shot out-of-band exec channel.
constexpr int kExecTimeoutMs = 20000;
// Re-type an unanswered command this often: the first keystrokes can land
// before the shell in a freshly created tmux session has readline up.
constexpr int kRetypeIntervalMs = 5000;
// How long the WebEngine renderer is given to mount and shake hands.
constexpr int kRendererTimeoutMs = 45000;

// Organisation/application names main.cpp installs; QSettings' native scope is
// addressed through them.
const char kOrganization[] = "CodeHarbor";
const char kApplication[] = "CodeHarbor";

// Captured by main() BEFORE the variables are removed from the environment.
struct LiveEnv {
    bool present = false;
    QString host;
    quint16 port = 0;
    QString user;
    QString node;
    QString repo;
};
LiveEnv g_live;

// Throwaway config root, installed as XDG_CONFIG_HOME by main().
QString g_configHome;

// QObject behind a QML property, whether it arrived as a raw pointer or boxed
// in a QJSValue (a `property var` holding a QObject can be either).
QObject* asObject(const QVariant& value)
{
    if (value.canConvert<QObject*>())
        return value.value<QObject*>();
    if (value.canConvert<QJSValue>())
        return value.value<QJSValue>().toQObject();
    return nullptr;
}

// Every item at or under `root`, walking BOTH the QObject child list and the
// visual child list.
//
// Neither list alone reaches every delegate, and the gap is not cosmetic: a
// declaratively nested item is a QObject child of its parent, but an item a
// QQmlDelegateModel produced (every ListView delegate, every Repeater delegate)
// is only ever the VISUAL child of the view's content item — the delegate model
// keeps the ownership and never reparents the QObject. QObject::findChildren()
// therefore cannot see a single sidebar row, which is precisely what the
// sessions list is made of.
void collectItems(QObject* node, QSet<QObject*>& seen, QList<QQuickItem*>& out)
{
    if (!node || seen.contains(node))
        return;
    seen.insert(node);

    auto* item = qobject_cast<QQuickItem*>(node);
    if (item)
        out.append(item);

    const QObjectList objectChildren = node->children();
    for (QObject* child : objectChildren)
        collectItems(child, seen, out);
    if (item) {
        const QList<QQuickItem*> visualChildren = item->childItems();
        for (QQuickItem* child : visualChildren)
            collectItems(child, seen, out);
    }
}

QList<QQuickItem*> allItems(QObject* root)
{
    QSet<QObject*> seen;
    QList<QQuickItem*> out;
    collectItems(root, seen, out);
    return out;
}

// The walkthrough phase currently running. Every error the app raises is
// stamped with it, because an unattributed list of error strings is what turned
// "four toasts happened somewhere in seven steps" into "the session is not
// wired at step 4". It is not: the first two of those four are raised BEFORE
// the first connect and BY the deliberate host-key refusal respectively.
QString g_phase = QStringLiteral("construct");

QString stamped(const QString& text)
{
    return QLatin1Char('[') + g_phase + QStringLiteral("] ") + text;
}

QString poolStateName(ch::SshConnectionPool::State state)
{
    switch (state) {
    case ch::SshConnectionPool::State::Disconnected:   return QStringLiteral("Disconnected");
    case ch::SshConnectionPool::State::Connecting:     return QStringLiteral("Connecting");
    case ch::SshConnectionPool::State::HostKeyCheck:   return QStringLiteral("HostKeyCheck");
    case ch::SshConnectionPool::State::Authenticating: return QStringLiteral("Authenticating");
    case ch::SshConnectionPool::State::Connected:      return QStringLiteral("Connected");
    case ch::SshConnectionPool::State::Error:          return QStringLiteral("Error");
    case ch::SshConnectionPool::State::NotAvailable:   return QStringLiteral("NotAvailable");
    }
    return QStringLiteral("?");
}

// The fingerprint of a stored known_hosts line, derived the same way
// AppController's host-key callback derives the one it compares against:
// base64(SHA-256(key blob)) with trailing '=' dropped. Lets the gate prove that
// the key the retry PINNED and the store PERSISTED is byte-for-byte the key the
// user was shown, rather than merely "some key ended up trusted".
QString fingerprintOfKnownHostsLine(const QString& line)
{
    const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (fields.size() < 3)
        return {};
    const QByteArray blob = QByteArray::fromBase64(fields.at(2).toLatin1());
    if (blob.isEmpty())
        return {};
    return QString::fromLatin1(
        QCryptographicHash::hash(blob, QCryptographicHash::Sha256)
            .toBase64(QByteArray::OmitTrailingEquals));
}

// First descendant whose QML type name starts with `prefix`. QML types compiled
// into a module keep their file name as the class name (possibly with a
// _QMLTYPE_n suffix), so this addresses the type, not an id nobody assigned.
QQuickItem* findByType(QObject* root, const QString& prefix)
{
    const QList<QQuickItem*> items = allItems(root);
    for (QQuickItem* item : items) {
        if (item != root
            && QString::fromLatin1(item->metaObject()->className()).startsWith(prefix))
            return item;
    }
    return nullptr;
}

// First descendant carrying this objectName. The sidebar delegates name
// themselves after the row they render ("sessionRow:<id>"), so this addresses
// the row that is actually on screen.
QQuickItem* findByName(QObject* root, const QString& objectName)
{
    const QList<QQuickItem*> items = allItems(root);
    for (QQuickItem* item : items) {
        if (item->objectName() == objectName)
            return item;
    }
    return nullptr;
}

QStringList typeNamesOf(QObject* root)
{
    QStringList names;
    const QList<QQuickItem*> items = allItems(root);
    for (QQuickItem* item : items)
        names << QString::fromLatin1(item->metaObject()->className());
    return names;
}

// The named items only: the useful half of a failure dump when the thing that
// is missing is addressed by name rather than by type.
QStringList objectNamesOf(QObject* root)
{
    QStringList names;
    const QList<QQuickItem*> items = allItems(root);
    for (QQuickItem* item : items) {
        if (!item->objectName().isEmpty())
            names << item->objectName();
    }
    return names;
}

// One JSON-RPC round trip driven to completion on the caller's event loop, used
// only for out-of-band bookkeeping (cleanup) — never to produce a result the
// walkthrough then asserts on.
struct RawRpc {
    QJsonValue result;
    std::optional<ch::RpcError> error;
    bool done = false;

    bool call(ch::CodeharbordClient& client, const QString& method,
              const QJsonObject& params, int timeoutMs = kOpTimeoutMs)
    {
        client.call(method, params,
                    [this](QJsonValue value, std::optional<ch::RpcError> err) {
                        result = value;
                        error = err;
                        done = true;
                    });
        if (!QTest::qWaitFor([this] { return done; }, timeoutMs))
            return false;
        return !error.has_value();
    }
};

// EXACTLY src/app/main.cpp's object graph: same members, same construction
// order (so destruction order matches too), same wiring, same context property
// names, and the real Main.qml on top.
struct AppGraph {
    ch::CodeharbordClient client;
    ch::AgentStatusMonitor monitor;
    ch::SshConnectionPool pool;
    ch::SessionBootstrap bootstrap;
    ch::AppController app;
    ch::ViewerProfiles viewerProfiles;
    ch::ViewerModel viewers;
    ch::ServerProfiles serverProfiles;
    ch::SessionLayouts layouts;
    ch::EditorFactory editorFactory;
    ch::TerminalFactory terminalFactory;
    ch::Notifier notifier;
    QQmlApplicationEngine engine;

    QStringList qmlWarnings;
    QStringList appErrors;
    QStringList bootstrapErrors;
    // True if connectAndWireFromEnvironment() did anything, which with the
    // CH_LIVE_* variables removed would mean the app still reaches the
    // environment behind the user's back.
    bool wiredFromEnvironment = false;

    AppGraph()
        : bootstrap(&pool, &client, &monitor)
        , app(&client)
        , viewerProfiles(&client)
        , viewers(&client)
        , layouts(app.workspaceDb(), app.uiState())
        , editorFactory(&client)
        , terminalFactory(&pool)
    {
        QObject::connect(&bootstrap, &ch::SessionBootstrap::error, &bootstrap,
                         [this](const QString& text) { bootstrapErrors << stamped(text); });
        QObject::connect(&app, &ch::AppController::error, &app,
                         [this](const QString& text) { appErrors << stamped(text); });

        // --- main.cpp, line for line ------------------------------------
        wiredFromEnvironment = bootstrap.connectAndWireFromEnvironment();

        app.setAgentMonitor(&monitor);
        viewers.setProfiles(&viewerProfiles);
        app.setConnection(&pool, &bootstrap, &serverProfiles, &layouts);
        QObject::connect(&monitor, &ch::AgentStatusMonitor::notify, &notifier,
                         &ch::Notifier::notify);

        // Engine-level QML warnings only: binding loops, type errors, undefined
        // property reads — everything Main.qml and its children can raise while
        // this walkthrough drives them.
        //
        // NOT a whole-process warning net. qmlWarning() raised from a
        // C++-constructed object never reaches QQmlEngine::warnings (it has no
        // QML file to blame), so the Qt 6.9 "Please use WebEngineProfilePrototype"
        // notice this gate prints as a QWARN in steps 1 and 7 is invisible here.
        // That polarity-inverted net — fail on every warning-or-worse message
        // outside a closed environmental allowlist — is tst_qmlload's contract,
        // and it is the file to extend if this class of warning needs to be
        // fatal; duplicating it here would fork the allowlist.
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [this](const QList<QQmlError>& warnings) {
                             for (const QQmlError& error : warnings)
                                 qmlWarnings << error.toString();
                         });

        // Exactly what main.cpp publishes, no more: QML reaches the layouts
        // through `app.layouts`, and the Notifier is a C++-only sink.
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("viewers"), &viewers);
        engine.rootContext()->setContextProperty(QStringLiteral("agentMonitor"), &monitor);
        engine.rootContext()->setContextProperty(QStringLiteral("editorFactory"),
                                                 &editorFactory);
        engine.rootContext()->setContextProperty(QStringLiteral("terminalFactory"),
                                                 &terminalFactory);

        engine.loadFromModule("CodeHarbor", "Main");
    }

    QQuickWindow* window() const
    {
        if (engine.rootObjects().isEmpty())
            return nullptr;
        return qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    }

    // Evaluate an expression in Main.qml's own context, so ids Main.qml
    // declares (outer, sidebarRegion, terminalRegion, connectSheet) resolve.
    QVariant eval(const QString& expression) const
    {
        QObject* root = engine.rootObjects().isEmpty()
                            ? nullptr
                            : engine.rootObjects().constFirst();
        if (!root)
            return {};
        QQmlExpression expr(qmlContext(root), root, expression);
        const QVariant value = expr.evaluate();
        return expr.hasError() ? QVariant(expr.error().toString()) : value;
    }

    QObject* evalObject(const QString& expression) const
    {
        return asObject(eval(expression));
    }
};

} // namespace

class TstColdStart : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void step1_firstRunOffersTheConnectSheet();
    void step2_addAServerThroughTheSheet();
    void step3_hostKeyIsPromptedAcceptedAndPersisted();
    void step4_workspaceAndSessionActivation();
    void step5_terminalPaneRunsARemoteShell();
    void step6_editorPaneSavesToTheRemoteDisk();
    void step7_relaunchRestoresTheSession();

private:
    void buildGraph();
    QByteArray runExec(const QString& command, int timeoutMs = kExecTimeoutMs);
    bool waitForConnected(int timeoutMs);
    // Everything the pane's renderer has received, plus everything the
    // controller retained while no renderer was mounted, so a headless page
    // that never shakes hands cannot hide the remote bytes.
    QString paneText() const;
    bool typeUntil(const QByteArray& command, const std::function<bool()>& done,
                   int timeoutMs);
    QString knownHostsPath() const;
    QString configFilePath() const;
    QString readFileText(const QString& path) const;
    // One rung of the connect ladder: everything needed to tell a refused
    // attempt, a parked prompt and a live session apart from each other.
    QString ladder(const QString& rung) const;

    std::unique_ptr<AppGraph> m_graph;

    QString m_runId;
    QString m_profileId;
    QString m_groupName;
    QString m_sessionName;
    QString m_groupId;
    QString m_sessionId;
    QString m_serverId;

    // Step 3 observations, asserted at the end of that step so the walkthrough
    // still reaches the later steps when the prompt never fires.
    bool m_hostKeyPrompted = false;
    QString m_promptedFingerprint;
    bool m_promptedOnSecondConnect = false;
    QString m_knownHostsAfterAccept;

    // Step 4 observations, deferred for the same reason.
    bool m_sidebarRowOnScreen = false;
    QString m_sidebarRowLabel;
    QString m_sidebarRowReport;

    // Step 5.
    QQuickItem* m_terminalPane = nullptr;
    ch::TerminalController* m_terminalController = nullptr;
    ch::TerminalBridge* m_terminalBridge = nullptr;
    QString m_rendered;

    // Step 6.
    QString m_remoteFile;

    // Step 7.
    int m_draggedSidebar = 0;
    int m_draggedTerminal = 0;
    QVariant m_viewerTreeBefore;
    QVariant m_terminalTreeBefore;
};

void TstColdStart::initTestCase()
{
    if (!g_live.present)
        QSKIP("CH_LIVE_SSH is not set; the cold-start gate is skipped");
    if (!ch::SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; the cold-start gate is skipped");

    QVERIFY2(!g_live.host.isEmpty() && g_live.port != 0 && !g_live.user.isEmpty()
                 && !g_live.node.isEmpty() && !g_live.repo.isEmpty(),
             "CH_LIVE_HOST/PORT/USER/NODE/REPO must all be set");

    // The config dir really is empty: no profiles, no known_hosts, no UI state.
    QVERIFY2(!QFileInfo::exists(configFilePath()),
             qPrintable(configFilePath() + QStringLiteral(" already exists")));
    QVERIFY2(!QFileInfo::exists(knownHostsPath()),
             qPrintable(knownHostsPath() + QStringLiteral(" already exists")));

    // Nothing in this process may still see the live environment: the app is
    // about to be built and must reach the server on typed-in settings alone.
    for (const char* key : {"CH_LIVE_SSH", "CH_LIVE_HOST", "CH_LIVE_PORT",
                            "CH_LIVE_USER", "CH_LIVE_NODE", "CH_LIVE_REPO",
                            "CH_LIVE_KNOWN_HOSTS"}) {
        QVERIFY2(qEnvironmentVariableIsEmpty(key),
                 qPrintable(QStringLiteral("%1 still set").arg(QLatin1String(key))));
    }

    m_runId = QStringLiteral("%1x%2")
                  .arg(QCoreApplication::applicationPid())
                  .arg(QDateTime::currentMSecsSinceEpoch() % 1000000);
    m_groupName = QStringLiteral("Cold start %1").arg(m_runId);
    m_sessionName = QStringLiteral("Session %1").arg(m_runId);
    m_remoteFile = QStringLiteral("/tmp/ch_coldstart_%1.txt").arg(m_runId);

    qInfo().noquote() << "cold start: fresh config home =" << g_configHome;
    qInfo().noquote() << "cold start: target =" << g_live.user + QLatin1Char('@')
                                                       + g_live.host
                                                       + QLatin1Char(':')
                                                       + QString::number(g_live.port);
}

void TstColdStart::cleanupTestCase()
{
    if (!m_graph)
        return;

    // Everything this run created goes away, and the remote is asked whether it
    // did. Terminal panes first: the tmux session outlives the client on
    // purpose, so nothing else would ever reap it.
    if (m_graph->pool.state() == ch::SshConnectionPool::State::Connected) {
        if (m_terminalController)
            m_graph->terminalFactory.kill(m_terminalController);
        // BOTH default panes: a new Dev Session now comes up with terminal-1
        // above terminal-2 (SessionLayouts::defaultTree), so a single-target
        // kill would leave the lower pane's tmux session running on the shared
        // fixture forever.
        if (!m_sessionId.isEmpty()) {
            const QStringList paneIds{QStringLiteral("terminal-1"),
                                      QStringLiteral("terminal-2")};
            for (const QString& paneId : paneIds) {
                const QString target = ch::TerminalController::tmuxTarget(
                    ch::DevSessionId{m_sessionId}, ch::TerminalId{paneId});
                qInfo().noquote()
                    << "cleanup tmux:" << paneId
                    << runExec(QStringLiteral("tmux kill-session -t '%1' >/dev/null 2>&1; "
                                              "tmux has-session -t '%1' >/dev/null 2>&1 "
                                              "&& echo ALIVE || echo GONE")
                                   .arg(target))
                           .trimmed();
            }
        }
        if (!m_remoteFile.isEmpty()) {
            runExec(QStringLiteral("rm -f '%1'; "
                                   "rm -rf \"${XDG_DATA_HOME:-$HOME/.local/share}/codeharbor/recovery\"")
                        .arg(m_remoteFile));
        }
        if (!m_groupId.isEmpty()) {
            RawRpc removed;
            removed.call(m_graph->client, QStringLiteral("workspace.deleteGroup"),
                         {{QStringLiteral("id"), m_groupId}});
        }
    }

    m_graph.reset();
}

void TstColdStart::buildGraph()
{
    m_graph = std::make_unique<AppGraph>();
    QVERIFY2(!m_graph->engine.rootObjects().isEmpty(),
             qPrintable(QStringLiteral("Main.qml did not load:\n%1")
                            .arg(m_graph->qmlWarnings.join(QLatin1Char('\n')))));
    QQuickWindow* window = m_graph->window();
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
}

QString TstColdStart::knownHostsPath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .filePath(QStringLiteral("known_hosts"));
}

QString TstColdStart::configFilePath() const
{
    return g_configHome + QLatin1Char('/') + QLatin1String(kOrganization)
           + QLatin1Char('/') + QLatin1String(kApplication) + QStringLiteral(".conf");
}

QString TstColdStart::readFileText(const QString& path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString TstColdStart::ladder(const QString& rung) const
{
    return QStringLiteral(
               "  %1 | app=%2 | pool=%3 | hostKeyCallback=%4 | known_hosts=%5 bytes")
        .arg(rung.leftJustified(28), m_graph->app.connectionState(),
             poolStateName(m_graph->pool.state()),
             m_graph->pool.hostKeyCallback() ? QStringLiteral("INSTALLED")
                                             : QStringLiteral("absent"))
        .arg(QFileInfo(knownHostsPath()).size());
}

QByteArray TstColdStart::runExec(const QString& command, int timeoutMs)
{
    ch::SshChannelDevice device(&m_graph->pool, ch::SshConnectionPool::ChannelKind::Exec);
    QByteArray out;
    bool finished = false;
    connect(&device, &ch::SshChannelDevice::readyRead, &device,
            [&out, &device] { out += device.readAll(); });
    connect(&device, &ch::SshChannelDevice::readChannelFinished, &device,
            [&finished] { finished = true; });

    if (!device.startExec(command))
        return {};

    QElapsedTimer clock;
    clock.start();
    while (!finished && clock.elapsed() < timeoutMs)
        QTest::qWait(50);

    out += device.readAll();
    device.closeChannel();
    return out;
}

bool TstColdStart::waitForConnected(int timeoutMs)
{
    return QTest::qWaitFor(
        [this] { return m_graph->app.connectionState() == QLatin1String("connected"); },
        timeoutMs);
}

QString TstColdStart::paneText() const
{
    QString text = m_rendered;
    if (m_terminalController)
        text += QString::fromUtf8(m_terminalController->hiddenBuffer());
    return text;
}

bool TstColdStart::typeUntil(const QByteArray& command,
                             const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    qint64 nextSend = 0;
    while (!done() && clock.elapsed() < timeoutMs) {
        if (clock.elapsed() >= nextSend) {
            // Through the BRIDGE slot the xterm.js page calls, not the C++ API.
            m_terminalBridge->sendInput(QString::fromLatin1(command) + QLatin1Char('\n'));
            nextSend = clock.elapsed() + kRetypeIntervalMs;
        }
        QTest::qWait(100);
    }
    return done();
}

// ---------------------------------------------------------------------------
// 1. FIRST RUN. A client that has never been configured must offer the way in,
//    not a dead shell the user has no handle on.
// ---------------------------------------------------------------------------
void TstColdStart::step1_firstRunOffersTheConnectSheet()
{
    g_phase = QStringLiteral("step1 cold launch");
    buildGraph();

    // Nothing self-connected: with CH_LIVE_* gone, connectAndWireFromEnvironment
    // must be the no-op a normal desktop launch takes.
    QVERIFY2(!m_graph->wiredFromEnvironment,
             "the app wired a session from the environment with no CH_LIVE_* set");
    QCOMPARE(m_graph->pool.state(), ch::SshConnectionPool::State::Disconnected);
    QCOMPARE(m_graph->app.connectionState(), QStringLiteral("disconnected"));

    // No stored servers, because the config dir is brand new.
    QCOMPARE(m_graph->serverProfiles.profiles().size(), 0);
    QVERIFY(m_graph->serverProfiles.activeId().isEmpty());

    QObject* sheet = m_graph->evalObject(QStringLiteral("connectSheet"));
    QVERIFY2(sheet != nullptr, "Main.qml has no connectSheet");
    QTRY_VERIFY2(sheet->property("shown").toBool(),
                 "first run left the user with no way to reach a server");
    QVERIFY(sheet->property("visible").toBool());

    // ...and it is really on top of the shell, not shown behind it.
    auto* sheetItem = qobject_cast<QQuickItem*>(sheet);
    QVERIFY(sheetItem != nullptr);
    QVERIFY(sheetItem->z() > 0);
    QVERIFY(sheetItem->width() > 0 && sheetItem->height() > 0);

    QVERIFY2(m_graph->qmlWarnings.isEmpty(),
             qPrintable(m_graph->qmlWarnings.join(QLatin1Char('\n'))));
}

// ---------------------------------------------------------------------------
// 2. ADD A SERVER. Typed into the sheet's own fields and committed with its own
//    Save button, so the whole path (field -> profileSaved -> ServerProfiles ->
//    QSettings) is the one a user walks.
// ---------------------------------------------------------------------------
void TstColdStart::step2_addAServerThroughTheSheet()
{
    g_phase = QStringLiteral("step2 add server");
    QObject* sheet = m_graph->evalObject(QStringLiteral("connectSheet"));
    QVERIFY(sheet != nullptr);

    const auto field = [sheet](const char* name) {
        return sheet->findChild<QQuickItem*>(QLatin1String(name));
    };
    const auto button = [sheet](const char* name) {
        return sheet->findChild<QQuickItem*>(QLatin1String(name));
    };

    for (const char* name : {"nameField", "hostField", "portField", "userField",
                             "nodePathField", "repoRootField", "saveButton",
                             "connectButton"}) {
        QVERIFY2(sheet->findChild<QQuickItem*>(QLatin1String(name)) != nullptr,
                 qPrintable(QStringLiteral("ConnectSheet has no %1")
                                .arg(QLatin1String(name))));
    }

    field("nameField")->setProperty("text", QStringLiteral("Cold start %1").arg(m_runId));
    field("hostField")->setProperty("text", g_live.host);
    field("portField")->setProperty("text", QString::number(g_live.port));
    field("userField")->setProperty("text", g_live.user);
    field("nodePathField")->setProperty("text", g_live.node);
    field("repoRootField")->setProperty("text", g_live.repo);

    // The sheet's own validity gate, not ours.
    QVERIFY2(button("saveButton")->property("enabled").toBool(),
             "ConnectSheet refused a fully filled, valid form");
    QVERIFY(QMetaObject::invokeMethod(button("saveButton"), "clicked"));

    QTRY_COMPARE(m_graph->serverProfiles.profiles().size(), 1);
    const QVariantMap stored = m_graph->serverProfiles.profiles().constFirst().toMap();
    m_profileId = stored.value(QStringLiteral("id")).toString();
    QVERIFY(!m_profileId.isEmpty());
    QCOMPARE(stored.value(QStringLiteral("host")).toString(), g_live.host);
    QCOMPARE(stored.value(QStringLiteral("port")).toInt(), int(g_live.port));
    QCOMPARE(stored.value(QStringLiteral("user")).toString(), g_live.user);
    QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(), g_live.node);
    QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(), g_live.repo);

    // It reached disk, not just the in-memory list: this is the file the next
    // launch in step 7 reads.
    QTRY_VERIFY2(readFileText(configFilePath()).contains(g_live.host),
                 qPrintable(QStringLiteral("%1 does not mention the host:\n%2")
                                .arg(configFilePath(), readFileText(configFilePath()))));

    // The sheet re-anchored onto the profile it just created, which is what
    // makes Connect reachable at all.
    QTRY_COMPARE(sheet->property("editingId").toString(), m_profileId);
    QVERIFY2(button("connectButton")->property("enabled").toBool(),
             "ConnectSheet cannot connect to the profile it just saved");

    qInfo().noquote() << "profile saved through the sheet:" << m_profileId;
}

// ---------------------------------------------------------------------------
// 3. HOST KEY. The fixture's key is unknown to the brand-new known_hosts store,
//    so the first attempt must be REFUSED and the fingerprint shown (SPEC
//    12.1). Accepting retries, reaches "connected", and persists the key so the
//    next launch does not ask again.
//
//    The prompt assertion is deliberately LAST: if the product trusts the key
//    silently the connection still comes up, the rest of the walkthrough still
//    runs, and this step reports the real failure instead of hiding behind it.
// ---------------------------------------------------------------------------
void TstColdStart::step3_hostKeyIsPromptedAcceptedAndPersisted()
{
    g_phase = QStringLiteral("step3 connect#1");
    QVERIFY2(!m_profileId.isEmpty(), "step 2 did not produce a profile");
    QObject* sheet = m_graph->evalObject(QStringLiteral("connectSheet"));
    QVERIFY(sheet != nullptr);

    QSignalSpy promptSpy(&m_graph->app, &ch::AppController::hostKeyPrompt);
    QVERIFY(promptSpy.isValid());

    // Every rung of the connect ladder, so a failure anywhere in this step says
    // WHICH attempt broke and what the pool/callback looked like at the time.
    QStringList rungs;
    rungs << ladder(QStringLiteral("before connect #1"));

    QVERIFY(QMetaObject::invokeMethod(
        sheet->findChild<QQuickItem*>(QStringLiteral("connectButton")), "clicked"));

    // Either the key was queried or the session came straight up.
    QVERIFY2(QTest::qWaitFor(
                 [this, &promptSpy] {
                     return promptSpy.count() > 0
                            || m_graph->app.connectionState() == QLatin1String("connected")
                            || m_graph->app.connectionState() == QLatin1String("failed");
                 },
                 kConnectTimeoutMs),
             qPrintable(QStringLiteral("connect never settled; state=%1 errors=%2")
                            .arg(m_graph->app.connectionState(),
                                 m_graph->bootstrapErrors.join(QLatin1Char('|')))));
    rungs << ladder(QStringLiteral("connect #1 settled"));

    if (promptSpy.count() > 0) {
        m_hostKeyPrompted = true;
        m_promptedFingerprint = promptSpy.at(0).at(2).toString();
        qInfo().noquote() << "host key prompt:" << promptSpy.at(0).at(0).toString()
                          << promptSpy.at(0).at(1).toString() << m_promptedFingerprint;

        QCOMPARE(m_graph->app.connectionState(), QStringLiteral("hostkey"));
        QVERIFY2(!m_promptedFingerprint.isEmpty(), "prompted with an empty fingerprint");
        // The refusal really was a refusal: nothing is trusted yet.
        QVERIFY2(!QFileInfo::exists(knownHostsPath()) || QFileInfo(knownHostsPath()).size() == 0,
                 qPrintable(QStringLiteral("the refused attempt already wrote known_hosts:\n%1")
                                .arg(readFileText(knownHostsPath()))));

        // The sheet surfaced it, and the panel really is on screen.
        QTRY_VERIFY(sheet->property("pendingHostKey").isValid()
                    && !sheet->property("pendingHostKey").isNull());
        QQuickItem* panel = sheet->findChild<QQuickItem*>(QStringLiteral("hostKeyPrompt"));
        QVERIFY(panel != nullptr);
        QTRY_VERIFY(panel->isVisible());
        QQuickItem* shown = sheet->findChild<QQuickItem*>(QStringLiteral("hostKeyFingerprint"));
        QVERIFY(shown != nullptr);
        QVERIFY2(shown->property("text").toString().contains(m_promptedFingerprint),
                 qPrintable(shown->property("text").toString()));

        // --- RE-ENTRANCY PROBE ------------------------------------------
        // A parked prompt must swallow a second Connect. This is the direct
        // observable for AppController's m_connecting guard (AppController.cpp
        // 201/264): if the guard were NOT still set here, this click would
        // start a second attempt underneath the parked one and swap
        // m_pendingProfileId/m_pendingFingerprint out from under
        // resolveHostKey() — the exact mechanism by which an accept would then
        // retry with nothing pinned and be refused again.
        g_phase = QStringLiteral("step3 reentrant-connect");
        const int promptsWhileParked = promptSpy.count();
        QVERIFY(QMetaObject::invokeMethod(
            sheet->findChild<QQuickItem*>(QStringLiteral("connectButton")), "clicked"));
        QTest::qWait(250);
        rungs << ladder(QStringLiteral("re-entrant connect"));
        QVERIFY2(m_graph->app.connectionState() == QLatin1String("hostkey"),
                 qPrintable(QStringLiteral("a second Connect broke into a parked host-key "
                                           "prompt; state=%1")
                                .arg(m_graph->app.connectionState())));
        QCOMPARE(promptSpy.count(), promptsWhileParked);

        // Accept, exactly as the user clicking Accept does.
        g_phase = QStringLiteral("step3 accept+retry");
        QVERIFY(QMetaObject::invokeMethod(
            sheet->findChild<QQuickItem*>(QStringLiteral("hostKeyAcceptButton")), "clicked"));
    }

    QVERIFY2(waitForConnected(kConnectTimeoutMs),
             qPrintable(QStringLiteral("never reached connected after accepting the key.\n"
                                       "ladder:\n%1\n  %2\nbootstrap=%3\napp=%4")
                            .arg(rungs.join(QLatin1Char('\n')),
                                 ladder(QStringLiteral("failed here")),
                                 m_graph->bootstrapErrors.join(QLatin1Char('|')),
                                 m_graph->appErrors.join(QLatin1Char('|')))));
    rungs << ladder(QStringLiteral("after accept -> connected"));
    QCOMPARE(m_graph->pool.state(), ch::SshConnectionPool::State::Connected);
    QVERIFY(m_graph->client.transport() == m_graph->bootstrap.rpcDevice());

    // The RETRY consumed the pinned fingerprint instead of asking again. A
    // second prompt here is the signature of a retry that lost its approval.
    QCOMPARE(promptSpy.count(), m_hostKeyPrompted ? 1 : 0);

    // The key was persisted into the DEFAULT store under the fresh config dir —
    // no CH_LIVE_KNOWN_HOSTS could have redirected it.
    QCOMPARE(m_graph->bootstrap.knownHostsPath(), knownHostsPath());
    QVERIFY2(QFileInfo::exists(knownHostsPath()),
             qPrintable(knownHostsPath() + QStringLiteral(" was never written")));
    m_knownHostsAfterAccept = readFileText(knownHostsPath()).trimmed();
    QVERIFY2(!m_knownHostsAfterAccept.isEmpty(), "known_hosts written but empty");
    qInfo().noquote() << "known_hosts after accept:\n" << m_knownHostsAfterAccept;
    // Stored OpenSSH-style for a non-default port.
    QVERIFY2(m_knownHostsAfterAccept.contains(
                 QStringLiteral("[%1]:%2").arg(g_live.host).arg(g_live.port)),
             qPrintable(m_knownHostsAfterAccept));

    // ...and it is THE key the user was shown, not merely "a" key that ended up
    // trusted: re-derive base64(SHA-256(blob)) from the stored line and compare
    // it to the fingerprint the prompt presented. This is the assertion that
    // would catch a retry pinning or persisting a different key than the one on
    // screen. The prompt carries OpenSSH's displayed form, "SHA256:" + that
    // base64, so the comparison has to add the same prefix.
    QCOMPARE(QStringLiteral("SHA256:")
                 + fingerprintOfKnownHostsLine(m_knownHostsAfterAccept),
             m_promptedFingerprint);

    // The server identity was adopted from the SERVER, not minted locally.
    QTRY_VERIFY_WITH_TIMEOUT(!m_graph->app.serverId().isEmpty(), kOpTimeoutMs);
    m_serverId = m_graph->app.serverId();
    qInfo().noquote() << "adopted serverId:" << m_serverId;
    QCOMPARE(m_graph->layouts.serverId(), m_serverId);

    // A SECOND connect over the now-trusted key must not ask again.
    g_phase = QStringLiteral("step3 disconnect");
    const int promptsBefore = promptSpy.count();
    m_graph->app.disconnectServer();
    QTRY_COMPARE(m_graph->app.connectionState(), QStringLiteral("disconnected"));
    rungs << ladder(QStringLiteral("after disconnect"));
    g_phase = QStringLiteral("step3 connect#2");
    QVERIFY(QMetaObject::invokeMethod(
        sheet->findChild<QQuickItem*>(QStringLiteral("connectButton")), "clicked"));
    QVERIFY2(waitForConnected(kConnectTimeoutMs),
             qPrintable(QStringLiteral("reconnect over a trusted key failed.\nladder:\n%1\n  %2"
                                       "\nbootstrap=%3")
                            .arg(rungs.join(QLatin1Char('\n')),
                                 ladder(QStringLiteral("failed here")),
                                 m_graph->bootstrapErrors.join(QLatin1Char('|')))));
    rungs << ladder(QStringLiteral("connect #2 (trusted key)"));
    m_promptedOnSecondConnect = promptSpy.count() > promptsBefore;
    QVERIFY2(!m_promptedOnSecondConnect,
             "an already-trusted host key was prompted for a second time");
    QCOMPARE(readFileText(knownHostsPath()).trimmed(), m_knownHostsAfterAccept);

    qInfo().noquote() << "host-key ladder:\n" << rungs.join(QLatin1Char('\n'));

    // A user who connected is not left staring at the connect sheet: dismiss it
    // the way the UI offers (Close), then the workspace must be uncovered.
    QVERIFY(QMetaObject::invokeMethod(
        sheet->findChild<QQuickItem*>(QStringLiteral("closeButton")), "clicked"));
    QTRY_VERIFY(!sheet->property("shown").toBool());
    QVERIFY(!qobject_cast<QQuickItem*>(sheet)->isVisible());

    // LAST, so everything above still ran: an unknown key must be the user's
    // decision, never a silent trust-on-first-use.
    QVERIFY2(m_hostKeyPrompted,
             "SPEC 12.1: an UNKNOWN host key was trusted WITHOUT prompting - "
             "AppController::connectToProfile installs a prompting host-key "
             "callback and SessionBootstrap::attemptWire() must connect through "
             "it, never through an unconditional Accept of its own");
}

// ---------------------------------------------------------------------------
// 4. WORKSPACE. Create a group and a Dev Session, see them in the sidebar, and
//    activate the session the way the sidebar does it.
// ---------------------------------------------------------------------------
void TstColdStart::step4_workspaceAndSessionActivation()
{
    g_phase = QStringLiteral("step4 workspace");

    // POSITIVE PROOF that the session step 3 built is live RIGHT NOW, asserted
    // before anything else so this step can never be read as "the app is not
    // wired". Three independent witnesses: the controller's own state, the SSH
    // pool, and a round trip on the wire that does not go through the RPC
    // client at all.
    QVERIFY2(m_graph->app.connectionState() == QLatin1String("connected"),
             qPrintable(QStringLiteral("step 3 left no connection: %1")
                            .arg(ladder(QStringLiteral("entering step 4")))));
    QCOMPARE(m_graph->pool.state(), ch::SshConnectionPool::State::Connected);
    QVERIFY(m_graph->client.transport() == m_graph->bootstrap.rpcDevice());
    const QByteArray liveProbe =
        runExec(QStringLiteral("printf 'WIRED_%s\\n' '") + m_runId + QStringLiteral("'"));
    QVERIFY2(liveProbe.contains(QByteArrayLiteral("WIRED_") + m_runId.toLatin1()),
             qPrintable(QStringLiteral("no live SSH session entering step 4; %1; exec said '%2'")
                            .arg(ladder(QStringLiteral("entering step 4")),
                                 QString::fromUtf8(liveProbe))));
    qInfo().noquote() << ladder(QStringLiteral("entering step 4")).trimmed();

    ch::SessionsModel* model = m_graph->app.sessionsModel();
    QVERIFY(model != nullptr);
    QSignalSpy refreshed(&m_graph->app, &ch::AppController::refreshed);

    m_graph->app.createGroup(m_groupName);
    QTRY_VERIFY_WITH_TIMEOUT(refreshed.count() > 0, kOpTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(model->rowCount() > 0, kOpTimeoutMs);

    int groupRow = -1;
    for (int g = 0; g < model->rowCount(); ++g) {
        if (model->data(model->index(g, 0), ch::SessionsModel::NameRole).toString()
            == m_groupName)
            groupRow = g;
    }
    QVERIFY2(groupRow >= 0, qPrintable(QStringLiteral("group %1 not in the sidebar")
                                           .arg(m_groupName)));
    m_groupId = model->data(model->index(groupRow, 0), ch::SessionsModel::IdRole).toString();
    QVERIFY(!m_groupId.isEmpty());

    const int before = refreshed.count();
    m_graph->app.createSession(m_groupId, m_sessionName, g_live.repo);
    QTRY_VERIFY_WITH_TIMEOUT(refreshed.count() > before, kOpTimeoutMs);

    const QModelIndex group = model->index(groupRow, 0);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(group), 1, kOpTimeoutMs);
    const QModelIndex session = model->index(0, 0, group);
    QCOMPARE(model->data(session, ch::SessionsModel::NameRole).toString(), m_sessionName);
    m_sessionId = model->data(session, ch::SessionsModel::IdRole).toString();
    QVERIFY(!m_sessionId.isEmpty());
    QCOMPARE(model->data(session, ch::SessionsModel::SubtitleRole).toString(),
             QFileInfo(QDir::cleanPath(g_live.repo)).fileName());

    // The QML sidebar really rendered the row: its delegate names itself after
    // the session id, so this is the item on screen, not just the model.
    //
    // OBSERVED here, ASSERTED at the end of the step — the same reason step 3
    // defers its host-key verdict. A sidebar that never draws the row is a real
    // defect, but swallowing activation, the region trees and every later step
    // behind it would hide four more verdicts behind one.
    QObject* sidebar = m_graph->evalObject(QStringLiteral("sidebarRegion"));
    QVERIFY(sidebar != nullptr);
    const QString rowName = QStringLiteral("sessionRow:") + m_sessionId;
    const bool rowAppeared = QTest::qWaitFor(
        [sidebar, &rowName] { return findByName(sidebar, rowName) != nullptr; },
        kOpTimeoutMs);
    QQuickItem* row = rowAppeared ? findByName(sidebar, rowName) : nullptr;
    if (row) {
        m_sidebarRowOnScreen =
            row->isVisible() && row->width() > 0 && row->height() > 0;
        m_sidebarRowLabel = row->property("name").toString();
        m_sidebarRowReport = QStringLiteral("%1 visible=%2 size=%3x%4")
                                 .arg(rowName)
                                 .arg(row->isVisible())
                                 .arg(row->width())
                                 .arg(row->height());
    } else {
        m_sidebarRowReport =
            QStringLiteral("no %1 in the sidebar; named items=[%2]; types=[%3]")
                .arg(rowName, objectNamesOf(sidebar).join(QLatin1Char(',')),
                     typeNamesOf(sidebar).join(QLatin1Char(',')));
    }

    // Activate it exactly as a click/Enter in the sidebar does: the sidebar owns
    // no navigation, it emits sessionActivated and Main.qml routes it.
    QVERIFY(QMetaObject::invokeMethod(sidebar, "sessionActivated",
                                      Q_ARG(QString, m_sessionId)));
    QTRY_COMPARE_WITH_TIMEOUT(m_graph->app.activeSessionId(), m_sessionId, kOpTimeoutMs);
    QCOMPARE(m_graph->app.activeSessionRepoRoot(), g_live.repo);

    // Both region trees must resolve from the server, and both regions must be
    // showing a real node rather than Main.qml's literal fallback.
    QTRY_VERIFY_WITH_TIMEOUT(!m_graph->layouts.viewerTree().isNull(), kOpTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!m_graph->layouts.terminalTree().isNull(), kOpTimeoutMs);
    qInfo().noquote() << "viewer tree:" << m_graph->layouts.viewerTree();
    qInfo().noquote() << "terminal tree:" << m_graph->layouts.terminalTree();

    QQuickItem* viewerRegion = findByType(m_graph->window(), QStringLiteral("ViewerRegion"));
    QVERIFY2(viewerRegion != nullptr,
             qPrintable(typeNamesOf(m_graph->window()).join(QLatin1Char(','))));
    QObject* terminalRegion = m_graph->evalObject(QStringLiteral("terminalRegion"));
    QVERIFY(terminalRegion != nullptr);
    QVERIFY2(!viewerRegion->property("node").isNull(), "viewer region has no node");
    QVERIFY2(!terminalRegion->property("node").isNull(), "terminal region has no node");
    QCOMPARE(terminalRegion->property("devSessionId").toString(), m_sessionId);
    QTRY_COMPARE_WITH_TIMEOUT(terminalRegion->property("workingDir").toString(),
                              g_live.repo, kOpTimeoutMs);

    // Deferred from the top of the step. Logged unconditionally first: the
    // toast check below can abort the step, and a reader still needs to know
    // whether the row was ever drawn.
    qInfo().noquote() << "sidebar row:" << m_sidebarRowReport;
    QVERIFY2(m_sidebarRowOnScreen, qPrintable(m_sidebarRowReport));
    QCOMPARE(m_sidebarRowLabel, m_sessionName);

    // ERROR TOASTS: NONE, anywhere between a cold launch and a working Dev
    // Session. Main.qml paints every AppController::error as a red banner, so
    // this is literally "the user was never shown a failure".
    //
    // Each entry is stamped with the phase that raised it. That stamp is the
    // point: an unattributed list of four strings was read as "the session is
    // not wired at step 4", when the entries actually came from a refresh
    // before any server existed (step 1), the DELIBERATE host-key refusal
    // (step 3), and the remote bridge's informational stderr banner (step 3,
    // twice). All three have since been fixed at the source, so the assertion
    // stands unnarrowed: any regression names its own phase.
    qInfo().noquote() << "error toasts raised so far (attributed): "
                      << (m_graph->appErrors.isEmpty()
                              ? QStringLiteral("none")
                              : QLatin1Char('\n') + m_graph->appErrors.join(QLatin1Char('\n')));
    QVERIFY2(m_graph->appErrors.isEmpty(),
             qPrintable(QStringLiteral("the walkthrough raised %1 error toast(s):\n  %2")
                            .arg(m_graph->appErrors.size())
                            .arg(m_graph->appErrors.join(QStringLiteral("\n  ")))));
}

// ---------------------------------------------------------------------------
// 5. LIVE TERMINAL. The pane the split tree built must carry a real remote
//    shell, rooted where the Dev Session says its repository is.
// ---------------------------------------------------------------------------
void TstColdStart::step5_terminalPaneRunsARemoteShell()
{
    g_phase = QStringLiteral("step5 terminal");
    QVERIFY2(!m_sessionId.isEmpty(), "step 4 produced no Dev Session");

    QObject* terminalRegion = m_graph->evalObject(QStringLiteral("terminalRegion"));
    QVERIFY(terminalRegion != nullptr);
    m_terminalPane = findByType(terminalRegion, QStringLiteral("TerminalPaneView"));
    QVERIFY2(m_terminalPane != nullptr,
             qPrintable(QStringLiteral("no TerminalPaneView under the terminal region; "
                                       "children=%1")
                            .arg(typeNamesOf(terminalRegion).join(QLatin1Char(',')))));

    m_terminalController = qobject_cast<ch::TerminalController*>(
        asObject(m_terminalPane->property("controller")));
    m_terminalBridge = qobject_cast<ch::TerminalBridge*>(
        asObject(m_terminalPane->property("bridge")));
    QVERIFY2(m_terminalController != nullptr, "the pane has no controller");
    QVERIFY2(m_terminalBridge != nullptr, "the pane has no bridge");

    connect(m_terminalBridge, &ch::TerminalBridge::write, this,
            [this](const QString& text) { m_rendered += text; });

    QString factoryErrors;
    connect(&m_graph->terminalFactory, &ch::TerminalFactory::error, this,
            [&factoryErrors](ch::TerminalController*, const QString& text) {
                factoryErrors += text + QLatin1Char('\n');
            });

    // The pane attached itself when the session arrived; no test poke.
    QTRY_VERIFY2_WITH_TIMEOUT(
        m_terminalPane->property("attached").toBool(),
        qPrintable(QStringLiteral("pane never attached: statusText=%1 errors=%2")
                       .arg(m_terminalPane->property("statusText").toString(), factoryErrors)),
        kAttachTimeoutMs);

    const QString expectedTarget = ch::TerminalController::tmuxTarget(
        ch::DevSessionId{m_sessionId},
        ch::TerminalId{m_terminalPane->property("terminalId").toString()});
    QCOMPARE(m_graph->terminalFactory.targetFor(m_terminalController), expectedTarget);
    QVERIFY(m_terminalController->transport() != nullptr);

    // Real bytes from a real shell.
    QTRY_VERIFY2_WITH_TIMEOUT(
        !paneText().isEmpty(),
        qPrintable(QStringLiteral("no output from the pane: errors=%1").arg(factoryErrors)),
        kAttachTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(m_terminalController->state() == ch::TerminalState::Ready,
                             kAttachTimeoutMs);
    QCOMPARE(m_terminalBridge->connectionState(), ch::toString(ch::TerminalState::Ready));

    // The renderer: report exactly what the headless page did, whether or not
    // it mounted, so a page failure is evidence rather than a mystery.
    const bool rendererMounted = QTest::qWaitFor(
        [this] { return m_terminalBridge->rendererReady(); }, kRendererTimeoutMs);
    qInfo().noquote() << QStringLiteral(
                             "renderer: pageLoaded=%1 rendererReady=%2 live=%3 status=%4")
                             .arg(m_terminalPane->property("pageLoaded").toBool())
                             .arg(rendererMounted)
                             .arg(m_terminalPane->property("live").toBool())
                             .arg(m_terminalPane->property("statusText").toString());
    QVERIFY2(m_terminalPane->property("pageLoaded").toBool(),
             qPrintable(QStringLiteral("the packaged xterm.js page never loaded: %1")
                            .arg(m_terminalPane->property("statusText").toString())));
    QVERIFY2(rendererMounted,
             "the xterm.js page loaded but never completed its WebChannel mount "
             "handshake (bridge.ready()), so the pane shows nothing");
    QTRY_VERIFY_WITH_TIMEOUT(m_terminalPane->property("live").toBool(), kRendererTimeoutMs);

    // A marker that CANNOT come from the pane echoing our own keystrokes.
    //
    // This is the whole point of the step, so it is built to be impossible to
    // fake: the id is passed to printf as an ARGUMENT, so the bytes we type
    // contain "CH_CWD_%s=%s" and never "CH_CWD_<id>=". Only printf running on
    // the remote host can splice the two together. (Interpolating the id into
    // the format string instead — "printf 'CH_CWD_<id>=%s\n'" — puts the needle
    // straight into the echoed command line, and the assertion then passes on a
    // pane that is wired to nothing but its own local echo.)
    const QByteArray command = QByteArrayLiteral("printf 'CH_CWD_%s=%s\\n' '")
                               + m_runId.toLatin1() + QByteArrayLiteral("' \"$PWD\"");
    const QString needle = QStringLiteral("CH_CWD_%1=").arg(m_runId);
    QVERIFY2(!QString::fromLatin1(command).contains(needle),
             "the marker leaked into the keystrokes, so the echo alone would "
             "satisfy every assertion below");
    QVERIFY2(typeUntil(
                 command, [this, &needle] { return paneText().contains(needle); },
                 kCommandTimeoutMs),
             qPrintable(QStringLiteral("no remote output for %1; errors=%2; tail=%3")
                            .arg(needle, factoryErrors, paneText().right(600))));
    qInfo().noquote() << "remote shell answered through the pane bridge";

    // The remote really owns a tmux session under the pane's target.
    QCOMPARE(runExec(QStringLiteral("tmux has-session -t '%1' >/dev/null 2>&1 "
                                    "&& echo ALIVE || echo GONE")
                         .arg(expectedTarget))
                 .trimmed(),
             QByteArray("ALIVE"));

    // ...and the shell opened where the Dev Session lives. A pane that attaches
    // before its workingDir binding has settled lands in $HOME instead, which is
    // invisible to any assertion that only checks "a shell answered".
    const QRegularExpression cwdLine(
        QStringLiteral("CH_CWD_%1=([^\\r\\n]*)").arg(m_runId));
    const QRegularExpressionMatch match = cwdLine.match(paneText());
    QVERIFY(match.hasMatch());
    const QString remoteCwd = match.captured(1).trimmed();
    qInfo().noquote() << "pane cwd =" << remoteCwd << " expected" << g_live.repo;
    QCOMPARE(remoteCwd, QDir::cleanPath(g_live.repo));

    QVERIFY2(factoryErrors.isEmpty(), qPrintable(factoryErrors));
}

// ---------------------------------------------------------------------------
// 6. EDITOR. A real remote file opened in the real editor pane, edited and
//    saved through the very slots the Monaco bundle calls, with the result read
//    back off the remote disk by a shell that knows nothing about the RPC layer.
// ---------------------------------------------------------------------------
void TstColdStart::step6_editorPaneSavesToTheRemoteDisk()
{
    g_phase = QStringLiteral("step6 editor");
    QVERIFY2(m_graph->pool.state() == ch::SshConnectionPool::State::Connected,
             "no connection");

    const QString original = QStringLiteral("cold start original %1\n").arg(m_runId);
    runExec(QStringLiteral("printf '%1' > '%2'").arg(original, m_remoteFile));
    QCOMPARE(QString::fromUtf8(runExec(QStringLiteral("cat '%1'").arg(m_remoteFile))),
             original);

    // Put the file in the viewer region. The persisted split tree carries only
    // paneIds (ch::SplitNode has no url field) and the shipped UI has no
    // open-a-file affordance at all, so this assigns the region the same literal
    // node shape Main.qml's own fallback uses, with the url a "file opened" node
    // would carry. Everything below is then the real pane path.
    QQuickItem* viewerRegion = findByType(m_graph->window(), QStringLiteral("ViewerRegion"));
    QVERIFY(viewerRegion != nullptr);
    QVariantMap node;
    node.insert(QStringLiteral("paneId"), QStringLiteral("viewer-1"));
    node.insert(QStringLiteral("url"), QStringLiteral("file://") + m_remoteFile);
    node.insert(QStringLiteral("children"), QVariantList{});
    viewerRegion->setProperty("node", node);

    QQuickItem* editorPane = nullptr;
    QTRY_VERIFY2_WITH_TIMEOUT(
        (editorPane = findByType(viewerRegion, QStringLiteral("EditorPaneView"))) != nullptr,
        qPrintable(QStringLiteral("no EditorPaneView for a text file; children=%1")
                       .arg(typeNamesOf(viewerRegion).join(QLatin1Char(',')))),
        kOpTimeoutMs);
    QCOMPARE(editorPane->property("remotePath").toString(), m_remoteFile);

    auto* editor = qobject_cast<ch::EditorController*>(
        asObject(editorPane->property("controller")));
    QVERIFY2(editor != nullptr, "the editor pane has no controller");

    QSignalSpy savedSpy(editor, &ch::EditorController::saved);
    QSignalSpy conflictSpy(editor, &ch::EditorController::saveConflict);
    QSignalSpy errorSpy(editor, &ch::EditorController::saveError);

    // The pane opened the file itself (EditorPaneView::start), so this waits on
    // the product's own load, not a poke from the test.
    QTRY_COMPARE_WITH_TIMEOUT(editor->path(), m_remoteFile, kOpTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!editor->revision().isEmpty(), kOpTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(editor->fileState(), QStringLiteral("clean"), kOpTimeoutMs);
    const QString revision = editor->revision();

    // Save through the frozen C3 slot the Monaco page invokes.
    const QString edited = QStringLiteral("cold start EDITED %1\n").arg(m_runId);
    QVERIFY(QMetaObject::invokeMethod(editor, "save", Q_ARG(QString, edited),
                                      Q_ARG(QString, revision)));
    QVERIFY2(QTest::qWaitFor(
                 [&] {
                     return savedSpy.count() > 0 || conflictSpy.count() > 0
                            || errorSpy.count() > 0;
                 },
                 kOpTimeoutMs),
             "save never resolved");
    QVERIFY2(conflictSpy.isEmpty(), "unexpected save conflict on an untouched file");
    QVERIFY2(errorSpy.isEmpty(),
             qPrintable(errorSpy.isEmpty() ? QString() : errorSpy.at(0).at(0).toString()));
    QCOMPARE(savedSpy.count(), 1);
    QVERIFY(!savedSpy.at(0).at(0).toString().isEmpty());
    QVERIFY2(savedSpy.at(0).at(0).toString() != revision,
             "the server returned the same revision after a write");

    // OUT OF BAND: a shell, not the RPC file service that just wrote it.
    const QString onDisk =
        QString::fromUtf8(runExec(QStringLiteral("cat '%1'").arg(m_remoteFile)));
    qInfo().noquote() << "remote bytes after save:" << onDisk.trimmed();
    QCOMPARE(onDisk, edited);
}

// ---------------------------------------------------------------------------
// 7. RELAUNCH. Tear the whole object graph down and build it again against the
//    same config dir: the app must reconnect on its own, reopen the same Dev
//    Session with the same layout, and restore the region widths.
// ---------------------------------------------------------------------------
void TstColdStart::step7_relaunchRestoresTheSession()
{
    g_phase = QStringLiteral("step7 relaunch");
    QVERIFY2(!m_sessionId.isEmpty(), "nothing to restore");

    m_viewerTreeBefore = m_graph->layouts.viewerTree();
    m_terminalTreeBefore = m_graph->layouts.terminalTree();
    QVERIFY(!m_viewerTreeBefore.isNull());
    QVERIFY(!m_terminalTreeBefore.isNull());

    // --- a real handle drag, the only thing Main.qml persists widths on ------
    QQuickWindow* window = m_graph->window();
    auto* outer = qobject_cast<QQuickItem*>(m_graph->evalObject(QStringLiteral("outer")));
    QVERIFY(outer != nullptr);
    const int sidebarWidth =
        qRound(m_graph->eval(QStringLiteral("sidebarRegion.width")).toReal());
    QVERIFY(sidebarWidth > 0);

    QQuickItem* handle = nullptr;
    QStringList geometry;
    for (QQuickItem* child : outer->childItems()) {
        geometry << QStringLiteral("%1 x=%2 w=%3")
                        .arg(QString::fromLatin1(child->metaObject()->className()))
                        .arg(child->x())
                        .arg(child->width());
        if (child->width() > 0 && child->width() <= 30 && child->x() >= sidebarWidth - 1
            && child->x() <= sidebarWidth + 30)
            handle = child;
    }
    QVERIFY2(handle != nullptr,
             qPrintable(QStringLiteral("no SplitView handle at the sidebar edge:\n%1")
                            .arg(geometry.join(QLatin1Char('\n')))));

    constexpr int kDragDelta = 55;
    const QPoint grab(qRound(handle->x() + handle->width() / 2),
                      qRound(window->height() / 2.0));
    const QPoint drop = grab + QPoint(kDragDelta, 0);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, grab);
    QTest::mouseMove(window, grab + QPoint(kDragDelta / 2, 0));
    QTest::mouseMove(window, drop);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, drop);
    QTRY_COMPARE(qRound(m_graph->eval(QStringLiteral("sidebarRegion.width")).toReal()),
                 sidebarWidth + kDragDelta);

    m_draggedSidebar = qRound(m_graph->eval(QStringLiteral("sidebarRegion.width")).toReal());
    m_draggedTerminal =
        qRound(m_graph->eval(QStringLiteral("terminalRegion.width")).toReal());
    QVERIFY(m_draggedSidebar > 0 && m_draggedTerminal > 0);

    // --- quit -----------------------------------------------------------
    m_terminalPane = nullptr;
    m_terminalBridge = nullptr;
    m_terminalController = nullptr;
    m_rendered.clear();
    m_graph.reset();
    QTest::qWait(250); // let the destroyed WebEngine views finish tearing down

    // The user's choices really are on disk, written by the process that just
    // exited — nothing below can be served from a live object.
    const QString config = readFileText(configFilePath());
    qInfo().noquote() << "config on disk before relaunch:\n" << config.trimmed();
    QVERIFY2(config.contains(QStringLiteral("sidebarWidth=%1").arg(m_draggedSidebar)),
             qPrintable(config));
    QVERIFY2(config.contains(m_sessionId),
             qPrintable(QStringLiteral("the active Dev Session was never stored:\n%1")
                            .arg(config)));

    // --- relaunch ---------------------------------------------------------
    buildGraph();

    // A stored, active profile must reconnect on its own, and the sheet must
    // NOT be in the way this time.
    QObject* sheet = m_graph->evalObject(QStringLiteral("connectSheet"));
    QVERIFY(sheet != nullptr);
    QCOMPARE(m_graph->serverProfiles.profiles().size(), 1);
    QCOMPARE(m_graph->serverProfiles.activeId(), m_profileId);
    QVERIFY2(!sheet->property("shown").toBool(),
             "a configured client opened on the connect sheet instead of the workspace");

    QVERIFY2(waitForConnected(kConnectTimeoutMs),
             qPrintable(QStringLiteral("relaunch never auto-connected; state=%1 %2")
                            .arg(m_graph->app.connectionState(),
                                 m_graph->bootstrapErrors.join(QLatin1Char('|')))));
    // ...without asking about the host key again: the store survived the restart.
    QCOMPARE(readFileText(knownHostsPath()).trimmed(), m_knownHostsAfterAccept);

    // Same server identity, same workspace.
    QTRY_COMPARE_WITH_TIMEOUT(m_graph->app.serverId(), m_serverId, kOpTimeoutMs);

    // Same Dev Session, reopened without the user touching anything.
    QTRY_COMPARE_WITH_TIMEOUT(m_graph->app.activeSessionId(), m_sessionId, kConnectTimeoutMs);
    QCOMPARE(m_graph->app.activeSessionRepoRoot(), g_live.repo);

    // Same layout in both regions.
    QTRY_VERIFY_WITH_TIMEOUT(!m_graph->layouts.viewerTree().isNull(), kOpTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!m_graph->layouts.terminalTree().isNull(), kOpTimeoutMs);
    QCOMPARE(m_graph->layouts.viewerTree(), m_viewerTreeBefore);
    QCOMPARE(m_graph->layouts.terminalTree(), m_terminalTreeBefore);

    // Same region widths, in the live layout (not just in QSettings).
    QTRY_COMPARE_WITH_TIMEOUT(
        qRound(m_graph->eval(QStringLiteral("sidebarRegion.width")).toReal()),
        m_draggedSidebar, kOpTimeoutMs);
    QCOMPARE(qRound(m_graph->eval(QStringLiteral("terminalRegion.width")).toReal()),
             m_draggedTerminal);

    // And the sidebar shows the workspace the previous run created.
    QObject* sidebar = m_graph->evalObject(QStringLiteral("sidebarRegion"));
    QVERIFY(sidebar != nullptr);
    QTRY_VERIFY2_WITH_TIMEOUT(
        findByName(sidebar, QStringLiteral("sessionRow:") + m_sessionId) != nullptr,
        qPrintable(QStringLiteral("the restored sidebar has no row for %1; named items=[%2]")
                       .arg(m_sessionId, objectNamesOf(sidebar).join(QLatin1Char(',')))),
        kOpTimeoutMs);

    QVERIFY2(m_graph->qmlWarnings.isEmpty(),
             qPrintable(m_graph->qmlWarnings.join(QLatin1Char('\n'))));

    // The relaunch graph is a fresh AppGraph, so this list covers exactly the
    // second launch: an auto-connect over an already-trusted key must reach a
    // restored workspace without showing the user one failure.
    QVERIFY2(m_graph->appErrors.isEmpty(),
             qPrintable(QStringLiteral("the relaunch raised %1 error toast(s):\n  %2")
                            .arg(m_graph->appErrors.size())
                            .arg(m_graph->appErrors.join(QStringLiteral("\n  ")))));
}

int main(int argc, char* argv[])
{
    // Capture the live fixture settings and then REMOVE them: everything built
    // after this point is the shipped app, and it must not be able to read them.
    g_live.present = !qEnvironmentVariableIsEmpty("CH_LIVE_SSH");
    g_live.host = qEnvironmentVariable("CH_LIVE_HOST");
    g_live.port = static_cast<quint16>(qEnvironmentVariable("CH_LIVE_PORT").toUInt());
    g_live.user = qEnvironmentVariable("CH_LIVE_USER");
    g_live.node = qEnvironmentVariable("CH_LIVE_NODE");
    g_live.repo = qEnvironmentVariable("CH_LIVE_REPO");
    for (const char* key : {"CH_LIVE_SSH", "CH_LIVE_HOST", "CH_LIVE_PORT", "CH_LIVE_USER",
                            "CH_LIVE_NODE", "CH_LIVE_REPO", "CH_LIVE_KNOWN_HOSTS"})
        qunsetenv(key);

    QTemporaryDir configRoot;
    if (!configRoot.isValid()) {
        qCritical("could not create a temporary config home");
        return 2;
    }

    // Linux's native QSettings path follows XDG_CONFIG_HOME. macOS and
    // Windows ignore that variable; when the live fixture is absent, Qt's
    // test-mode path keeps this skipped gate away from the user's settings.
#if !defined(Q_OS_LINUX)
    if (!g_live.present)
        QStandardPaths::setTestModeEnabled(true);
#endif
#if defined(Q_OS_LINUX)
    g_configHome = configRoot.filePath(QStringLiteral("config"));
    QDir().mkpath(g_configHome);
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(g_configHome));
#else
    g_configHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QDir().mkpath(g_configHome);
#endif

    // main.cpp's startup order: URL scheme, then WebEngine, then the GUI app.
    ch::ViewerProfiles::registerUrlScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QString::fromLatin1(kApplication));
    QGuiApplication::setOrganizationName(QString::fromLatin1(kOrganization));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

#if defined(Q_OS_LINUX)
    if (QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        != g_configHome) {
        qCritical("XDG_CONFIG_HOME redirection did not take effect");
        return 2;
    }
#endif

    TstColdStart testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_coldstart.moc"
