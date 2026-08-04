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
#include "GroupPaletteService.h"
#include "WindowChromeNative.h"

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
    // Pins the Wayland app_id (and the X11 startup-notification id) to the
    // basename of packaging/codeharbor.desktop, whose StartupWMClass=codeharbor
    // is what associates the running window with its launcher icon in the
    // dock/taskbar. Qt only falls back to the executable's base name when this
    // is unset, which happens to match today - so this makes a guarantee out of
    // what was an accident of the binary being named `codeharbor`. NO ".desktop"
    // suffix: Qt documents this as the base name and only strips a trailing
    // ".desktop" for backward compatibility, printing a warning when it does.
    QGuiApplication::setDesktopFileName(QStringLiteral("codeharbor"));

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
    // ch_viewers deliberately does not link ch_app; push the validated,
    // client-local preference across that library boundary here.
    viewers.setDefaultKinds(appController.settings()->viewerDefaultKinds());
    QObject::connect(appController.settings(), &ch::AppSettings::viewerDefaultsChanged,
                     &viewers, [&appController, &viewers]() {
                         viewers.setDefaultKinds(
                             appController.settings()->viewerDefaultKinds());
                     });

    // Connection spine the UI drives: stored server profiles (client-local, the
    // only way to reach a server before one is reachable) and the per-session
    // split layouts read back from that server. The workspace is keyed by the
    // SERVER's own id, taken from server.info once wired.
    ch::ServerProfiles serverProfiles;
    ch::SessionLayouts sessionLayouts(appController.workspaceDb(),
                                      appController.uiState());
    appController.setConnection(&sshPool, &sessionBootstrap, &serverProfiles,
                                &sessionLayouts);

    // Per-pane factories (workstreams E and T): each pane owns its controller so
    // split panes never clobber each other. Declared AFTER sshPool so they are
    // destroyed before the pool TerminalFactory points at.
    ch::EditorFactory editorFactory(&client);
    ch::TerminalFactory terminalFactory(&sshPool);

    // A terminal's identity is a row in the SERVER's terminal_panes table, so
    // the factory needs the workspace repository and the id of the server whose
    // rows those are (SPEC 5.2). The repository is the AppController's and is
    // declared above, so it outlives the factory; the server id is only known
    // once server.info has answered, which is what the connection is for.
    //
    // Wired here, before the environment auto-connect below, for the same
    // reason setEditorFactory() is: adoptServerIdentity() runs SYNCHRONOUSLY
    // out of connectAndWire(), and a factory registered afterwards would miss
    // the identity it is about to need.
    terminalFactory.setWorkspace(appController.workspaceDb());
    QObject::connect(&appController, &ch::AppController::serverIdChanged, &terminalFactory,
                     [&appController, &terminalFactory]() {
                         terminalFactory.setServerId(appController.serverId());
                     });

    // SPEC 6.6. The "generic" harness publishes no lifecycle events, so the
    // bridge has no adapter for it and the monitor would otherwise never learn
    // anything about such a pane. Its only observable is terminal output, and
    // the factory is the one object that sees both the pane's terminal_panes
    // row id and the PTY channel its bytes arrive on — so it reports the FACT
    // of output (never the bytes) and the monitor derives running/idle from it.
    terminalFactory.setAgentMonitor(&agentMonitor);

    // Self-migration for LEGACY layouts. A terminal leaf stored before layouts
    // carried a `terminal_panes` row id resolves once by its slot label; the
    // factory reports the row that found, and the id is written into the leaf
    // and persisted. From then on that leaf is addressed by row id like every
    // other, and the recyclable label is never used to name a shell again.
    QObject::connect(&terminalFactory, &ch::TerminalFactory::paneRowResolved,
                     &sessionLayouts, &ch::SessionLayouts::bindTerminalPaneRow);

    // Feed server.info.recoveryDir to the editor factory once a server identity
    // is adopted, so per-pane crash-recovery snapshots (SPEC 11.3) land under a
    // server-chosen remote path.
    //
    // This MUST be wired before the environment auto-connect below, for the
    // same reason setConnection() must: connectAndWire() emits wired()
    // SYNCHRONOUSLY, which runs adoptServerIdentity() and issues server.info.
    // Registering the factory afterwards left the recoveryDir handoff racing
    // that response.
    appController.setEditorFactory(&editorFactory);
    appController.setTerminalFactory(&terminalFactory);

    // Agent attention -> OS notification: construct and connect this sink
    // before any environment auto-connect can deliver the first agent event.
    // Startup events are transitions too; wiring afterwards silently loses
    // the only chance to notify the user about one that happened during the
    // initial handshake.
    ch::Notifier notifier;
    QObject::connect(&agentMonitor, &ch::AgentStatusMonitor::notify, &notifier,
                     &ch::Notifier::notify);

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


    // GroupPaletteService is a pure deterministic colour service, but QML
    // needs one long-lived instance to cache the expanded palette and each
    // name's index. It is declared above the engine so every binding sees a
    // live object until the engine has finished destroying those bindings.

    // Windows' frameless shell needs its native snap styles and maximise
    // hit-test bridge. The helper is a no-op elsewhere, but it still follows
    // the same lifetime rule as every object exposed to QML.
    ch::WindowChromeNative windowChrome;
    ch::GroupPaletteService groupPalette;


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
    // Only what QML actually reads. `sessionLayouts` is reached through
    // `app.layouts` (AppController owns the connection spine and QML binds to it
    // there), and `notifier` is a pure C++ sink for AgentStatusMonitor::notify —
    // publishing either one again as a root context property would advertise a
    // second, unread way into the same object and invite bindings to it.
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &appController);
    engine.rootContext()->setContextProperty(QStringLiteral("groupPalette"), &groupPalette);
    engine.rootContext()->setContextProperty(QStringLiteral("windowChrome"), &windowChrome);
    engine.rootContext()->setContextProperty(QStringLiteral("viewers"), &viewers);
    engine.rootContext()->setContextProperty(QStringLiteral("agentMonitor"), &agentMonitor);
    engine.rootContext()->setContextProperty(QStringLiteral("editorFactory"), &editorFactory);
    engine.rootContext()->setContextProperty(QStringLiteral("terminalFactory"), &terminalFactory);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("CodeHarbor", "Main");

    return app.exec();
}
