// Gate for the packaged xterm.js page (SPEC 5.1) inside the REAL terminal pane.
//
// Everything under test here is production: src/qml/TerminalPaneView.qml loaded
// from the CodeHarbor module's qrc, the ch::TerminalFactory that mints its
// controller and bridge, a REAL QWebChannel, and the REAL bundle embedded at
// qrc:/codeharbor/web/terminal/index.html. No SSH is involved — the remote half
// is the live gate's job (tst_liveterminalfactory); what this proves is the
// half that was missing entirely: that a pane actually renders a terminal and
// that bytes cross the bridge in BOTH directions.
//
// Three claims, each one a regression that would otherwise ship silently:
//   1. the renderer's initial size reaches C++ (a pane whose PTY is stuck at
//      the channel default is unusable, and the handshake is easy to lose),
//   2. C++ -> JS works: a lifecycle state lands in the page's status strip and
//      output lands in xterm's screen,
//   3. JS -> C++ works: a keystroke in the terminal reaches the controller's
//      transport.
//
// Labelled `live` and QSKIPs (rather than fails) when the box cannot host
// WebEngine at all — the same rule as tst_webengine_headless, since that is a
// property of the machine, not of this code.

#include "CodeharbordClient.h"
#include "KnownHosts.h"
#include "SessionState.h"
#include "SshConnectionPool.h"
#include "TerminalController.h"
#include "TerminalFactory.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QtTest/QtTest>

#include <QBuffer>
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
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QtQuickControls2/QQuickStyle>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include <memory>

using ch::SshConnectionPool;
using ch::TerminalController;
using ch::TerminalFactory;
using ch::TerminalState;

namespace {

// Chromium's first start (zygote + renderer) is slow on a cold cache.
constexpr int kPageLoadTimeoutMs = 60000;
constexpr int kProbeTimeoutMs = 20000;

// Thin host: it owns nothing but a Loader, because the pane under test is the
// REAL src/qml/TerminalPaneView.qml from the CodeHarbor module's qrc — the same
// URL the shipped binary resolves. `terminalFactory` and `viewers` arrive as
// context properties exactly as main.cpp supplies them, so the WebChannel
// registration, the privileged profile and the bundle URL are production code.
constexpr auto kShellQml = R"QML(
import QtQuick
import QtQuick.Window
import QtWebEngine

Window {
    id: win
    width: 900
    height: 500
    visible: true
    color: "#11111b"

    property string jsResult: ""
    property bool jsFinished: false

    Loader {
        id: paneLoader
        objectName: "paneLoader"
        anchors.fill: parent
    }

    function openPane(devSessionId, terminalId, workingDir) {
        paneLoader.setSource("qrc:/qt/qml/CodeHarbor/TerminalPaneView.qml",
                             { paneId: terminalId, devSessionId: devSessionId,
                               terminalId: terminalId, workingDir: workingDir })
        return paneLoader.status === Loader.Ready
            ? "ready" : ("loader-status=" + paneLoader.status)
    }

    function paneItem() { return paneLoader.item }
    function paneController() { return paneLoader.item ? paneLoader.item.controller : null }
    function paneLoaded() { return paneLoader.item ? paneLoader.item.pageLoaded : false }
    function paneStatusText() { return paneLoader.item ? paneLoader.item.statusText : "" }
    function paneAttach() {
        if (!paneLoader.item)
            return false
        paneLoader.item.attachNow()
        return paneLoader.item.attached
    }
    function paneKill() { if (paneLoader.item) paneLoader.item.killSession() }

    // The pane's WebEngineView, found structurally: TerminalPaneView keeps it
    // inside a Loader with no objectName, and a test has no business editing it.
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

    function evalJs(script) {
        win.jsFinished = false
        win.jsResult = ""
        var view = findView(paneLoader.item)
        if (!view) {
            win.jsResult = "NO_VIEW"
            win.jsFinished = true
            return
        }
        view.runJavaScript(script, function(result) {
            win.jsResult = (result === undefined || result === null) ? "" : String(result)
            win.jsFinished = true
        })
    }
}
)QML";

// The page's own status strip: written synchronously by TerminalHost
// .setConnectionState(), so it is readable without waiting on a repaint.
constexpr auto kJsStatus = R"JS(
(function () {
    try {
        var el = document.querySelector(".ch-terminal-status");
        if (!el) return "NO_STATUS";
        return el.dataset.state + "|" + el.textContent;
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// xterm's rendered screen. The DOM renderer is the default in xterm 5, so the
// glyphs are real elements rather than canvas pixels.
constexpr auto kJsScreenText = R"JS(
(function () {
    try {
        var rows = document.querySelector(".xterm-rows");
        if (!rows) return "NO_ROWS";
        return rows.textContent.replace(/\u00a0/g, " ");
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// A real keydown on xterm's input target. Enter is handled entirely inside
// xterm's keydown path (no composition, no keypress), so a synthetic event is
// enough to drive the frozen bridge.sendInput() contract.
constexpr auto kJsPressEnter = R"JS(
(function () {
    try {
        var textarea = document.querySelector(".xterm-helper-textarea");
        if (!textarea) return "NO_TEXTAREA";
        textarea.focus();
        var event = new KeyboardEvent("keydown", {
            key: "Enter", code: "Enter", keyCode: 13, which: 13,
            bubbles: true, cancelable: true
        });
        textarea.dispatchEvent(event);
        return "SENT";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// A real paste into xterm's input target: xterm reads the ClipboardEvent's
// DataTransfer and pushes the text through the same data path a typed line
// takes. This is how the live gate below "types" a whole command from the PAGE
// rather than from C++.
constexpr auto kJsPasteTemplate = R"JS(
(function () {
    try {
        var textarea = document.querySelector(".xterm-helper-textarea");
        if (!textarea) return "NO_TEXTAREA";
        textarea.focus();
        var data = new DataTransfer();
        data.setData("text/plain", %1);
        var event = new ClipboardEvent("paste", {
            clipboardData: data, bubbles: true, cancelable: true
        });
        textarea.dispatchEvent(event);
        return "PASTED";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

} // namespace

class TstTerminalPage : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void rendererReportsItsInitialSizeToTheController();
    void controllerStateAndOutputReachTheRenderer();
    void keystrokesReachTheControllerTransport();
    // Declared last: it takes the pane live and leaves it attached to a real
    // remote shell.
    void livePaneRendersARealRemoteShell();

private:
    QString evalJs(const QString& script, int timeoutMs = kProbeTimeoutMs);
    bool waitForJs(const QString& script, const QString& needle, int timeoutMs);
    // Paste `line` into the terminal from the page, repeating until `needle`
    // shows up on xterm's screen. The retype matters: the first keystrokes can
    // land before the shell inside a freshly created tmux session has readline.
    bool pasteUntilScreenContains(const QString& line, const QString& needle, int timeoutMs);
    void connectLivePool();

    ch::CodeharbordClient m_client;
    ch::ViewerProfiles m_profiles{&m_client};
    ch::ViewerModel m_viewers{&m_client};
    // Disconnected for the renderer tests, so attach() refuses and the pane
    // shows its "not connected" chrome — exactly what a user sees before a
    // server is configured. The live test connects it for real.
    ch::SshConnectionPool m_pool;
    TerminalFactory m_factory{&m_pool};

    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QObject> m_window;
    TerminalController* m_controller = nullptr;
    QString m_liveTarget;
};

void TstTerminalPage::initTestCase()
{
    m_viewers.setProfiles(&m_profiles);

    m_engine = std::make_unique<QQmlEngine>();
    m_engine->rootContext()->setContextProperty(QStringLiteral("viewers"), &m_viewers);
    m_engine->rootContext()->setContextProperty(QStringLiteral("terminalFactory"), &m_factory);

    QQmlComponent component(m_engine.get());
    component.setData(QByteArray(kShellQml), QUrl(QStringLiteral("qrc:/tst_terminalpage/shell.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    m_window.reset(component.create());
    QVERIFY2(m_window != nullptr, qPrintable(component.errorString()));

    QVariant opened;
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "openPane", Q_RETURN_ARG(QVariant, opened),
                                      Q_ARG(QVariant, QStringLiteral("dev-page")),
                                      Q_ARG(QVariant, QStringLiteral("term-page")),
                                      Q_ARG(QVariant, QStringLiteral("/tmp"))));
    QCOMPARE(opened.toString(), QStringLiteral("ready"));

    QVariant controller;
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "paneController",
                                      Q_RETURN_ARG(QVariant, controller)));
    m_controller = controller.value<TerminalController*>();
    QVERIFY2(m_controller != nullptr, "the pane did not mint a controller through terminalFactory");

    // Wait for Chromium to load the packaged page. A box that cannot host
    // WebEngine skips rather than fails; that is a machine property.
    QElapsedTimer clock;
    clock.start();
    QVariant loaded;
    while (clock.elapsed() < kPageLoadTimeoutMs) {
        QVERIFY(QMetaObject::invokeMethod(m_window.get(), "paneLoaded",
                                          Q_RETURN_ARG(QVariant, loaded)));
        if (loaded.toBool())
            break;
        QTest::qWait(100);
    }
    if (!loaded.toBool()) {
        QVariant status;
        QMetaObject::invokeMethod(m_window.get(), "paneStatusText", Q_RETURN_ARG(QVariant, status));
        QSKIP(qPrintable(QStringLiteral("the terminal page never loaded in %1 ms (%2); "
                                        "WebEngine cannot run under this recipe")
                             .arg(kPageLoadTimeoutMs)
                             .arg(status.toString())));
    }
    qInfo("terminal page loaded in %lld ms", clock.elapsed());
}

void TstTerminalPage::cleanupTestCase()
{
    m_window.reset();
    m_engine.reset();
}

QString TstTerminalPage::evalJs(const QString& script, int timeoutMs)
{
    if (!QMetaObject::invokeMethod(m_window.get(), "evalJs", Q_ARG(QVariant, script)))
        return QStringLiteral("INVOKE_FAILED");
    QElapsedTimer clock;
    clock.start();
    while (!m_window->property("jsFinished").toBool() && clock.elapsed() < timeoutMs)
        QTest::qWait(50);
    return m_window->property("jsFinished").toBool() ? m_window->property("jsResult").toString()
                                                     : QStringLiteral("JS_TIMEOUT");
}

bool TstTerminalPage::waitForJs(const QString& script, const QString& needle, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    QString last;
    while (clock.elapsed() < timeoutMs) {
        last = evalJs(script, timeoutMs);
        if (last.contains(needle))
            return true;
        QTest::qWait(100);
    }
    qWarning().noquote() << "probe never matched" << needle << "last:" << last.left(400);
    return false;
}

// (1) The size handshake. xterm measures its own cells, so the renderer is the
// only thing that knows the pane's real cols x rows; if that never crosses the
// bridge the remote PTY is stuck at whatever the channel defaulted to.
void TstTerminalPage::rendererReportsItsInitialSizeToTheController()
{
    QTRY_VERIFY_WITH_TIMEOUT(m_controller->columns() > 0 && m_controller->rows() > 0,
                             kProbeTimeoutMs);
    qInfo("renderer reported %d x %d", m_controller->columns(), m_controller->rows());

    // A pane this size fits well more than the 1x1 a broken fit() would report.
    QVERIFY(m_controller->columns() >= 20);
    QVERIFY(m_controller->rows() >= 5);
}

// (2) C++ -> JS across the real channel: the lifecycle string lands in the
// page's status strip, and output lands in xterm's screen.
void TstTerminalPage::controllerStateAndOutputReachTheRenderer()
{
    m_controller->setState(TerminalState::Ready);
    QVERIFY2(waitForJs(QString::fromLatin1(kJsStatus), ch::toString(TerminalState::Ready),
                       kProbeTimeoutMs),
             "the page never showed the Ready state");

    m_controller->ingestOutput(QByteArrayLiteral("CH_PAGE_MARKER_alpha\r\n"));
    QVERIFY2(waitForJs(QString::fromLatin1(kJsScreenText), QStringLiteral("CH_PAGE_MARKER_alpha"),
                       kProbeTimeoutMs),
             "controller output never reached the xterm.js screen");
    qInfo("output crossed the bridge into the renderer");

    // A drop must be visible rather than silent.
    m_controller->setState(TerminalState::Disconnected);
    QVERIFY2(waitForJs(QString::fromLatin1(kJsStatus), ch::toString(TerminalState::Disconnected),
                       kProbeTimeoutMs),
             "the page never showed the Disconnected state");
    m_controller->setState(TerminalState::Ready);
}

// (3) JS -> C++: a keystroke in the terminal must reach the transport the
// controller writes to — the frozen bridge.sendInput() contract, end to end.
void TstTerminalPage::keystrokesReachTheControllerTransport()
{
    // WriteOnly, not ReadWrite: a QBuffer hands back everything written into it
    // as READABLE data, so a ReadWrite buffer would feed the keystroke straight
    // back to the controller as terminal OUTPUT and echo it onto the real xterm
    // screen the next test inspects. The controller refuses to read a transport
    // that is not readable, so write-only keeps the direction one-way.
    QBuffer transport;
    QVERIFY(transport.open(QIODevice::WriteOnly));
    m_controller->setTransport(&transport);

    QCOMPARE(evalJs(QString::fromLatin1(kJsPressEnter)), QStringLiteral("SENT"));
    QTRY_VERIFY_WITH_TIMEOUT(transport.data().contains('\r'), kProbeTimeoutMs);
    qInfo() << "keystroke reached the transport as" << transport.data().toHex();

    m_controller->setTransport(nullptr);
}

bool TstTerminalPage::pasteUntilScreenContains(const QString& line, const QString& needle,
                                               int timeoutMs)
{
    // JSON-encode the payload so quotes/newlines in the command cannot break
    // the injected script.
    const QString script =
        QString::fromLatin1(kJsPasteTemplate)
            .arg(QString::fromUtf8(
                QJsonDocument(QJsonArray{line}).toJson(QJsonDocument::Compact))
                     .mid(1)
                     .chopped(1));

    QElapsedTimer clock;
    clock.start();
    qint64 nextPaste = 0;
    QString screen;
    while (clock.elapsed() < timeoutMs) {
        if (clock.elapsed() >= nextPaste) {
            const QString pasted = evalJs(script);
            if (pasted != QStringLiteral("PASTED")) {
                qWarning().noquote() << "paste probe failed:" << pasted;
                return false;
            }
            // The line is only inserted, never submitted: readline's bracketed
            // paste (on by default in bash 5) deliberately treats a pasted
            // newline as literal text. Enter is a real keystroke, so it is sent
            // as one — the same keydown path a user's Return takes.
            const QString submitted = evalJs(QString::fromLatin1(kJsPressEnter));
            if (submitted != QStringLiteral("SENT")) {
                qWarning().noquote() << "enter probe failed:" << submitted;
                return false;
            }
            nextPaste = clock.elapsed() + 5000;
        }
        QTest::qWait(250);
        screen = evalJs(QString::fromLatin1(kJsScreenText));
        if (screen.contains(needle))
            return true;
    }
    qWarning().noquote() << "screen never showed" << needle << "last:" << screen.right(400);
    return false;
}

void TstTerminalPage::connectLivePool()
{
    if (m_pool.state() == SshConnectionPool::State::Connected)
        return;

    QString knownHostsPath = qEnvironmentVariable("CH_LIVE_KNOWN_HOSTS");
    if (knownHostsPath.isEmpty())
        knownHostsPath = QDir::temp().filePath(QStringLiteral("ch_live_terminal_page_known_hosts"));

    // First-use trust, exactly like SessionBootstrap: load the store, accept an
    // unknown key once, write it back. A Mismatch never reaches the callback.
    ch::KnownHosts hosts;
    QFile store(knownHostsPath);
    if (store.open(QIODevice::ReadOnly | QIODevice::Text))
        hosts = ch::KnownHosts::parse(QString::fromUtf8(store.readAll()));
    store.close();
    m_pool.setKnownHosts(hosts);
    m_pool.setHostKeyCallback([](const QString&, const QString&, const QByteArray&,
                                 ch::KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    QString failure;
    const auto conn = connect(&m_pool, &SshConnectionPool::errorOccurred,
                              [&failure](const QString& text) { failure += text; });
    const bool ok = m_pool.connectToHost(qEnvironmentVariable("CH_LIVE_HOST"),
                                         static_cast<quint16>(
                                             qEnvironmentVariable("CH_LIVE_PORT").toUInt()),
                                         qEnvironmentVariable("CH_LIVE_USER"));
    disconnect(conn);
    QVERIFY2(ok, qPrintable(QStringLiteral("connectToHost failed: %1").arg(failure)));

    QDir().mkpath(QFileInfo(knownHostsPath).absolutePath());
    QFile out(knownHostsPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(m_pool.knownHosts().serialize());
}

// (4) The whole slice in one go: a REAL remote shell rendered by the REAL pane.
// The pane attaches through its own attachNow() (the button a user presses),
// the PTY runs a real tmux session on the fixture server, and the command is
// "typed" into xterm from the PAGE — so the marker on screen can only have come
// from the remote shell, through libssh, the controller, the bridge and
// xterm.js.
void TstTerminalPage::livePaneRendersARealRemoteShell()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live pane gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live pane gate skipped");

    connectLivePool();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    // Unique per run so repeat runs never inherit a stale tmux session.
    const QString terminalId = QStringLiteral("p%1x%2")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QDateTime::currentMSecsSinceEpoch() % 1000000);
    const QString devSessionId = QStringLiteral("livepage");
    QVariant setSession;
    QVERIFY(QMetaObject::invokeMethod(
        m_window.get(), "openPane", Q_RETURN_ARG(QVariant, setSession),
        Q_ARG(QVariant, devSessionId), Q_ARG(QVariant, terminalId),
        Q_ARG(QVariant, qEnvironmentVariable("CH_LIVE_REPO"))));
    QCOMPARE(setSession.toString(), QStringLiteral("ready"));

    // A fresh pane means a fresh controller and a fresh page.
    QVariant controller;
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "paneController",
                                      Q_RETURN_ARG(QVariant, controller)));
    m_controller = controller.value<TerminalController*>();
    QVERIFY(m_controller != nullptr);
    m_liveTarget = m_factory.targetFor(m_controller);
    QVERIFY2(!m_liveTarget.isEmpty(),
             "the pane did not attach through terminalFactory on completion");

    QVariant loaded;
    QTRY_VERIFY_WITH_TIMEOUT(
        QMetaObject::invokeMethod(m_window.get(), "paneLoaded", Q_RETURN_ARG(QVariant, loaded))
            && loaded.toBool(),
        kPageLoadTimeoutMs);

    // tmux drawing itself into the pane is the shell coming up, and the page is
    // where we look for it — not the controller.
    QTRY_VERIFY_WITH_TIMEOUT(m_controller->state() == TerminalState::Ready, kPageLoadTimeoutMs);
    QVERIFY2(waitForJs(QString::fromLatin1(kJsStatus), ch::toString(TerminalState::Ready),
                       kProbeTimeoutMs),
             "the live pane never reported Ready to the page");

    const QString marker = QStringLiteral("CH_PANE_MARKER_") + terminalId;
    // printf, not echo: the pasted line carries "%s_%s", the OUTPUT carries the
    // joined marker, so xterm echoing our own keystrokes cannot satisfy this.
    const QString command = QStringLiteral("printf '%s_%s\\n' CH_PANE MARKER_") + terminalId;
    QVERIFY2(pasteUntilScreenContains(command, marker, 60000),
             qPrintable(QStringLiteral("the remote marker %1 never appeared on the pane's screen")
                            .arg(marker)));
    qInfo().noquote() << "live pane rendered remote output:" << marker;

    // The renderer's real geometry reached the remote PTY, not the 80x24
    // fallback: ask tmux itself what size its client has.
    const QString sizeProbe =
        QStringLiteral("tmux display-message -p 'CHSIZE=#{client_width}x#{client_height}'");
    const QString expected = QStringLiteral("CHSIZE=%1x%2")
                                 .arg(m_controller->columns())
                                 .arg(m_controller->rows());
    QVERIFY2(pasteUntilScreenContains(sizeProbe, expected, 60000),
             qPrintable(QStringLiteral("the remote PTY never reported %1").arg(expected)));
    qInfo().noquote() << "remote tmux client size matches the renderer:" << expected;

    // Leave nothing running on the fixture: the pane's own kill path.
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "paneKill"));
    QTest::qWait(1000);
    m_liveTarget.clear();
}

int main(int argc, char* argv[])
{
    QStandardPaths::setTestModeEnabled(true);

    // Must precede QtWebEngineQuick::initialize(): the privileged profile the
    // pane binds carries the internal scheme (SPEC 7.2).
    ch::ViewerProfiles::registerUrlScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    TstTerminalPage testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_terminalpage.moc"
