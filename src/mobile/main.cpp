#include "AgentStatusMonitor.h"
#include "AppController.h"
#include "CodeharbordClient.h"
#include "EditorFactory.h"
#include "MobileAppController.h"
#include "MobileImageProvider.h"
#include "MobileKeyStore.h"
#include "MobileTerminalView.h"
#include "MobileViewerService.h"
#include "ServerProfiles.h"
#include "SessionBootstrap.h"
#include "SessionLayouts.h"
#include "SshConnectionPool.h"
#include "TerminalFactory.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlContext>
#include <QtQuickControls2/QQuickStyle>

// The optional system web view, present only where the kit ships Qt WebView AND
// Qt WebViewQuick (CH_HAVE_QTWEBVIEW; see src/mobile/CMakeLists.txt). Included
// here, at the top and under the same guard as the initialize() call in main(),
// because a STATIC Qt - which is what Qt for iOS is, and iOS is the one mobile
// platform where this module exists in 6.10 - compiles out the automatic startup
// hook that prepares the platform plugin.
#if CH_HAVE_QTWEBVIEW
#include <QtWebView/QtWebView>
#endif

// Process entry point for the MOBILE client (Android, iOS, and any desktop host,
// where it is built so the shell can be developed and tested without a device).
//
// It assembles the SAME object graph src/app/main.cpp does — the RPC peer, the
// agent monitor, the SSH pool and session bootstrap, the AppController with its
// workspace repository and client-local UI state, the per-region layouts, and the
// per-pane editor and terminal factories — in the same order and for the same
// lifetime reasons. Nothing below is a mobile reimplementation of any of it.
//
// WHAT IS DELIBERATELY ABSENT, and why (each one is a desktop object with no
// mobile counterpart, not an oversight):
//   * QtWebEngineQuick::initialize() and ViewerProfiles::registerUrlScheme().
//     Qt WebEngine ships on neither Android nor iOS, and the privileged internal
//     URL scheme exists only to feed a WebEngine profile. Every rich surface here
//     is native Qt Quick instead, so there is no browser to initialise and no
//     scheme to register.
//   * QGuiApplication::setDesktopFileName(). It pins a Wayland app_id / X11
//     startup-notification id to packaging/codeharbor.desktop. Neither platform
//     has a .desktop file; the application identity comes from
//     packaging/android/AndroidManifest.xml and packaging/ios/Info.plist.in.
//   * ch::ViewerModel / ch::ViewerProfiles. Both are WebEngine-bound halves of
//     ch_viewers. The mobile shell links only ch_viewers_core, whose
//     ViewerHandlerRegistry is the Qt-Core-only URL/MIME classification, and it
//     is reached through ch::MobileViewerService and ch::PaneListModel.
//   * ch::ViewerCommandService. It is the channel an AI agent in a terminal pane
//     drives this window's VIEWER PANES through (SPEC 4.3) - open this file in
//     that pane, split, focus. There is no "that pane" here: the mobile client
//     shows exactly one surface at a time, so every command in the vocabulary
//     either has no addressee or would yank the screen out from under the user.
//     Not wiring it means an agent's pane commands are refused by this client
//     rather than half-obeyed.
//   * ch::Notifier. Its one backend is a D-Bus desktop notification service, and
//     QtDBus is not part of a mobile build; a platform notification channel is a
//     feature of its own, not a port of this one.
//   * ch::WindowChromeNative and ch::GroupPaletteService. The first is the
//     Windows frameless-window hit-test bridge; there is no window chrome on a
//     phone. The second caches the sidebar's per-group colour palette, and there
//     is no sidebar: the mobile session picker is a full-screen list.
//
// Command line: nothing is parsed here. argv goes to QGuiApplication, so only
// Qt's own switches are recognised.
//
// Exit codes: 0 for a normal quit; 255 when the QML graph failed to instantiate
// (objectCreationFailed below exits with -1, which the OS truncates to a byte).
int main(int argc, char *argv[])
{
#if CH_HAVE_QTWEBVIEW
    // MUST run here: BEFORE the application object exists, and it must not be
    // "tidied" to the more natural-looking spot just after it. Qt WebView loads
    // a platform plugin (the system web view) that has to be prepared before any
    // WebView element is created, and in a shared-library Qt that preparation is
    // registered to run automatically at startup. A STATIC Qt compiles that hook
    // out, so this explicit call is the only thing that ever performs it - and
    // Qt's own contract for it is "immediately before the QGuiApplication is
    // constructed". Qt for iOS is a static build and iOS is the one mobile
    // platform where Qt WebView exists in 6.10, so without this the web pane
    // builds, passes CI on a host where the module is absent, and then fails on
    // the device.
    QtWebView::initialize();
#endif

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setApplicationVersion(QStringLiteral(CODEHARBOR_VERSION));

    // Material, not Basic. The desktop pins Basic because it draws its own
    // chrome down to the scrollbars and wants no platform styling underneath;
    // here the opposite is true - the controls ARE the chrome, they have to be
    // thumb-sized, and Material is the one Qt Quick Controls style that is
    // designed for touch and available on both mobile platforms. This overrides
    // QT_QUICK_CONTROLS_STYLE, so the shell is drawn against Material and
    // nothing else.
    QQuickStyle::setStyle(QStringLiteral("Material"));

    // Client-side RPC peer. Its transport is a dedicated SSH RPC channel wired
    // from the connection pool once a server session is established; until then
    // calls fail gracefully with a synthetic transport error.
    ch::CodeharbordClient client;

    // Client-side agent-status monitor (SPEC 6.4). Its transport is a dedicated
    // SSH agent-status channel wired later; fed AgentEvent JSONL.
    ch::AgentStatusMonitor agentMonitor;

    // Remote session spine: the SSH connection pool plus the bootstrap that opens
    // the RPC and agent-status channels and hands each one to the consumer above
    // as a QIODevice.
    //
    // The pool is declared before the bootstrap so it is destroyed AFTER it: the
    // channel devices the bootstrap owns must not outlive the session they were
    // opened on.
    ch::SshConnectionPool sshPool;
    ch::SessionBootstrap sessionBootstrap(&sshPool, &client, &agentMonitor);

    ch::AppController appController(&client);
    appController.setAgentMonitor(&agentMonitor);

    // Connection spine the UI drives: stored server profiles (client-local, the
    // only way to reach a server before one is reachable) and the per-session
    // split layouts read back from that server. The workspace is keyed by the
    // SERVER's own id, taken from server.info once wired.
    ch::ServerProfiles serverProfiles;
    ch::SessionLayouts sessionLayouts(appController.workspaceDb(),
                                      appController.uiState());
    appController.setConnection(&sshPool, &sessionBootstrap, &serverProfiles,
                                &sessionLayouts);

    // Per-pane factories: each pane owns its controller so two panes never
    // clobber each other. Declared AFTER sshPool so they are destroyed before the
    // pool TerminalFactory points at.
    //
    // The mobile shell shows one pane at a time, so in practice at most one
    // controller of each kind is alive - but the factories are what OWN that
    // lifetime, and the single-pane rule is enforced by the navigation (see
    // ch::MobileAppController), not by pretending there can only ever be one.
    ch::EditorFactory editorFactory(&client);
    ch::TerminalFactory terminalFactory(&sshPool);

    // A terminal's identity is a row in the SERVER's terminal_panes table, so the
    // factory needs the workspace repository and the id of the server whose rows
    // those are (SPEC 5.2). Wired here, before the environment auto-connect
    // below, because adoptServerIdentity() runs SYNCHRONOUSLY out of
    // connectAndWire() and a factory registered afterwards would miss the
    // identity it is about to need.
    terminalFactory.setWorkspace(appController.workspaceDb());
    // And the RPC peer for one out-of-band diagnostic: `tmux new-session -A`
    // creates the pane's session when it is missing and says so nowhere, so after
    // an attach the factory asks `tmux.listSessions` whether the session it just
    // joined was in fact created by that attach.
    terminalFactory.setRpcClient(&client);
    QObject::connect(&appController, &ch::AppController::serverIdChanged,
                     &terminalFactory, [&appController, &terminalFactory]() {
                         terminalFactory.setServerId(appController.serverId());
                     });

    // SPEC 6.6. The "generic" harness publishes no lifecycle events, so the
    // factory reports the FACT of terminal output (never the bytes) and the
    // monitor derives running/idle from it.
    terminalFactory.setAgentMonitor(&agentMonitor);

    // Self-migration for LEGACY layouts: a terminal leaf stored before layouts
    // carried a `terminal_panes` row id resolves once by its slot label, and the
    // id is then written into the leaf and persisted.
    QObject::connect(&terminalFactory, &ch::TerminalFactory::paneRowResolved,
                     &sessionLayouts, &ch::SessionLayouts::bindTerminalPaneRow);

    // Feed server.info.recoveryDir to the editor factory once a server identity
    // is adopted, so per-pane crash-recovery snapshots (SPEC 11.3) land under a
    // server-chosen remote path. Both MUST be wired before the environment
    // auto-connect, for the same synchronous-wired() reason as setConnection().
    appController.setEditorFactory(&editorFactory);
    appController.setTerminalFactory(&terminalFactory);

    // App-private key material and known_hosts. The store is told about the pool
    // so an in-memory-only imported key can be armed for exactly one connect
    // attempt without ever touching the filesystem, and the bootstrap is pointed
    // at the store's known_hosts path so both halves of the first-use trust
    // decision - the file the pool verifies against and the file the trust sheet
    // reads to say "you have seen this host before" - are the same file.
    ch::MobileKeyStore keyStore;
    keyStore.setConnectionPool(&sshPool);
    sessionBootstrap.setKnownHostsPath(keyStore.knownHostsPath());

    // The mobile viewer surface: bounded remote file/directory reads over the
    // same RPC peer, with no network access of its own and no external-handler
    // path (SPEC 7.4/7.5).
    ch::MobileViewerService viewerService(&client);

    // Navigation. Borrows the controller and the layouts declared above, and the
    // pool for the one thing a CHANGED host key can only be learned from.
    ch::MobileAppController mobile(&appController, &sessionLayouts, &sshPool);
    // The one thing the navigation controller mints rather than borrows: a
    // terminal session per terminal page (see createTerminalSession). Injected
    // the same way AppController takes its factories, so a host with no
    // terminal at all simply never calls this.
    mobile.setTerminalFactory(&terminalFactory);
    // So an explicit disconnect drops every credential the store is holding
    // (SPEC 12.1). Without this the last key stays live for the whole run.
    mobile.setKeyStore(&keyStore);

    // A normal launch stays server-less: this returns immediately unless
    // CH_LIVE_SSH is set (with CH_LIVE_HOST/PORT/USER/NODE/REPO), so the shell
    // still comes up with no session and RPC calls fail gracefully. It is kept on
    // mobile because it is how the desktop-hosted build of this shell is driven
    // against a real server without typing a profile into a phone-sized form.
    //
    // It MUST run after setConnection(): connectAndWire() emits wired()
    // SYNCHRONOUSLY, and nobody listening means no server identity is adopted -
    // a healthy SSH session under a permanently empty session list.
    sessionBootstrap.connectAndWireFromEnvironment();

    // NOTE ON QML TYPE REGISTRATION: there is none to do here. Every C++ type
    // QML names (ch::MobileAppController and its NavStage enumerators,
    // ch::MobileTerminalView, ch::MobileTerminalSession, ch::PaneListModel,
    // ch::MobileCapabilities) declares QML_ELEMENT and is registered STATICALLY
    // by ch_mobile's own QML module, CodeHarbor.Mobile.Core.
    //
    // Doing it with runtime qmlRegister* calls here did not work and could not:
    // CodeHarbor.Mobile is a compiled QML module, which Qt marks PROTECTED, and
    // a runtime registration into a protected URI is refused with "Cannot
    // install element ... into protected module". The failure was quiet — the
    // shell loaded, and only the type that a QML file happened to NAME first
    // still resolved.

    // The engine is declared LAST, so it is destroyed FIRST. That is load
    // bearing, not stylistic: every context property below is a stack object in
    // this scope, and a QML binding re-evaluated against one of them after it had
    // been destroyed is a crash on exit. Anything new that QML can see must
    // therefore be declared ABOVE this line. The rest of the scope unwinds in
    // reverse declaration order, which is already the safe order: the pane and
    // its controller (owned by the engine) before the factories, the factories
    // before the pool, the bootstrap before the pool it opened channels on, and
    // the client/monitor the bootstrap points at last of all.
    QQmlApplicationEngine engine;

    // Remote image bytes reach QML as "image://chremote/<percent-encoded path>",
    // fetched through file.readFile like every other remote read. The engine
    // takes ownership of the provider, which is why the service mints one rather
    // than handing out a pointer to something it owns.
    engine.addImageProvider(ch::MobileImageProvider::providerId(),
                            viewerService.createImageProvider());

    // Only what QML actually reads. `sessionLayouts` is published as `layouts`
    // (rather than being reached through `app.layouts` as on the desktop) because
    // the mobile pages bind to the trees directly and a two-hop property path
    // through a possibly-null connection surface would need a guard at every use.
    engine.rootContext()->setContextProperty(QStringLiteral("mobile"), &mobile);
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &appController);
    engine.rootContext()->setContextProperty(QStringLiteral("layouts"), &sessionLayouts);
    engine.rootContext()->setContextProperty(QStringLiteral("terminalFactory"),
                                             &terminalFactory);
    engine.rootContext()->setContextProperty(QStringLiteral("viewerService"),
                                             &viewerService);
    engine.rootContext()->setContextProperty(QStringLiteral("keyStore"), &keyStore);
    engine.rootContext()->setContextProperty(QStringLiteral("editorFactory"),
                                             &editorFactory);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    // Resolves to qrc:/qt/qml/CodeHarbor/Mobile/MobileMain.qml, the entry point
    // of the CodeHarbor.Mobile module that codeharbor_mobile_qmlplugin
    // registers. Named by module and type rather than by that URL so the two
    // spellings cannot drift apart.
    engine.loadFromModule("CodeHarbor.Mobile", "MobileMain");

    return app.exec();
}
