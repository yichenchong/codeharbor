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

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "Ids.h"
#include "SessionBootstrap.h"
#include "SessionState.h"
#include "SshConnectionPool.h"
#include "TerminalController.h"
#include "TerminalFactory.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"
#include "WorkspaceDb.h"
#include "WorkspaceTypes.h"

#include <QtTest/QtTest>

#include <cmath>

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
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
#include <optional>

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

    // `terminalPaneId` is the pane's identity: the id of its row in the
    // server's `terminal_panes` table. A pane without one has nothing safe to
    // attach to and deliberately waits, so the live case mints a real row and
    // passes it here — which is exactly the shape production builds a pane in.
    function openPane(devSessionId, terminalId, workingDir, terminalPaneId) {
        paneLoader.setSource("qrc:/qt/qml/CodeHarbor/TerminalPaneView.qml",
                             { paneId: terminalId, devSessionId: devSessionId,
                               terminalId: terminalId, workingDir: workingDir,
                               terminalPaneId: terminalPaneId ? terminalPaneId : "" })
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

// xterm's rendered screen.
//
// Read out of the PAGE's own diagnostics hook, which reports xterm's buffer,
// not out of the DOM. The .xterm-rows element only carries glyphs while the DOM
// renderer is active, and the page loads the WebGL addon whenever the host can
// run it (src/web/terminal/src/index.ts) — which disposes the DOM renderer and
// removes that element. Scraping the DOM therefore reported an empty screen on
// exactly the hosts where rendering works best. The DOM scrape stays as the
// fallback for a page too old to expose the hook.
constexpr auto kJsScreenText = R"JS(
(function () {
    try {
        if (typeof window.codeharborTerminalDiagnostics === "function") {
            var text = window.codeharborTerminalDiagnostics().screenText;
            if (typeof text === "string" && text.length > 0)
                return text;
        }
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

// Apply the two renderer preferences the way TerminalPaneView.qml applies them,
// then report the apparent cell size the page ended up with. The reply is
// "cellWidth|cellHeight|fontCssPixels|cols|rows", or a diagnostic string.
constexpr auto kJsMeasureTemplate = R"JS(
(function () {
    try {
        if (typeof window.codeharborSetTerminalPreferences !== "function")
            return "NO_PREFERENCES_HOOK";
        window.codeharborSetTerminalPreferences(%1, %2);
        var d = window.codeharborTerminalDiagnostics();
        if (!d.cellCssPixels) return "NO_CELL";
        return d.cellCssPixels.width.toFixed(3) + "|" + d.cellCssPixels.height.toFixed(3)
             + "|" + d.fontCssPixels + "|" + d.cols + "|" + d.rows;
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
    // The two renderer preferences must not be multiplied together: raising the
    // pixel ratio buys sharpness, and only the font size decides how big the
    // text looks.
    void apparentTextSizeIgnoresThePixelRatio();
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
                                      Q_ARG(QVariant, QStringLiteral("/tmp")),
                                      // No row id: these three cases are about the
                                      // renderer and the bridge, and the pool is
                                      // deliberately disconnected, so the pane never
                                      // gets as far as needing an identity.
                                      Q_ARG(QVariant, QString())));
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

// (4) The two renderer preferences are independent. The pixel ratio decides how
// many physical pixels back one logical pixel — it buys sharpness — while the
// font size alone decides how big the text looks. Multiplying them, as the page
// once effectively did, means a user who wants nine-point text at one-and-a-half
// times the resolution has to ask for six points and guess at the arithmetic.
void TstTerminalPage::apparentTextSizeIgnoresThePixelRatio()
{
    struct Measurement {
        double cellWidth = 0.0;
        double cellHeight = 0.0;
        double fontCssPixels = 0.0;
        int cols = 0;
        int rows = 0;
    };
    const auto measure = [this](int points, double ratio) {
        const QString reply = evalJs(QString::fromLatin1(kJsMeasureTemplate)
                                         .arg(points)
                                         .arg(ratio));
        const QStringList parts = reply.split(QLatin1Char('|'));
        Measurement measurement;
        if (parts.size() != 5) {
            qWarning().noquote() << "measurement probe replied" << reply;
            return measurement;
        }
        measurement.cellWidth = parts.at(0).toDouble();
        measurement.cellHeight = parts.at(1).toDouble();
        measurement.fontCssPixels = parts.at(2).toDouble();
        measurement.cols = parts.at(3).toInt();
        measurement.rows = parts.at(4).toInt();
        return measurement;
    };

    const Measurement plain = measure(9, 1.0);
    QVERIFY2(plain.cellWidth > 0.0 && plain.cellHeight > 0.0,
             "the page reported no cell size at all");

    // The user's exact complaint: nine points at one-and-a-half times the
    // resolution must look like nine points, not like thirteen and a half.
    for (const double ratio : {1.5, 2.0, 4.0}) {
        const Measurement scaled = measure(9, ratio);
        // A tolerance of half a CSS pixel, because the renderer rounds its cell
        // to whole device pixels and a fractional ratio can move that rounding
        // by a fraction of a logical pixel. A ratio being multiplied into the
        // font size would move it by tens of per cent, never by this much.
        QVERIFY2(std::abs(scaled.cellWidth - plain.cellWidth) <= 0.5
                     && std::abs(scaled.cellHeight - plain.cellHeight) <= 0.5,
                 qPrintable(QStringLiteral("pixel ratio %1 changed the apparent text size: "
                                           "%2x%3 CSS pixels per cell against %4x%5 at ratio 1")
                                .arg(ratio)
                                .arg(scaled.cellWidth)
                                .arg(scaled.cellHeight)
                                .arg(plain.cellWidth)
                                .arg(plain.cellHeight)));
        QCOMPARE(scaled.fontCssPixels, plain.fontCssPixels);
    }

    // ...and the font size still does what it says, at a raised ratio.
    // Not `small`/`large`: Windows' rpcndr.h defines `small` as a macro for
    // `char`, so a variable of that name stops the file compiling on MSVC.
    const Measurement smaller = measure(6, 1.5);
    const Measurement larger = measure(12, 1.5);
    QVERIFY2(smaller.cellHeight < plain.cellHeight && larger.cellHeight > plain.cellHeight,
             qPrintable(QStringLiteral("the font size stopped changing the text size at ratio "
                                       "1.5: 6pt gave %1, 9pt gave %2, 12pt gave %3")
                            .arg(smaller.cellHeight)
                            .arg(plain.cellHeight)
                            .arg(larger.cellHeight)));
    // A smaller cell must also mean more of them, or the grid the remote shell
    // is told about has stopped following what is on screen.
    QVERIFY2(smaller.rows > larger.rows && smaller.cols > larger.cols,
             "the grid did not follow the cell size");

    measure(13, 0.0);
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

// (4) The whole slice in one go: a REAL remote shell rendered by the REAL pane.
// The pane attaches through its own attachNow() (the button a user presses),
// the PTY runs a real tmux session on the fixture server, and the command is
// "typed" into xterm from the PAGE — so the marker on screen can only have come
// from the remote shell, through libssh, the controller, the bridge and
// xterm.js.
//
// The pane is built the way production builds one: its layout leaf carries a
// `terminal_panes` row id (SPEC 5.2), minted here against the real codeharbord,
// and the pane resolves its tmux target FROM that row. A pane with no row id
// has no identity and deliberately attaches nothing — so the factory is given a
// real workspace and server id, exactly as main.cpp gives it one.
void TstTerminalPage::livePaneRendersARealRemoteShell()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live pane gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live pane gate skipped");

    // Connects the SSH session AND wires codeharbord over it, because this case
    // needs both: the PTY channel for the shell, and the workspace RPC for the
    // pane's identity.
    ch::AgentStatusMonitor monitor;
    ch::SessionBootstrap boot(&m_pool, &m_client, &monitor);
    boot.setReconnectEnabled(false); // reconnect would fight the teardown below
    QVERIFY2(boot.connectAndWireFromEnvironment(),
             "connectAndWireFromEnvironment failed");
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    ch::WorkspaceDb db(&m_client);
    const QString serverId =
        QStringLiteral("tst-tp-%1").arg(QCoreApplication::applicationPid());

    std::optional<ch::Group> group;
    std::optional<ch::RpcError> failure;
    db.createGroup(ch::CreateGroupParams{.serverId = ch::ServerId{serverId},
                                         .name = QStringLiteral("tst_terminalpage")},
                   [&](std::optional<ch::Group> g, std::optional<ch::RpcError> e) {
                       group = g;
                       failure = e;
                   });
    QTRY_VERIFY_WITH_TIMEOUT(group.has_value() || failure.has_value(), 15000);
    QVERIFY2(group.has_value(), qPrintable(failure ? failure->message : QString()));

    std::optional<ch::DevSession> session;
    db.createSession(ch::CreateSessionParams{.serverId = ch::ServerId{serverId},
                                             .groupId = group->id,
                                             .name = QStringLiteral("pane probe"),
                                             .repositoryRoot =
                                                 qEnvironmentVariable("CH_LIVE_REPO")},
                     [&](std::optional<ch::DevSession> s, std::optional<ch::RpcError> e) {
                         session = s;
                         failure = e;
                     });
    QTRY_VERIFY_WITH_TIMEOUT(session.has_value() || failure.has_value(), 15000);
    QVERIFY2(session.has_value(), qPrintable(failure ? failure->message : QString()));

    // The pane's identity. Unique per run by construction — a row id is minted
    // fresh and never recycled — so repeat runs cannot inherit a stale tmux
    // session, which is what the hand-rolled unique label used to be for.
    const QString terminalId = QStringLiteral("terminal-1");
    std::optional<ch::TerminalPane> row;
    db.createTerminalPane(
        ch::CreateTerminalPaneParams{.serverId = ch::ServerId{serverId},
                                     .devSessionId = session->id,
                                     .name = terminalId},
        [&](std::optional<ch::TerminalPane> p, std::optional<ch::RpcError> e) {
            row = p;
            failure = e;
        });
    QTRY_VERIFY_WITH_TIMEOUT(row.has_value() || failure.has_value(), 15000);
    QVERIFY2(row.has_value(), qPrintable(failure ? failure->message : QString()));
    QVERIFY(!row->id.value.isEmpty());
    // The server minted the tmux target from the row id; the client never
    // composes one.
    QVERIFY(!row->tmuxTarget.isEmpty());

    m_factory.setWorkspace(&db);
    m_factory.setServerId(serverId);

    QVariant setSession;
    QVERIFY(QMetaObject::invokeMethod(
        m_window.get(), "openPane", Q_RETURN_ARG(QVariant, setSession),
        Q_ARG(QVariant, session->id.value), Q_ARG(QVariant, terminalId),
        Q_ARG(QVariant, qEnvironmentVariable("CH_LIVE_REPO")),
        Q_ARG(QVariant, row->id.value)));
    QCOMPARE(setSession.toString(), QStringLiteral("ready"));

    // A fresh pane means a fresh controller and a fresh page.
    QVariant controller;
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "paneController",
                                      Q_RETURN_ARG(QVariant, controller)));
    m_controller = controller.value<TerminalController*>();
    QVERIFY(m_controller != nullptr);

    // Resolving the row is a round trip, so the attach happens on the answer
    // rather than inside openPane(). What must be true is that the pane ends up
    // attached to the target the SERVER minted for its row — not to anything
    // the client composed, and not to a target derived from the slot label.
    QTRY_VERIFY_WITH_TIMEOUT(!m_factory.targetFor(m_controller).isEmpty(), 20000);
    QCOMPARE(m_factory.targetFor(m_controller), row->tmuxTarget);

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

    const QString marker = QStringLiteral("CH_PANE_MARKER_") + row->id.value.left(8);
    // printf, not echo: the pasted line carries "%s_%s", the OUTPUT carries the
    // joined marker, so xterm echoing our own keystrokes cannot satisfy this.
    const QString command =
        QStringLiteral("printf '%s_%s\\n' CH_PANE MARKER_") + row->id.value.left(8);
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

    // Leave nothing running on the fixture: the pane's own kill path, then the
    // rows it was built from.
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "paneKill"));
    QTest::qWait(1000);

    bool cleaned = false;
    std::optional<ch::RpcError> cleanupError;
    db.deleteGroup(group->id, [&](std::optional<ch::RpcError> error) {
        cleanupError = error;
        cleaned = true;
    });
    QString cleanupFailure;
    if (!QTest::qWaitFor([&] { return cleaned; }, 15000)) {
        cleanupFailure = QStringLiteral("cleanup timed out deleting group %1")
                             .arg(group->id.value);
    } else if (cleanupError) {
        cleanupFailure = QStringLiteral("cleanup failed deleting group %1: %2")
                             .arg(group->id.value, cleanupError->message);
    }
    // `db` and `boot` are locals: the factory must not outlive them holding a
    // dangling repository.
    m_factory.setWorkspace(nullptr);
    boot.disconnectSession();
    QVERIFY2(cleanupFailure.isEmpty(), qPrintable(cleanupFailure));
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
