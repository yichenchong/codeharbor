// LIVE gate for the editor workstream (SPEC 8.1-8.7, docs/PLAN.md E).
//
// This is the join nobody had made yet. tst_editorcontroller drives the C++
// EditorController against a FAKE codeharbord on a QLocalSocket pair, with no
// page in the picture at all; tst_livessh proves JSON-RPC over a REAL ssh
// channel; tst_liveviewers proves real remote bytes reaching Chromium. Here
// every piece runs at once: the packaged Monaco bundle in a WebEngineView,
// bridged over a REAL QWebChannel to the REAL ch::EditorController, whose every
// file.* call crosses a REAL ssh channel into a REAL remote codeharbord.
// Nothing is stubbed on either side of the bridge.
//
// What it proves, in order:
//   1. WIRE      — SessionBootstrap connects the pool, execs codeharbord on an
//                  Rpc channel and hands the transport to CodeharbordClient;
//                  server.info answers first so a wiring failure can never be
//                  mistaken for an editor failure.
//   2. LOAD      — a remote file created on an out-of-band ssh shell channel is
//                  opened by the production pane (src/qml/EditorPaneView.qml,
//                  unmodified) and MONACO'S OWN MODEL is read back through
//                  runJavaScript(): model.getValue() === the bytes on the server.
//   3. EDIT+SAVE — the buffer is modified from inside the page through Monaco's
//                  own type handler, Ctrl+S is dispatched as a DOM keydown onto
//                  Monaco's input surface (the binding mountEditor() installs),
//                  EditorController::saved() fires with a NEW revision, and the
//                  bytes ON THE REMOTE DISK — re-read with `cat` on that
//                  out-of-band channel, never through the RPC path under test —
//                  are exactly Monaco's buffer.
//   4. GUARD     — with the buffer dirty the file is mutated EXTERNALLY on the
//                  server; the next save carries the now-stale revision and is
//                  REJECTED (saveConflict, FileState::Conflict, remote bytes
//                  untouched). The page's own conflict UI then recovers: its
//                  "Reload" button adopts the new revision and the next save is
//                  accepted, landing on disk.
//
// Skipped wholesale unless CH_LIVE_SSH is set, so the default suite stays green
// on a machine with no fixture. The ctest registration pins the headless recipe
// (offscreen platform, software Quick backend, Chromium sandbox/GPU off).
//
// PROBE, and why it does not weaken the assertion: the packaged bundle imports
// Monaco as a module and never publishes it, so a page-side assertion needs a
// handle on the standalone API. monaco-editor itself offers one — it assigns
// globalThis.monaco when MonacoEnvironment.globalAPI is set at module-eval time
// — so a DocumentCreation user script in the MAIN world sets exactly that flag
// before the bundle boots. It publishes the SAME api object the bundle uses; the
// editor instance, its model and every byte asserted below are the production
// page's own. Nothing in src/web/editor is changed for the test.

#include "CodeharbordClient.h"
#include "EditorController.h"
#include "EditorFactory.h"
#include "SessionBootstrap.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QtTest>

#include <QByteArray>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJSValue>
#include <QMetaObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QtQuickControls2/QQuickStyle>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include <functional>
#include <memory>
#include <optional>

using ch::CodeharbordClient;
using ch::EditorController;
using ch::EditorFactory;
using ch::RpcError;
using ch::SessionBootstrap;
using ch::SshChannelDevice;
using ch::SshConnectionPool;
using ch::ViewerModel;
using ch::ViewerProfiles;

namespace {

// An ssh connect plus a remote node cold start (which also type-strips the
// TypeScript entry point) is measured in seconds, not milliseconds.
constexpr int kExecTimeoutMs = 30000;
constexpr int kRpcTimeoutMs = 60000;
// Chromium's first navigation pays for the zygote, the renderer start AND
// parsing/executing a ~3.7 MB Monaco bundle on a software rasteriser.
constexpr int kPageTimeoutMs = 180000;
constexpr int kJsTimeoutMs = 30000;
// A bridge round trip is a WebChannel hop plus a full ssh RPC round trip.
constexpr int kSignalTimeoutMs = 60000;
// How long one dispatched Ctrl+S is given to produce ANY save outcome before
// the next input surface is tried. Deliberately shorter than kSignalTimeoutMs:
// it only has to cover a WebChannel hop plus one file.writeFile round trip.
constexpr int kSaveSettleMs = 15000;

// Quote a string for /bin/sh.
QString sq(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// A JavaScript string literal for `value`. JSON string escaping is a subset of
// JS string escaping, so QJsonDocument does the work exactly.
QString jsLiteral(const QString &value)
{
    const QByteArray json =
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    // "[\"...\"]" -> "\"...\""
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

// Spin the event loop until `ready` or the deadline elapses. QTRY_* covers the
// assertions; this is for plumbing steps whose failure needs a custom message.
bool waitFor(const std::function<bool()> &ready, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeoutMs)
        QTest::qWait(20);
    return ready();
}

// Make a multi-line payload printable on one qInfo line.
QString oneLine(const QString &text)
{
    QString out = text;
    out.replace(QLatin1String("\n"), QLatin1String("\\n"));
    return out;
}

// ---------------------------------------------------------------------------
// Host shell. Deliberately thin: it owns nothing but a Loader, because the pane
// under test is the REAL src/qml/EditorPaneView.qml loaded from the CodeHarbor
// module's qrc — the same URL the shipped binary resolves. Everything the pane
// needs (`editorFactory`, `viewers`) arrives as context properties exactly as
// main.cpp supplies them, so the WebChannel registration, the privileged
// profile, the bundle URL and the open-on-completion sequence are production
// code, not a re-implementation.
//
// evalJs()/jsResult are the readout channel: runJavaScript is asynchronous and
// callback-based, so the callback lands in a property the C++ side polls.
// ---------------------------------------------------------------------------
constexpr auto kShellQml = R"QML(
import QtQuick
import QtQuick.Window
import QtWebEngine

Window {
    id: win
    width: 1000
    height: 680
    visible: true
    color: "#1e1e1e"

    property string jsResult: ""
    property bool jsFinished: false

    Loader {
        id: paneLoader
        objectName: "paneLoader"
        anchors.fill: parent
    }

    // Publish monaco-editor's own standalone API on the page's window object by
    // setting the flag the library already checks (globalThis.monaco is assigned
    // when MonacoEnvironment.globalAPI is truthy at module-eval time). Injected
    // at DocumentCreation into the MAIN world so it lands BEFORE the bundle
    // boots; the bundle overwrites MonacoEnvironment with its own worker factory
    // immediately afterwards, which is why only the flag is set here.
    function installProbe() {
        var profile = viewers.internalProfile();
        profile.userScripts.collection = [{
            name: "chLiveEditorGlobalApi",
            sourceCode: "globalThis.MonacoEnvironment = { globalAPI: true };",
            injectionPoint: WebEngineScript.DocumentCreation,
            worldId: WebEngineScript.MainWorld,
            runsOnSubFrames: false
        }];
        return profile.userScripts.collection.length;
    }

    function openPane(remoteUrl) {
        paneLoader.setSource("qrc:/qt/qml/CodeHarbor/EditorPaneView.qml",
                             { fileUrl: remoteUrl, paneId: "viewer-1" });
        return paneLoader.status === Loader.Ready
            ? "ready" : ("loader-status=" + paneLoader.status);
    }

    function paneItem() {
        return paneLoader.item;
    }

    function paneController() {
        return paneLoader.item ? paneLoader.item.controller : null;
    }

    // The pane's WebEngineView, found structurally: EditorPaneView keeps it
    // private (no objectName), and a test has no business editing it.
    function webView() {
        var pane = paneLoader.item;
        if (!pane)
            return null;
        for (var i = 0; i < pane.children.length; ++i) {
            var child = pane.children[i];
            if (child && typeof child.runJavaScript === "function")
                return child;
        }
        return null;
    }

    function evalJs(script) {
        win.jsFinished = false;
        win.jsResult = "";
        var view = webView();
        if (!view) {
            win.jsResult = "NO_VIEW";
            win.jsFinished = true;
            return;
        }
        view.runJavaScript(script, function(result) {
            win.jsResult = (result === undefined || result === null)
                ? "" : String(result);
            win.jsFinished = true;
        });
    }
}
)QML";

// ---- page-side probes -----------------------------------------------------

// Monaco is mounted and the bridge handshake has run.
constexpr auto kJsProbeReady = R"JS(
(function () {
    try {
        if (typeof window.monaco === "undefined") return "NO_MONACO";
        if (!window.monaco.editor.getModels().length) return "NO_MODEL";
        if (!window.monaco.editor.getEditors().length) return "NO_EDITOR";
        return "READY";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// The authoritative page-side readout: Monaco's own text model.
constexpr auto kJsReadModel = R"JS(
(function () {
    try { return "VAL:" + window.monaco.editor.getModels()[0].getValue(); }
    catch (e) { return "ERR:" + e; }
})()
)JS";

// The status bar mountEditor() renders from fileStateChanged.
constexpr auto kJsReadStatus = R"JS(
(function () {
    try {
        var el = document.querySelector(".ch-editor-state");
        return "STATUS:" + (el ? el.textContent : "<none>");
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// The conflict affordance mountEditor() renders from saveConflict. It stays up
// until the user resolves the conflict, so it is the stable page-side evidence
// that the refusal reached the UI — stabler than the status label, which the
// user's next keystroke re-renders with its dirty mark. (EditorController no
// longer downgrades Conflict to ExternallyModified when the watch event for the
// same external write lands, so the label itself is not racy any more; the
// notice is still the thing that proves the PAGE was told.)
constexpr auto kJsReadNotice = R"JS(
(function () {
    try {
        var notice = document.querySelector(".ch-editor-notice");
        if (!notice || notice.style.display === "none") return "NO_NOTICE";
        var buttons = notice.querySelectorAll("button");
        var labels = [];
        for (var i = 0; i < buttons.length; ++i) labels.push(buttons[i].textContent);
        return "NOTICE:" + notice.textContent + " [" + labels.join("|") + "]";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// Click the "Reload" affordance mountEditor() renders on saveConflict.
constexpr auto kJsClickReload = R"JS(
(function () {
    try {
        var buttons = document.querySelectorAll(".ch-editor-notice button");
        if (!buttons.length) return "NO_NOTICE";
        var labels = [];
        for (var i = 0; i < buttons.length; ++i) labels.push(buttons[i].textContent);
        for (var i = 0; i < buttons.length; ++i) {
            if (buttons[i].textContent === "Reload") {
                buttons[i].click();
                return "CLICKED[" + labels.join("|") + "]";
            }
        }
        return "NO_RELOAD[" + labels.join("|") + "]";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// Monaco's OWN read-only option, read off the editor instance the bundle
// created. This is the thing that actually decides whether keystrokes mutate
// the model, so it is the only page-side answer worth asserting.
constexpr auto kJsReadReadOnly = R"JS(
(function () {
    try {
        var editor = window.monaco.editor.getEditors()[0];
        return "RO:" + editor.getOption(window.monaco.editor.EditorOption.readOnly);
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// Insert text at the end of the buffer through Monaco's own type handler — the
// same command path a keystroke takes — so the PAGE is the source of the edit.
QString jsTypeAtEnd(const QString &text)
{
    return QStringLiteral(R"JS(
(function () {
    try {
        var editor = window.monaco.editor.getEditors()[0];
        var model = editor.getModel();
        editor.focus();
        var last = model.getLineCount();
        editor.setPosition({ lineNumber: last, column: model.getLineMaxColumn(last) });
        editor.trigger("keyboard", "type", { text: %1 });
        return "TYPED:" + model.getValue().length;
    } catch (e) { return "ERR:" + e; }
})()
)JS")
        .arg(jsLiteral(text));
}

// Dispatch a real Ctrl+S keydown at `target` (a JS expression). Monaco's
// keybinding service listens on the editor's DOM subtree, so this is the same
// wiring a user's Ctrl+S goes through: mountEditor()'s
// addCommand(CtrlCmd|KeyS) -> bridge.save(model value, loaded revision).
QString jsCtrlS(const QString &targetExpression)
{
    return QStringLiteral(R"JS(
(function () {
    try {
        var editor = window.monaco.editor.getEditors()[0];
        editor.focus();
        var target = %1;
        if (!target) return "NO_TARGET";
        target.dispatchEvent(new KeyboardEvent("keydown", {
            key: "s", code: "KeyS", keyCode: 83, which: 83,
            ctrlKey: true, bubbles: true, cancelable: true, composed: true
        }));
        return "DISPATCHED:" + (target.className || target.tagName || "?");
    } catch (e) { return "ERR:" + e; }
})()
)JS")
        .arg(targetExpression);
}

} // namespace

class TstLiveEditor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // (2) The remote file's bytes are in Monaco's model, read from the page.
    void remoteFileLoadsIntoRealMonaco();
    // (3) An edit made inside the page, saved with Ctrl+S, lands on the server.
    void pageEditSavesToRemoteDisk();
    // (4) SPEC 8.6 live: stale revision refused, page's Reload recovers.
    void staleRevisionIsRejectedAndReloadRecovers();
    // (5) SPEC 8.2 live: a real chmod 444 remote file is read-only in Monaco,
    //     refuses to save, and becomes editable again when the mode is restored.
    void unwritableRemoteFileIsReadOnlyInMonaco();

private:
    // Start the ONE out-of-band shell channel every remoteExec() rides on.
    bool startRemoteShell();
    bool remoteExec(const QString &command, QByteArray *stdoutText = nullptr,
                    QString *stderrText = nullptr);
    // Exact bytes of the remote file, read back on the out-of-band shell
    // channel — never through the RPC path under test.
    QString remoteBytes();
    QString runJs(const QString &script, int timeoutMs = kJsTimeoutMs);
    // Monaco's model value, or a QVERIFY-friendly failure marker.
    QString monacoValue();
    QString pageStatus();
    // Ctrl+S on the first input surface whose keydown actually reaches Monaco's
    // keybinding service, i.e. the first one after which `settled` becomes true.
    // `settled` must cover EVERY outcome of the save round trip (saved, conflict
    // AND error), or a refused save would be retried on the next surface and
    // this would fire two saves for one Ctrl+S. Returns the JS result of the
    // dispatch that settled (or the last failure).
    QString pressCtrlS(const std::function<bool()> &settled);
    void typeInPage(const QString &text);
    // Block until the controller knows the buffer carries unsaved edits.
    void waitUntilDirty();

    // Declaration order IS construction order; the QML engine and the shell are
    // declared last so they are destroyed FIRST — no QML tree may unbind (or an
    // EditorController unwatch) against a dead client or a closed transport.
    SshConnectionPool m_pool;
    CodeharbordClient m_client;
    ViewerProfiles m_profiles{&m_client};
    ViewerModel m_viewers{&m_client};
    EditorFactory m_editorFactory{&m_client};
    // The server-reported recovery directory (server.info.recoveryDir),
    // captured in initTestCase and forwarded to the factory exactly as
    // AppController::adoptServerIdentity does in production (SPEC 11.3).
    QString m_recoveryDir;
    std::unique_ptr<SessionBootstrap> m_bootstrap;
    std::unique_ptr<QQmlEngine> m_engine;
    // The host Window (kShellQml). Named for what it is, so it is never confused
    // with m_execShell, the remote /bin/sh below.
    std::unique_ptr<QObject> m_window;

    // The out-of-band shell channel and its accumulated streams (see
    // startRemoteShell()). Declared after m_pool so it is torn down first.
    std::unique_ptr<SshChannelDevice> m_execShell;
    QByteArray m_execOut;
    QString m_execErr;
    int m_execSeq = 0;

    QPointer<EditorController> m_controller;
    QString m_remoteDir;
    QString m_filePath;
    QString m_marker;
    QString m_initialContent;
    QString m_loadedRevision;
    QStringList m_bootstrapErrors;
    bool m_live = false;
};

void TstLiveEditor::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live editor gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live editor gate skipped");

    m_viewers.setProfiles(&m_profiles);

    // ---- (1) WIRE ---------------------------------------------------------
    // The production seam, byte for byte: pool connect -> `codeharbord rpc
    // --stdio` on an ssh Rpc channel -> client->setTransport(). Chosen over
    // hand-rolling SshChannelDevice because the editor pane in the shipped app
    // is fed by exactly this object, and a bug in it must fail this gate too.
    m_bootstrap = std::make_unique<SessionBootstrap>(&m_pool, &m_client, nullptr);
    connect(m_bootstrap.get(), &SessionBootstrap::error, this,
            [this](const QString &message) { m_bootstrapErrors.append(message); });

    QVERIFY2(m_bootstrap->connectAndWireFromEnvironment(),
             qPrintable(QStringLiteral("SessionBootstrap could not wire a live session: %1")
                            .arg(m_bootstrapErrors.join(QStringLiteral(" | ")))));
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY(m_bootstrap->rpcDevice() != nullptr);
    QCOMPARE(m_client.transport(), static_cast<QIODevice *>(m_bootstrap->rpcDevice()));

    // Answer server.info BEFORE anything editor-shaped runs, so a dead server
    // fails here with a clear message instead of as an empty Monaco buffer.
    QJsonValue info;
    std::optional<RpcError> rpcError;
    bool answered = false;
    m_client.call(QStringLiteral("server.info"), QJsonValue(),
                  [&](QJsonValue value, std::optional<RpcError> err) {
                      info = value;
                      rpcError = err;
                      answered = true;
                  });
    QVERIFY2(waitFor([&] { return answered; }, kRpcTimeoutMs),
             qPrintable(QStringLiteral("server.info never answered. bootstrap: %1")
                            .arg(m_bootstrapErrors.join(QStringLiteral(" | ")))));
    QVERIFY2(!rpcError.has_value(), qPrintable(rpcError ? rpcError->message : QString()));
    QCOMPARE(info.toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("codeharbord"));
    QVERIFY(info.toObject().value(QStringLiteral("schemaVersion")).toInt(-1) >= 2);

    // Mirror AppController::adoptServerIdentity: forward the server-reported
    // recovery directory (SPEC 11.3) to the editor factory, since this harness
    // bypasses AppController. Per-pane snapshots land at <recoveryDir>/<paneId>.
    m_recoveryDir = info.toObject().value(QStringLiteral("recoveryDir")).toString();
    QVERIFY2(!m_recoveryDir.isEmpty(),
             "server.info reported no recoveryDir; the SPEC 11.3 snapshot path is unknown");
    m_editorFactory.setRecoveryDir(m_recoveryDir);

    // ---- (2a) REMOTE FIXTURE ---------------------------------------------
    // Created over an ssh Exec channel rather than with file.writeFile: the
    // fixture must not depend on the very RPC surface the gate is testing, so a
    // broken writeFile shows up as a failed assertion, not as a missing file.
    // Only the working directory is pre-created; the SPEC 11.3 recovery snapshot
    // now lands under the SERVER-reported recovery directory
    // (server.info.recoveryDir), whose parent file.writeFile creates on the
    // first write, so no sibling directory is pre-made here.
    QVERIFY2(startRemoteShell(),
             qPrintable(QStringLiteral("could not start the out-of-band shell channel: %1")
                            .arg(m_execErr)));
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    m_remoteDir = QStringLiteral("/tmp/ch-live-editor-%1").arg(token);
    m_filePath = m_remoteDir + QStringLiteral("/note.txt");
    m_marker = QStringLiteral("CH_LIVE_EDITOR_%1").arg(token.toUpper());
    m_initialContent = m_marker + QStringLiteral("\n")
                       + QStringLiteral("second line: these bytes crossed a real ssh channel\n")
                       + QStringLiteral("third line: monaco owns this buffer\n");

    const QString setup =
        QStringList{
            QStringLiteral("mkdir -p"),
            sq(m_remoteDir),
            QStringLiteral("&& printf '%s\\n' "),
            sq(m_marker),
            sq(QStringLiteral("second line: these bytes crossed a real ssh channel")),
            sq(QStringLiteral("third line: monaco owns this buffer")),
            QStringLiteral(">"),
            sq(m_filePath),
            QStringLiteral("&& echo SETUP_OK"),
        }
            .join(QLatin1Char(' '));

    QByteArray setupOut;
    QString setupErr;
    QVERIFY2(remoteExec(setup, &setupOut, &setupErr),
             qPrintable(QStringLiteral("remote fixture setup timed out: %1").arg(setupErr)));
    QVERIFY2(setupOut.contains("SETUP_OK"),
             qPrintable(QStringLiteral("remote fixture setup failed: out=%1 err=%2")
                            .arg(QString::fromUtf8(setupOut), setupErr)));
    QCOMPARE(remoteBytes(), m_initialContent);

    // ---- (2b) HOST --------------------------------------------------------
    m_engine = std::make_unique<QQmlEngine>();
    m_engine->rootContext()->setContextProperty(QStringLiteral("viewers"), &m_viewers);
    m_engine->rootContext()->setContextProperty(QStringLiteral("editorFactory"),
                                                &m_editorFactory);

    QQmlComponent component(m_engine.get());
    component.setData(QByteArray(kShellQml), QUrl(QStringLiteral("qrc:/tst_liveeditor/shell.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    m_window.reset(component.create());
    QVERIFY2(m_window != nullptr, qPrintable(component.errorString()));

    // The page-side probe must be installed before the pane navigates.
    QVariant installed;
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "installProbe",
                                      Q_RETURN_ARG(QVariant, installed)));
    QCOMPARE(installed.toInt(), 1);

    m_live = true;
    qInfo("live editor fixture ready: file=%s marker=%s", qPrintable(m_filePath),
          qPrintable(m_marker));
}

void TstLiveEditor::cleanupTestCase()
{
    // Window first: the pane owns the EditorController, whose destructor releases
    // the server-side file.watch through the still-live transport (SPEC 8.7).
    m_window.reset();
    m_engine.reset();

    if (m_live && !m_remoteDir.isEmpty()) {
        QByteArray out;
        QString err;
        const bool ok = remoteExec(QStringLiteral("rm -rf ") + sq(m_remoteDir)
                                       + QStringLiteral(" && echo CLEANUP_OK"),
                                   &out, &err);
        if (!ok || !out.contains("CLEANUP_OK")) {
            qWarning("remote cleanup of %s may have failed: out=%s err=%s",
                     qPrintable(m_remoteDir), out.constData(), qPrintable(err));
        } else {
            qInfo("removed remote fixture %s", qPrintable(m_remoteDir));
        }
    }

    if (m_execShell) {
        // EOF on stdin is what reaps the remote `sh`; an exec channel sends no
        // SIGHUP, so this close IS the shutdown signal.
        m_execShell->closeChannel();
        m_execShell.reset();
    }

    // Drops both ssh channels (Rpc + the bridge relay, which self-terminates on
    // stdin EOF), then the session itself: no orphan remote node processes.
    m_bootstrap.reset();
    m_pool.disconnectFromHost();
}

bool TstLiveEditor::startRemoteShell()
{
    // ONE ssh channel carries every out-of-band command this gate runs, rather
    // than one channel per command. That is not tidiness, it is a hard limit:
    // sshd caps concurrent session channels per connection (MaxSessions, 10 by
    // default) and this session already spends two of them on the RPC and
    // bridge channels — and a finished exec channel does NOT give its slot back
    // here, because SshChannelDevice::closeChannel() only sends the SSH close
    // while ssh_channel_is_open() is still true, which it no longer is once the
    // remote command has exited. A channel per command therefore wedges the
    // whole session part-way through the run.
    //
    // `sh -s` reads its command stream from stdin and exits on EOF, so closing
    // the channel reaps it — the same stdin-driven lifetime
    // SessionBootstrap::bridgeCommand() relies on, because an exec channel
    // sends no SIGHUP.
    m_execShell = std::make_unique<SshChannelDevice>(&m_pool,
                                                     SshConnectionPool::ChannelKind::Exec);
    connect(m_execShell.get(), &QIODevice::readyRead, m_execShell.get(),
            [this] { m_execOut += m_execShell->readAll(); });
    connect(m_execShell.get(), &SshChannelDevice::channelError, m_execShell.get(),
            [this](const QString &text) { m_execErr += text; });
    return m_execShell->startExec(QStringLiteral("/bin/sh -s"));
}

bool TstLiveEditor::remoteExec(const QString &command, QByteArray *stdoutText,
                               QString *stderrText)
{
    if (stdoutText)
        stdoutText->clear();
    if (stderrText)
        stderrText->clear();
    if (!m_execShell)
        return false;

    // Commands share one stdout stream, so each one is terminated by a unique
    // sentinel line: everything before it is this command's output, byte for
    // byte (a file with no trailing newline simply puts the sentinel on the
    // same line, and cutting at its offset still yields the exact bytes).
    const QByteArray sentinel = QByteArrayLiteral("__CH_EXEC_DONE_")
                                + QByteArray::number(++m_execSeq)
                                + QByteArrayLiteral("__");
    m_execOut.clear();
    m_execErr.clear();

    const QString line = command + QStringLiteral("\nprintf '%s\\n' ")
                         + QString::fromUtf8(sentinel) + QStringLiteral("\n");
    if (m_execShell->write(line.toUtf8()) < 0) {
        if (stderrText)
            *stderrText = m_execErr + QStringLiteral(" (write to shell channel failed)");
        return false;
    }

    const bool done = waitFor([&] { return m_execOut.contains(sentinel); }, kExecTimeoutMs);
    if (stdoutText) {
        const qsizetype at = m_execOut.indexOf(sentinel);
        *stdoutText = at >= 0 ? m_execOut.left(at) : m_execOut;
    }
    if (stderrText)
        *stderrText = m_execErr;
    return done;
}

QString TstLiveEditor::remoteBytes()
{
    QByteArray out;
    QString err;
    if (!remoteExec(QStringLiteral("cat -- ") + sq(m_filePath), &out, &err))
        return QStringLiteral("<cat timed out: %1>").arg(err);
    return QString::fromUtf8(out);
}

QString TstLiveEditor::runJs(const QString &script, int timeoutMs)
{
    if (!m_window)
        return QStringLiteral("NO_WINDOW");
    if (!QMetaObject::invokeMethod(m_window.get(), "evalJs", Q_ARG(QVariant, QVariant(script))))
        return QStringLiteral("INVOKE_FAILED");
    if (!waitFor([this] { return m_window->property("jsFinished").toBool(); }, timeoutMs))
        return QStringLiteral("JS_TIMEOUT");
    return m_window->property("jsResult").toString();
}

QString TstLiveEditor::monacoValue()
{
    const QString raw = runJs(QString::fromLatin1(kJsReadModel));
    if (!raw.startsWith(QLatin1String("VAL:")))
        return raw; // NO_MONACO / ERR:... / JS_TIMEOUT — surfaced by the caller
    return raw.mid(4);
}

QString TstLiveEditor::pageStatus()
{
    const QString raw = runJs(QString::fromLatin1(kJsReadStatus));
    return raw.startsWith(QLatin1String("STATUS:")) ? raw.mid(7) : raw;
}

void TstLiveEditor::typeInPage(const QString &text)
{
    const QString typed = runJs(jsTypeAtEnd(text));
    QVERIFY2(typed.startsWith(QLatin1String("TYPED:")), qPrintable(typed));
}

void TstLiveEditor::waitUntilDirty()
{
    // The page's debounced reportContent (SPEC 11.3) is what tells the
    // controller, and which state that lands in depends on whether a watch event
    // for an external write has already arrived. Both "modified" and
    // "externally_modified" mean dirty, and dirty is the only thing that matters
    // here: a CLEAN buffer would be silently auto-reloaded instead (SPEC 8.7),
    // which would re-baseline the revision and defeat the guard being tested.
    QString state;
    QVERIFY2(waitFor([&] {
                 state = m_controller->fileState();
                 return state == QLatin1String("modified")
                        || state == QLatin1String("externally_modified");
             }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("the buffer never went dirty; fileState=%1").arg(state)));
}

QString TstLiveEditor::pressCtrlS(const std::function<bool()> &settled)
{
    // Monaco's input surface moved between releases (textarea.inputarea, then
    // the edit-context div), so try the plausible ones in order rather than
    // pinning the gate to whichever 0.52 happens to render. The editor's
    // container is the last resort: the keybinding service listens on the whole
    // subtree, so a bubbling keydown from any of these reaches it.
    const QStringList targets{
        QStringLiteral(R"(document.querySelector(".monaco-editor textarea.inputarea"))"),
        QStringLiteral(R"(document.querySelector(".monaco-editor .native-edit-context"))"),
        QStringLiteral(R"(window.monaco.editor.getEditors()[0].getContainerDomNode())"),
    };

    QString last;
    for (const QString &target : targets) {
        last = runJs(jsCtrlS(target));
        if (!last.startsWith(QLatin1String("DISPATCHED")))
            continue;
        if (waitFor(settled, kSaveSettleMs))
            return last;
    }
    return last;
}

// ---------------------------------------------------------------------------
// (2) The remote file reaches Monaco's model.
// ---------------------------------------------------------------------------
void TstLiveEditor::remoteFileLoadsIntoRealMonaco()
{
    QVERIFY(m_window != nullptr);

    // The production pane: it creates its controller through the `editorFactory`
    // context property, registers it on a QWebChannel under the object name
    // "editor", calls open() and navigates to the packaged bundle.
    QVariant opened;
    QVERIFY(QMetaObject::invokeMethod(
        m_window.get(), "openPane", Q_RETURN_ARG(QVariant, opened),
        Q_ARG(QVariant, QVariant::fromValue(QUrl::fromLocalFile(m_filePath)))));
    QCOMPARE(opened.toString(), QStringLiteral("ready"));

    QVariant controllerValue;
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "paneController",
                                      Q_RETURN_ARG(QVariant, controllerValue)));
    QObject *controllerObject = controllerValue.value<QObject *>();
    if (!controllerObject)
        controllerObject = controllerValue.value<QJSValue>().toQObject();
    m_controller = qobject_cast<EditorController *>(controllerObject);
    QVERIFY2(m_controller != nullptr,
             "EditorPaneView did not expose a ch::EditorController");
    QCOMPARE(m_controller->path(), m_filePath);

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    // Chromium start + bundle parse + WebChannel handshake + mountEditor().
    QString probe;
    QVERIFY2(waitFor([&] {
                 probe = runJs(QString::fromLatin1(kJsProbeReady));
                 return probe == QLatin1String("READY");
             }, kPageTimeoutMs),
             qPrintable(QStringLiteral("Monaco never came up in the page: %1").arg(probe)));

    // contentLoaded is held by the controller until the page's ready() arrives
    // (the C3 handshake), so by construction it fires only once the page can
    // receive it.
    QVERIFY2(waitFor([&] { return contentSpy.count() >= 1; }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("contentLoaded never fired; fileState=%1")
                            .arg(m_controller->fileState())));
    QCOMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(), m_initialContent);
    m_loadedRevision = contentSpy.at(0).at(1).toString();
    QVERIFY(!m_loadedRevision.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(m_controller->fileState(), QStringLiteral("clean"),
                              kSignalTimeoutMs);

    // THE assertion: Monaco's own model, read from inside the page, holds the
    // bytes that are on the remote disk.
    QString value;
    QVERIFY2(waitFor([&] { value = monacoValue(); return value == m_initialContent; },
                     kSignalTimeoutMs),
             qPrintable(QStringLiteral("monaco model != remote bytes.\n  model: %1\n  file:  %2")
                            .arg(oneLine(value), oneLine(m_initialContent))));

    const QString onDisk = remoteBytes();
    QCOMPARE(value, onDisk);
    QVERIFY(value.contains(m_marker));

    qInfo("JS monaco.editor.getModels()[0].getValue() = \"%s\"", qPrintable(oneLine(value)));
    qInfo("remote `cat %s`                            = \"%s\"", qPrintable(m_filePath),
          qPrintable(oneLine(onDisk)));
    qInfo("loaded revision = %s   page status = \"%s\"", qPrintable(m_loadedRevision),
          qPrintable(pageStatus()));
}

// ---------------------------------------------------------------------------
// (3) An edit made in the page is saved onto the remote disk.
// ---------------------------------------------------------------------------
void TstLiveEditor::pageEditSavesToRemoteDisk()
{
    QVERIFY2(m_controller != nullptr, "the pane never came up; see the load case");
    QVERIFY(!m_loadedRevision.isEmpty());

    const QString edit = QStringLiteral("edited in monaco %1\n").arg(m_marker);
    typeInPage(edit);

    // The page owns the buffer, so the buffer it now holds is the expectation
    // for both the save payload and the bytes on disk.
    const QString expected = monacoValue();
    QVERIFY2(expected.startsWith(m_initialContent), qPrintable(oneLine(expected)));
    QVERIFY2(expected.contains(edit.trimmed()), qPrintable(oneLine(expected)));

    // The debounced reportContent (SPEC 11.3) marks the buffer dirty C++-side.
    waitUntilDirty();

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    QSignalSpy errorSpy(m_controller, &EditorController::saveError);

    const QString dispatch = pressCtrlS(
        [&] { return savedSpy.count() + conflictSpy.count() + errorSpy.count() > 0; });
    QVERIFY2(dispatch.startsWith(QLatin1String("DISPATCHED")), qPrintable(dispatch));
    QVERIFY2(waitFor([&] { return savedSpy.count() == 1; }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("saved() never fired. conflicts=%1 errors=%2 state=%3")
                            .arg(conflictSpy.count())
                            .arg(errorSpy.count())
                            .arg(m_controller->fileState())));
    QCOMPARE(conflictSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);

    const QString newRevision = savedSpy.at(0).at(0).toString();
    QVERIFY(!newRevision.isEmpty());
    QVERIFY2(newRevision != m_loadedRevision,
             qPrintable(QStringLiteral("revision did not advance: %1").arg(newRevision)));
    QCOMPARE(m_controller->revision(), newRevision);

    // The criterion that closes the gate: the bytes ON THE SERVER, read back on
    // an independent ssh channel, are the page's buffer.
    const QString onDisk = remoteBytes();
    QCOMPARE(onDisk, expected);
    QVERIFY(onDisk.contains(edit.trimmed()));

    qInfo("ctrl+s dispatch target: %s", qPrintable(dispatch));
    qInfo("revision %s -> %s", qPrintable(m_loadedRevision), qPrintable(newRevision));
    qInfo("remote bytes after save = \"%s\"", qPrintable(oneLine(onDisk)));

    m_loadedRevision = newRevision;
}

// ---------------------------------------------------------------------------
// (4) SPEC 8.6, live: a stale revision is refused, and the page's own conflict
//     UI recovers.
// ---------------------------------------------------------------------------
void TstLiveEditor::staleRevisionIsRejectedAndReloadRecovers()
{
    QVERIFY2(m_controller != nullptr, "the pane never came up; see the load case");
    QVERIFY(!m_loadedRevision.isEmpty());

    // 1. Dirty the buffer FIRST. A clean buffer would be auto-reloaded when the
    //    watch event lands (SPEC 8.7), adopting the new revision and making the
    //    save legal — the guard is only meaningful with local edits pending.
    const QString doomedEdit = QStringLiteral("this edit must never reach the disk %1\n")
                                   .arg(m_marker);
    typeInPage(doomedEdit);
    waitUntilDirty();

    // 2. Mutate the file EXTERNALLY: mtime, ctime and size all move, so the
    //    revision the buffer is guarded at is now stale.
    const QString externalLine =
        QStringLiteral("external writer touched this file %1").arg(m_marker);
    QByteArray out;
    QString err;
    QVERIFY2(remoteExec(QStringLiteral("printf '%s\\n' ") + sq(externalLine)
                            + QStringLiteral(" >> ") + sq(m_filePath)
                            + QStringLiteral(" && echo APPEND_OK"),
                        &out, &err),
             qPrintable(QStringLiteral("external append timed out: %1").arg(err)));
    QVERIFY2(out.contains("APPEND_OK"), qPrintable(QString::fromUtf8(out) + err));

    const QString externalBytes = remoteBytes();
    QVERIFY(externalBytes.contains(externalLine));
    QVERIFY2(!externalBytes.contains(doomedEdit.trimmed()), qPrintable(oneLine(externalBytes)));

    // 3. Save with the stale revision: MUST be refused, never silently applied.
    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    QSignalSpy errorSpy(m_controller, &EditorController::saveError);
    // The state HISTORY, not the state at some later instant: the watch event
    // for the same external write can land after the conflict and move the
    // machine on to externally_modified, which would make a poll for the final
    // value racy. Entering Conflict at all is the assertion.
    QSignalSpy stateSpy(m_controller, &EditorController::fileStateChanged);

    const QString dispatch = pressCtrlS(
        [&] { return savedSpy.count() + conflictSpy.count() + errorSpy.count() > 0; });
    QVERIFY2(dispatch.startsWith(QLatin1String("DISPATCHED")), qPrintable(dispatch));
    QVERIFY2(waitFor([&] { return conflictSpy.count() == 1; }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("saveConflict never fired. saved=%1 errors=%2 state=%3")
                            .arg(savedSpy.count())
                            .arg(errorSpy.count())
                            .arg(m_controller->fileState())));
    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);
    QStringList states;
    for (const QList<QVariant> &emission : stateSpy)
        states.append(emission.at(0).toString());
    QVERIFY2(states.contains(QStringLiteral("conflict")),
             qPrintable(QStringLiteral("file state never entered conflict: [%1]")
                            .arg(states.join(QStringLiteral(" -> ")))));

    const QString conflictRevision = conflictSpy.at(0).at(0).toString();
    QVERIFY(!conflictRevision.isEmpty());
    QVERIFY(conflictRevision != m_loadedRevision);

    // ...and the server's bytes are untouched: no silent overwrite.
    const QString afterConflict = remoteBytes();
    QCOMPARE(afterConflict, externalBytes);
    QVERIFY2(!afterConflict.contains(doomedEdit.trimmed()),
             qPrintable(QStringLiteral("stale save CLOBBERED the remote file: %1")
                            .arg(oneLine(afterConflict))));

    qInfo("stale save refused: revision %s is stale, server holds %s",
          qPrintable(m_loadedRevision), qPrintable(conflictRevision));
    qInfo("remote bytes after refused save = \"%s\"", qPrintable(oneLine(afterConflict)));
    // The PAGE knows too: saveConflict crossed the bridge and mountEditor()
    // rendered its reload/overwrite affordance. Waited for, because the
    // WebChannel hop lands after the C++-side signal.
    QString notice;
    QVERIFY2(waitFor([&] {
                 notice = runJs(QString::fromLatin1(kJsReadNotice));
                 return notice.startsWith(QStringLiteral("NOTICE:"));
             }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("page never rendered the conflict notice: \"%1\"")
                            .arg(notice)));
    QVERIFY2(notice.contains(QStringLiteral("File changed on disk.")), qPrintable(notice));
    QVERIFY2(notice.endsWith(QStringLiteral("[Reload|Overwrite]")), qPrintable(notice));
    qInfo("page conflict notice = %s   (status label: \"%s\")", qPrintable(notice),
          qPrintable(pageStatus()));

    // 4. Recovery through the page's OWN conflict affordance: "Reload" calls
    //    bridge.requestReload(), which re-fetches and re-baselines the revision.
    QSignalSpy reloadSpy(m_controller, &EditorController::contentLoaded);
    const QString clicked = runJs(QString::fromLatin1(kJsClickReload));
    QVERIFY2(clicked.startsWith(QLatin1String("CLICKED")), qPrintable(clicked));
    QVERIFY2(waitFor([&] { return reloadSpy.count() == 1; }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("reload never delivered content; state=%1")
                            .arg(m_controller->fileState())));

    const QString reloadedRevision = reloadSpy.at(0).at(1).toString();
    QCOMPARE(reloadSpy.at(0).at(0).toString(), externalBytes);
    QCOMPARE(reloadedRevision, conflictRevision);
    QTRY_COMPARE_WITH_TIMEOUT(m_controller->fileState(), QStringLiteral("clean"),
                              kSignalTimeoutMs);

    // The page adopted it: Monaco now shows the external content, not the edit
    // that was refused.
    QString reloadedValue;
    QVERIFY2(waitFor([&] { reloadedValue = monacoValue(); return reloadedValue == externalBytes; },
                     kSignalTimeoutMs),
             qPrintable(QStringLiteral("monaco did not adopt the reloaded file: %1")
                            .arg(oneLine(reloadedValue))));

    // 5. A save on the fresh revision is accepted and lands on disk.
    const QString recoveredEdit =
        QStringLiteral("saved after reload %1\n").arg(m_marker);
    typeInPage(recoveredEdit);
    const QString expected = monacoValue();
    waitUntilDirty();

    const int settledBefore = savedSpy.count() + conflictSpy.count() + errorSpy.count();
    const QString dispatch2 = pressCtrlS([&] {
        return savedSpy.count() + conflictSpy.count() + errorSpy.count() > settledBefore;
    });
    QVERIFY2(dispatch2.startsWith(QLatin1String("DISPATCHED")), qPrintable(dispatch2));
    QVERIFY2(waitFor([&] { return savedSpy.count() == 1; }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("post-reload save never succeeded. conflicts=%1 state=%2")
                            .arg(conflictSpy.count())
                            .arg(m_controller->fileState())));
    QCOMPARE(conflictSpy.count(), 1); // still just the one from step 3

    const QString finalRevision = savedSpy.at(0).at(0).toString();
    QVERIFY(!finalRevision.isEmpty());
    QVERIFY(finalRevision != reloadedRevision);

    const QString finalBytes = remoteBytes();
    QCOMPARE(finalBytes, expected);
    QVERIFY(finalBytes.contains(externalLine));
    QVERIFY(finalBytes.contains(recoveredEdit.trimmed()));
    QVERIFY2(!finalBytes.contains(doomedEdit.trimmed()), qPrintable(oneLine(finalBytes)));

    qInfo("recovered: revision %s -> %s", qPrintable(reloadedRevision),
          qPrintable(finalRevision));
    qInfo("remote bytes after recovery save = \"%s\"", qPrintable(oneLine(finalBytes)));

    m_loadedRevision = finalRevision;
}

// ---------------------------------------------------------------------------
// (5) SPEC 8.2, live: a file the session user genuinely cannot write.
//
// Read-only used to be a setter nobody called, so an unwritable file opened
// freely editable, accepted keystrokes, snapshotted them for crash recovery and
// only failed at save time. Everything here is real: a chmod 444 file on the
// fixture host, the production pane, Monaco's own readOnly option, and the
// bytes on the remote disk read back on an independent ssh channel.
// ---------------------------------------------------------------------------
void TstLiveEditor::unwritableRemoteFileIsReadOnlyInMonaco()
{
    QVERIFY2(m_controller != nullptr, "the pane never came up; see the load case");

    QByteArray out;
    QString err;

    // Mode bits mean nothing to root, and the derivation deliberately declines
    // to guess at a euid it cannot see. Say so out loud rather than passing
    // vacuously on a root fixture.
    QVERIFY(remoteExec(QStringLiteral("id -u"), &out, &err));
    if (QString::fromUtf8(out).trimmed() == QLatin1String("0"))
        QSKIP("the fixture user is root, for whom 0444 is still writable");

    const QString lockedPath = m_remoteDir + QStringLiteral("/locked.txt");
    const QString lockedContent =
        QStringLiteral("read-only fixture %1\n").arg(m_marker);
    const QString recoveryPath = m_recoveryDir + QStringLiteral("/viewer-1");

    QVERIFY2(remoteExec(QStringLiteral("printf '%s\\n' ")
                            + sq(QStringLiteral("read-only fixture ") + m_marker)
                            + QStringLiteral(" > ") + sq(lockedPath)
                            + QStringLiteral(" && chmod 444 ") + sq(lockedPath)
                            + QStringLiteral(" && test ! -w ") + sq(lockedPath)
                            + QStringLiteral(" && echo LOCKED_OK"),
                        &out, &err),
             qPrintable(QStringLiteral("locking the fixture file timed out: %1").arg(err)));
    QVERIFY2(out.contains("LOCKED_OK"),
             qPrintable(QStringLiteral("could not create an unwritable file: out=%1 err=%2")
                            .arg(QString::fromUtf8(out), err)));

    // Open it in the SAME controller the page is bridged to, so every signal
    // below crosses the real WebChannel into the real bundle.
    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    m_controller->open(lockedPath);
    QVERIFY2(waitFor([&] { return contentSpy.count() >= 1; }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("the locked file never loaded; fileState=%1")
                            .arg(m_controller->fileState())));
    QCOMPARE(contentSpy.at(0).at(0).toString(), lockedContent);

    // C++ derived it from the server's own file.stat — nothing told it.
    QVERIFY2(waitFor([&] { return m_controller->readOnly(); }, kSignalTimeoutMs),
             "an unwritable remote file was not derived as read-only");

    // ...and it REACHED the page: Monaco's own option, not a mirror of ours.
    QString ro;
    QVERIFY2(waitFor([&] {
                 ro = runJs(QString::fromLatin1(kJsReadReadOnly));
                 return ro == QLatin1String("RO:true");
             }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("monaco is still editable: %1").arg(ro)));

    // The option is not decoration: a type command through Monaco's own handler
    // — the path a keystroke takes — leaves the model untouched.
    QVERIFY2(waitFor([&] { return monacoValue() == lockedContent; }, kSignalTimeoutMs),
             "monaco never adopted the locked file's bytes");
    typeInPage(QStringLiteral("this must not appear\n"));
    QCOMPARE(monacoValue(), lockedContent);

    // Ctrl+S is a no-op the page swallows: mountEditor()'s binding checks
    // readOnly before it ever calls bridge.save(). Dispatched at every input
    // surface, then given a window in which ANY save outcome would show up.
    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    QSignalSpy errorSpy(m_controller, &EditorController::saveError);
    for (const QString &target :
         QStringList{QStringLiteral(R"(document.querySelector(".monaco-editor textarea.inputarea"))"),
                     QStringLiteral(R"(document.querySelector(".monaco-editor .native-edit-context"))"),
                     QStringLiteral(R"(window.monaco.editor.getEditors()[0].getContainerDomNode())")}) {
        runJs(jsCtrlS(target));
    }
    QTest::qWait(3000);
    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(conflictSpy.count(), 0);

    // The other half of the guard, and the one that matters for the page's own
    // conflict/error notices: they call bridge.save() DIRECTLY, past the key
    // binding's check. The controller refuses with a reason instead of issuing a
    // write that dies as EACCES.
    m_controller->save(QStringLiteral("this must not reach the disk\n"),
                       m_controller->revision());
    QVERIFY2(waitFor([&] { return errorSpy.count() == 1; }, kSignalTimeoutMs),
             "a read-only save produced no explanation");
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(QStringLiteral("read-only"),
                                                      Qt::CaseInsensitive),
             qPrintable(errorSpy.at(0).at(0).toString()));
    QCOMPARE(savedSpy.count(), 0);

    // Nothing was written, and nothing was left behind: no crash-recovery
    // snapshot can accumulate for a buffer that could never be saved (SPEC 11.3).
    m_controller->reportContent(QStringLiteral("not a snapshot\n"));
    QTest::qWait(1000);
    QVERIFY(remoteExec(QStringLiteral("cat -- ") + sq(lockedPath), &out, &err));
    QCOMPARE(QString::fromUtf8(out), lockedContent);
    QVERIFY(remoteExec(QStringLiteral("test -s ") + sq(recoveryPath)
                           + QStringLiteral(" && echo SNAPSHOT || echo NO_SNAPSHOT"),
                       &out, &err));
    QVERIFY2(out.contains("NO_SNAPSHOT"),
             "a read-only buffer left a recovery snapshot that could never be applied");

    qInfo("chmod 444 %s -> monaco readOnly=%s, save refused, disk untouched",
          qPrintable(lockedPath), qPrintable(ro));

    // Restore the mode: read-only is DERIVED, not latched, so a reload must give
    // the file back. A verdict that never lifts is its own bug.
    QVERIFY(remoteExec(QStringLiteral("chmod 644 ") + sq(lockedPath)
                           + QStringLiteral(" && echo UNLOCK_OK"),
                       &out, &err));
    QVERIFY2(out.contains("UNLOCK_OK"), qPrintable(QString::fromUtf8(out) + err));

    m_controller->requestReload();
    QVERIFY2(waitFor([&] { return !m_controller->readOnly(); }, kSignalTimeoutMs),
             "a re-writable file stayed read-only: the verdict was latched");
    QVERIFY2(waitFor([&] {
                 ro = runJs(QString::fromLatin1(kJsReadReadOnly));
                 return ro == QLatin1String("RO:false");
             }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("monaco stayed locked: %1").arg(ro)));

    // And the buffer is genuinely usable again, all the way to the disk.
    typeInPage(QStringLiteral("editable again\n"));
    const QString expected = monacoValue();
    QVERIFY(expected.contains(QStringLiteral("editable again")));
    m_controller->save(expected, m_controller->revision());
    QVERIFY2(waitFor([&] { return savedSpy.count() == 1; }, kSignalTimeoutMs),
             qPrintable(QStringLiteral("the unlocked file would not save. errors=%1 state=%2")
                            .arg(errorSpy.count())
                            .arg(m_controller->fileState())));
    QVERIFY(remoteExec(QStringLiteral("cat -- ") + sq(lockedPath), &out, &err));
    QCOMPARE(QString::fromUtf8(out), expected);

    qInfo("chmod 644 + reload -> monaco readOnly=false, save landed on disk");
}

// QTEST_MAIN cannot be used: the internal URL scheme must be registered and
// WebEngine initialised BEFORE the QGuiApplication exists, exactly as main.cpp
// does it.
int main(int argc, char *argv[])
{
    QStandardPaths::setTestModeEnabled(true);

    ViewerProfiles::registerUrlScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    TstLiveEditor testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_liveeditor.moc"
