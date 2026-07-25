#include "AppController.h"
#include "CodeharbordClient.h"
#include "SessionBootstrap.h"
#include "SshConnectionPool.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"
#include "AgentStatusMonitor.h"
#include "EditorFactory.h"

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
    // A normal desktop launch stays server-less: connectAndWireFromEnvironment()
    // returns immediately unless CH_LIVE_SSH is set (with CH_LIVE_HOST/PORT/
    // USER/NODE/REPO), so the UI still comes up with no session and RPC calls
    // fail gracefully with a synthetic transport error. The pool is declared
    // before the bootstrap so it is destroyed after it: the channel devices the
    // bootstrap owns must not outlive the session they were opened on.
    ch::SshConnectionPool sshPool;
    ch::SessionBootstrap sessionBootstrap(&sshPool, &client, &agentMonitor);
    sessionBootstrap.connectAndWireFromEnvironment();

    // UI shell (workstream U) and viewer subsystem (workstream V) share the one
    // client. ViewerModel and ViewerProfiles share the same profiles so QML
    // WebEngineViews and the internal scheme handler use one security context.
    ch::AppController appController(&client);
    appController.setAgentMonitor(&agentMonitor);
    ch::ViewerProfiles profiles(&client);
    ch::ViewerModel viewers(&client);
    viewers.setProfiles(&profiles);

    // Per-pane editor controllers (workstream E): each editor pane creates its
    // own controller via this factory so split panes never clobber each other.
    ch::EditorFactory editorFactory(&client);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &appController);
    engine.rootContext()->setContextProperty(QStringLiteral("viewers"), &viewers);
    engine.rootContext()->setContextProperty(QStringLiteral("agentMonitor"), &agentMonitor);
    engine.rootContext()->setContextProperty(QStringLiteral("editorFactory"), &editorFactory);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("CodeHarbor", "Main");

    return app.exec();
}
