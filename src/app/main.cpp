#include "AppController.h"
#include "CodeharbordClient.h"
#include "SessionBootstrap.h"
#include "SessionLayouts.h"
#include "ServerProfiles.h"
#include "SshConnectionPool.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"
#include "AgentStatusMonitor.h"
#include "EditorFactory.h"
#include "TerminalFactory.h"
#include "Notifier.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtWebEngineQuick/QtWebEngineQuick>
#include <QtQuickControls2/QQuickStyle>

int main(int argc, char *argv[])
{
    // Custom URL schemes must be registered before WebEngine and the GUI
    // application are set up (SPEC 7.4). Idempotent; safe to call once here.
    ch::ViewerProfiles::registerUrlScheme();

    // WebEngine must be initialised before the GUI application (SPEC 7).
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setApplicationVersion(QStringLiteral(CODEHARBOR_VERSION));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Client-side RPC peer. Its transport is a dedicated SSH RPC channel wired
    // from the connection pool once a server session is established (workstream
    // S/U); until then calls fail gracefully with a synthetic transport error.
    ch::CodeharbordClient client;

    // Client-side agent-status monitor (workstream A). Its transport is a
    // dedicated SSH agent-status channel wired later; fed AgentEvent JSONL.
    ch::AgentStatusMonitor agentMonitor;

    // Remote session spine (workstream S): the SSH connection pool plus the
    // bootstrap that opens the RPC and agent-status channels and hands each one
    // to the consumer above as a QIODevice.
    //
    // The pool is declared before the bootstrap so it is destroyed AFTER it: the
    // channel devices the bootstrap owns must not outlive the session they were
    // opened on. connectAndWireFromEnvironment() is deliberately NOT called here
    // — see below, after appController.setConnection().
    ch::SshConnectionPool sshPool;
    ch::SessionBootstrap sessionBootstrap(&sshPool, &client, &agentMonitor);

    // UI shell (workstream U) and viewer subsystem (workstream V) share the one
    // client. ViewerModel and ViewerProfiles share the same profiles so QML
    // WebEngineViews and the internal scheme handler use one security context.
    ch::AppController appController(&client);
    appController.setAgentMonitor(&agentMonitor);
    ch::ViewerProfiles profiles(&client);
    ch::ViewerModel viewers(&client);
    viewers.setProfiles(&profiles);

    // Connection spine the UI drives: stored server profiles (client-local, the
    // only way to reach a server before one is reachable) and the per-session
    // split layouts read back from that server. The workspace is keyed by the
    // SERVER's own id, taken from server.info once wired.
    ch::ServerProfiles serverProfiles;
    ch::SessionLayouts sessionLayouts(appController.workspaceDb());
    appController.setConnection(&sshPool, &sessionBootstrap, &serverProfiles,
                                &sessionLayouts);

    // A normal desktop launch stays server-less: this returns immediately unless
    // CH_LIVE_SSH is set (with CH_LIVE_HOST/PORT/USER/NODE/REPO), so the UI still
    // comes up with no session and RPC calls fail gracefully with a synthetic
    // transport error.
    //
    // It MUST run after setConnection(): connectAndWire() emits wired()
    // SYNCHRONOUSLY from inside attemptWire(). Wired before the controller is
    // listening meant nobody ran adoptServerIdentity(), so the serverId stayed
    // empty, workspace.list was issued for "" and the whole shell came up with
    // an empty sidebar on top of a perfectly good SSH session.
    sessionBootstrap.connectAndWireFromEnvironment();

    // Per-pane factories (workstreams E and T): each pane owns its controller so
    // split panes never clobber each other.
    ch::EditorFactory editorFactory(&client);
    ch::TerminalFactory terminalFactory(&sshPool);

    // Agent attention -> OS notification (SPEC 6.2). A box with no notification
    // daemon degrades to a silent no-op.
    ch::Notifier notifier;
    QObject::connect(&agentMonitor, &ch::AgentStatusMonitor::notify, &notifier,
                     &ch::Notifier::notify);

    // The engine is declared LAST, so it is destroyed FIRST. That is load
    // bearing, not stylistic: every context property below is a stack object in
    // this scope, and a QML binding re-evaluated against one of them after it
    // had been destroyed is a crash on exit. Anything new that QML can see must
    // therefore be declared ABOVE this line. The rest of the scope unwinds in
    // reverse declaration order, which is already the safe order: panes and
    // their controllers (owned by the engine) before the factories, the
    // factories before the pool, the bootstrap before the pool it opened
    // channels on, and the client/monitor the bootstrap points at last of all.
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &appController);
    engine.rootContext()->setContextProperty(QStringLiteral("viewers"), &viewers);
    engine.rootContext()->setContextProperty(QStringLiteral("agentMonitor"), &agentMonitor);
    engine.rootContext()->setContextProperty(QStringLiteral("editorFactory"), &editorFactory);
    engine.rootContext()->setContextProperty(QStringLiteral("terminalFactory"), &terminalFactory);
    engine.rootContext()->setContextProperty(QStringLiteral("notifier"), &notifier);
    engine.rootContext()->setContextProperty(QStringLiteral("layouts"), &sessionLayouts);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("CodeHarbor", "Main");

    return app.exec();
}
