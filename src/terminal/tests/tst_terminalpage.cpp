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
// Four claims, each one a regression that would otherwise ship silently:
//   1. the renderer's initial size reaches C++ (a pane whose PTY is stuck at
//      the channel default is unusable, and the handshake is easy to lose),
//   2. C++ -> JS works: a lifecycle state lands in the page's status strip and
//      output lands in xterm's screen,
//   3. JS -> C++ works: a keystroke in the terminal reaches the controller's
//      transport,
//   4. the right mouse button belongs to the application: it opens the page's
//      own menu, that menu is unaffected by pointer movement, Escape closes it,
//      and none of it reaches the remote side. The menu it replaces was tmux's,
//      drawn inside the terminal grid, which closed again on every movement.
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
#include <QtQuick/QQuickWindow>
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

// A real right-button press on the terminal, at a point inside the grid.
//
// Dispatched on the innermost element under the pointer — xterm's own screen
// when it exists — rather than on the surface itself, because that is where a
// user's pointer actually lands and it is the only way the swallowing can be
// shown to work: the page's handlers sit on `.ch-terminal-surface` in the
// CAPTURE phase, so an event aimed at a descendant still passes through them
// on its way down, while xterm.js's own handlers further in only see what the
// capture phase let through.
//
// The three events are the ones a browser really produces for a right click,
// in the order it produces them.
constexpr auto kJsRightPress = R"JS(
(function () {
    try {
        var surface = document.querySelector(".ch-terminal-surface");
        if (!surface) return "NO_SURFACE";
        var target = surface.querySelector(".xterm-screen") || surface;
        var rect = target.getBoundingClientRect();
        var x = Math.round(rect.left + rect.width / 3);
        var y = Math.round(rect.top + rect.height / 3);
        function press(type, button, buttons) {
            target.dispatchEvent(new MouseEvent(type, {
                button: button, buttons: buttons, clientX: x, clientY: y,
                bubbles: true, cancelable: true
            }));
        }
        press("mousedown", 2, 2);
        press("contextmenu", 2, 2);
        press("mouseup", 2, 0);
        return "PRESSED";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// Press the left button and then the right one on the same spot, with a spy
// listening on the document in the BUBBLE phase, and report what got through.
//
// This is how "the right button never reaches the remote side" is shown without
// a remote side: the page swallows that button at the surface in the CAPTURE
// phase, so an event aimed at anything inside the terminal is stopped on its
// way DOWN and never comes back up to the document. Everything xterm.js listens
// on is inside the surface, so what the spy cannot see, xterm.js cannot see
// either — and what xterm.js never sees, it cannot report.
//
// The left button is the control, and it is a control that cannot lie: it is
// dispatched on the same element, from the same code, and the only difference
// is which button it claims to be.
//
// The reply is "buttonN=<reached the document>,<was cancelled>" for each button.
constexpr auto kJsComparePresses = R"JS(
(function () {
    try {
        var surface = document.querySelector(".ch-terminal-surface");
        if (!surface) return "NO_SURFACE";
        var target = surface.querySelector(".xterm-screen") || surface;
        var rect = target.getBoundingClientRect();
        var x = Math.round(rect.left + rect.width / 3);
        var y = Math.round(rect.top + rect.height / 3);
        var seen = 0;
        var spy = function () { seen += 1; };
        document.addEventListener("mousedown", spy, false);
        var parts = [];
        try {
            [0, 2].forEach(function (button) {
                seen = 0;
                var down = new MouseEvent("mousedown", {
                    button: button, buttons: button === 2 ? 2 : 1,
                    clientX: x, clientY: y, bubbles: true, cancelable: true
                });
                var uncancelled = target.dispatchEvent(down);
                parts.push("button" + button + "=" + seen + "," + (uncancelled ? 0 : 1));
                target.dispatchEvent(new MouseEvent("mouseup", {
                    button: button, buttons: 0, clientX: x, clientY: y,
                    bubbles: true, cancelable: true
                }));
                if (button === 2) {
                    target.dispatchEvent(new MouseEvent("contextmenu", {
                        button: 2, buttons: 2, clientX: x, clientY: y,
                        bubbles: true, cancelable: true
                    }));
                }
            });
        } finally {
            document.removeEventListener("mousedown", spy, false);
        }
        return parts.join("|");
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// Everything worth knowing about the menu in one delimited reply, read the same
// way the measurement probe above reads the cell size:
// "menu=|open=|items=|copyDisabled=|hint=|selection=".
// `menu` is the DOM element, `open` is the page's own diagnostics; they must
// always agree, and a case that only asked one of them would miss a menu that
// was left on screen after being logically closed.
constexpr auto kJsMenuState = R"JS(
(function () {
    try {
        var menu = document.querySelector("div.ch-terminal-menu");
        var d = window.codeharborTerminalDiagnostics();
        var copy = document.querySelector('.ch-terminal-menu-item[data-action="copy"]');
        return "menu=" + (menu ? 1 : 0)
             + "|open=" + (d.menuOpen ? 1 : 0)
             + "|items=" + (menu ? menu.querySelectorAll("button.ch-terminal-menu-item").length : 0)
             + "|copyDisabled=" + (copy ? (copy.disabled ? 1 : 0) : -1)
             + "|hint=" + (menu && menu.querySelector("div.ch-terminal-menu-hint") ? 1 : 0)
             + "|selection=" + (d.hasSelection ? 1 : 0);
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// A recorder for input the page receives from OUTSIDE itself. Every other probe
// here makes its own events, which proves what the page does with an event but
// says nothing about whether a real one ever arrives: between the user's mouse
// and this document sit the QML pane's full-size MouseArea and the
// WebEngineView. Installed once, read as often as wanted.
constexpr auto kJsInstallInputSpy = R"JS(
(function () {
    try {
        window.__chInput = [];
        if (!window.__chInputSpy) {
            window.__chInputSpy = function (event) {
                window.__chInput.push(event.type + ":" + event.button);
            };
            ["mousedown", "mouseup", "contextmenu"].forEach(function (type) {
                document.addEventListener(type, window.__chInputSpy, true);
            });
        }
        return "SPY";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

constexpr auto kJsReadInputSpy = R"JS(
(function () {
    try {
        return (window.__chInput || []).join(",");
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// An ordinary left press and release inside the page, used to clear a selection
// left behind by an earlier case: with no program holding the mouse, xterm.js
// drops its selection on the next press, which is what a user's next click does.
constexpr auto kJsPlainLeftPress = R"JS(
(function () {
    try {
        var surface = document.querySelector(".ch-terminal-surface");
        if (!surface) return "NO_SURFACE";
        var target = surface.querySelector(".xterm-screen") || surface;
        var rect = target.getBoundingClientRect();
        ["mousedown", "mouseup"].forEach(function (type) {
            target.dispatchEvent(new MouseEvent(type, {
                button: 0, buttons: type === "mousedown" ? 1 : 0, detail: 1,
                clientX: Math.round(rect.left + 4), clientY: Math.round(rect.top + 4),
                bubbles: true, cancelable: true,
            }));
        });
        return "PRESSED";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// A pointer wandering across the terminal with no button held. This is the
// reported bug in one line: tmux's in-grid menu closed on the first of these.
constexpr auto kJsMovePointer = R"JS(
(function () {
    try {
        var surface = document.querySelector(".ch-terminal-surface");
        if (!surface) return "NO_SURFACE";
        var target = surface.querySelector(".xterm-screen") || surface;
        var rect = surface.getBoundingClientRect();
        for (var i = 1; i <= 8; ++i) {
            target.dispatchEvent(new MouseEvent("mousemove", {
                button: 0, buttons: 0,
                clientX: Math.round(rect.left + (rect.width * i) / 10),
                clientY: Math.round(rect.top + (rect.height * i) / 10),
                bubbles: true, cancelable: true
            }));
        }
        return "MOVED";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// Escape, sent to whatever holds the keyboard — the menu focuses its first
// usable item when it opens, so that is the menu itself while it is up.
constexpr auto kJsPressEscape = R"JS(
(function () {
    try {
        var target = document.activeElement || document.querySelector(".ch-terminal-surface");
        if (!target) return "NO_TARGET";
        target.dispatchEvent(new KeyboardEvent("keydown", {
            key: "Escape", code: "Escape", keyCode: 27, which: 27,
            bubbles: true, cancelable: true
        }));
        return "SENT";
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// Choose the menu's Select All item, the way a user chooses it: a click on the
// button. That both closes the menu and gives the terminal a selection, which
// is what the Copy item's enabled state is supposed to follow.
constexpr auto kJsChooseSelectAll = R"JS(
(function () {
    try {
        var item = document.querySelector('.ch-terminal-menu-item[data-action="select-all"]');
        if (!item) return "NO_ITEM";
        item.click();
        return "CHOSEN";
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
    // The right mouse button belongs to the application, not to the remote
    // side: it opens the page's own menu, that menu stays put while the pointer
    // moves, Escape closes it, and nothing about the press ever reaches the
    // terminal's transport.
    void rightClickOpensTheApplicationMenu();
    void theMenuOutlivesPointerMovement();
    void escapeClosesTheMenu();
    void rightClickSendsNothingToTheRemoteSide();
    void copyIsOfferedOnlyWhenThereIsSomethingToCopy();
    // Everything above drives the page with events the page makes for itself.
    // This one uses REAL Qt input, delivered to the window and through the QML
    // pane, because that path has an input-sniffing MouseArea over the whole
    // pane and a WebEngineView with a context menu of its own.
    void realMouseInputTravelsThroughTheQmlPane();
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
    // Open the page's menu with a real right-button press and return the
    // delimited state reply, having first insisted the menu is up.
    QString openTerminalMenu();
    // Escape, then wait for the menu to be gone. Every case that opens the menu
    // ends with this, so the next one starts from a closed menu.
    void closeTerminalMenu();

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

QString TstTerminalPage::openTerminalMenu()
{
    if (evalJs(QString::fromLatin1(kJsRightPress)) != QStringLiteral("PRESSED"))
        return QStringLiteral("PRESS_FAILED");
    // The menu is built synchronously inside the contextmenu handler, but the
    // reply above and the reply below are two separate trips into the renderer,
    // so the state is polled rather than read once.
    QString state;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kProbeTimeoutMs) {
        state = evalJs(QString::fromLatin1(kJsMenuState));
        if (state.startsWith(QStringLiteral("menu=1")))
            return state;
        QTest::qWait(50);
    }
    return state;
}

void TstTerminalPage::closeTerminalMenu()
{
    evalJs(QString::fromLatin1(kJsPressEscape));
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kProbeTimeoutMs) {
        if (evalJs(QString::fromLatin1(kJsMenuState)).startsWith(QStringLiteral("menu=0")))
            return;
        QTest::qWait(50);
    }
}

// (5) The right button opens the APPLICATION's menu.
//
// The pane's tmux session runs with mouse reporting on, so tmux used to get the
// right button and answer it with a menu drawn inside the terminal grid. That
// menu could not be used with a mouse (see the next case), and tmux's binding
// for it is global, so the fix is to keep the button out of the remote side
// altogether and put a real menu of the page's own in its place.
void TstTerminalPage::rightClickOpensTheApplicationMenu()
{
    const QString state = openTerminalMenu();
    QVERIFY2(state.startsWith(QStringLiteral("menu=1")),
             qPrintable(QStringLiteral("a right-button press did not put a "
                                       "div.ch-terminal-menu on the page: %1")
                            .arg(state)));
    // The page's own diagnostics must agree with the DOM: a menu that is on
    // screen while the page believes it is closed answers to nothing.
    QVERIFY2(state.contains(QStringLiteral("|open=1")),
             qPrintable(QStringLiteral("the page does not report its menu as open: %1")
                            .arg(state)));
    // Copy, Paste and Select All, plus the line naming the modifier that draws
    // a local selection — without it the menu is the only discoverable place
    // that behaviour is written down.
    QVERIFY2(state.contains(QStringLiteral("|items=3")),
             qPrintable(QStringLiteral("the menu did not offer three items: %1").arg(state)));
    QVERIFY2(state.contains(QStringLiteral("|hint=1")),
             qPrintable(QStringLiteral("the menu lost its selection hint: %1").arg(state)));

    closeTerminalMenu();
}

// (6) The bug as it was reported: the menu vanished as soon as the mouse
// moved. tmux redraws its in-grid menu away on the next mouse report, and a
// moving pointer produces one per motion. The replacement is ordinary DOM, so
// motion over the terminal must leave it exactly where it is.
void TstTerminalPage::theMenuOutlivesPointerMovement()
{
    QVERIFY2(openTerminalMenu().startsWith(QStringLiteral("menu=1")),
             "the menu did not open, so its survival cannot be judged");

    QCOMPARE(evalJs(QString::fromLatin1(kJsMovePointer)), QStringLiteral("MOVED"));
    QTest::qWait(250);

    const QString state = evalJs(QString::fromLatin1(kJsMenuState));
    QVERIFY2(state.startsWith(QStringLiteral("menu=1")) && state.contains(QStringLiteral("|open=1")),
             qPrintable(QStringLiteral("moving the pointer over the terminal closed the menu: %1")
                            .arg(state)));

    closeTerminalMenu();
}

// (7) Escape closes it, because a menu the keyboard cannot dismiss is a trap.
// Both the element and the page's own view of the menu must go.
void TstTerminalPage::escapeClosesTheMenu()
{
    QVERIFY2(openTerminalMenu().startsWith(QStringLiteral("menu=1")),
             "the menu did not open, so closing it cannot be judged");

    QCOMPARE(evalJs(QString::fromLatin1(kJsPressEscape)), QStringLiteral("SENT"));

    QString state;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kProbeTimeoutMs) {
        state = evalJs(QString::fromLatin1(kJsMenuState));
        if (state.startsWith(QStringLiteral("menu=0")))
            break;
        QTest::qWait(50);
    }
    QVERIFY2(state.startsWith(QStringLiteral("menu=0")),
             qPrintable(QStringLiteral("Escape left the menu element on the page: %1").arg(state)));
    QVERIFY2(state.contains(QStringLiteral("|open=0")),
             qPrintable(QStringLiteral("Escape removed the menu but the page still "
                                       "believes it is open: %1")
                            .arg(state)));
}

// (8) A right click produces no terminal input at all.
//
// Two things have to hold, and one without the other proves little. The page
// must write nothing to the controller's transport, which is read here exactly
// the way the keystroke case above reads it — a QBuffer standing in for the
// remote end. And the press must not get as far as xterm.js in the first place,
// because a terminal only reports mouse events while the program on the far end
// has asked for them, so silence alone could just mean nobody was asking.
//
// The second half is checked in the page: the press is swallowed at the surface
// in the capture phase, so it never reaches anything inside — which is where
// all of xterm.js's listeners are — and never comes back up to the document
// either. A left press dispatched by the same code on the same spot is the
// control, and it must arrive.
//
// Mouse reporting is switched on for the duration with the DECSET sequence a
// remote program (tmux, here) sends when it wants the mouse, so the terminal is
// in the state where a forwarded button WOULD be reported.
void TstTerminalPage::rightClickSendsNothingToTheRemoteSide()
{
    // WriteOnly for the same reason as the keystroke case above: a readable
    // QBuffer would feed everything written into it back to the controller as
    // terminal output.
    QBuffer transport;
    QVERIFY(transport.open(QIODevice::WriteOnly));
    m_controller->setTransport(&transport);

    m_controller->ingestOutput(QByteArrayLiteral("\x1b[?1000h"));
    QTest::qWait(500);

    const QString reply = evalJs(QString::fromLatin1(kJsComparePresses));
    qInfo().noquote() << "presses seen at the document (reached,cancelled):" << reply;
    const QStringList parts = reply.split(QLatin1Char('|'));
    QVERIFY2(parts.size() == 2, qPrintable(QStringLiteral("the press probe replied %1").arg(reply)));
    // The control: an ordinary left press travels the whole way, so the probe
    // itself works and the terminal really is being clicked on.
    QVERIFY2(!parts.at(0).startsWith(QStringLiteral("button0=0,")),
             qPrintable(QStringLiteral("the left button did not reach the terminal either, so "
                                       "this case proves nothing: %1")
                            .arg(reply)));
    // The claim: the right press is stopped and cancelled at the surface, so
    // nothing inside — xterm.js included — ever sees it.
    QCOMPARE(parts.at(1), QStringLiteral("button2=0,1"));

    // ...and, the whole point of stopping it there, nothing was sent to the
    // remote side. Nothing to wait FOR, so the wait is for the absence: long
    // enough that a report crossing the bridge would have arrived.
    QTest::qWait(1000);
    QVERIFY2(transport.data().isEmpty(),
             qPrintable(QStringLiteral("a right click sent %1 to the remote side")
                            .arg(QString::fromLatin1(transport.data().toHex()))));

    m_controller->ingestOutput(QByteArrayLiteral("\x1b[?1000l"));
    m_controller->setTransport(nullptr);
    // The probe's right press opened the menu; leave the page closed.
    closeTerminalMenu();
}

// (9) Copy is offered only when there is something to copy.
//
// A Copy item that is always live is worse than none: it silently does nothing,
// and the user cannot tell whether the selection they made was taken. The state
// is read from the menu the page actually built, and the selection is made the
// way a user makes one here — by choosing Select All from the menu itself.
void TstTerminalPage::copyIsOfferedOnlyWhenThereIsSomethingToCopy()
{
    const QString empty = openTerminalMenu();
    QVERIFY2(empty.contains(QStringLiteral("|selection=0")),
             qPrintable(QStringLiteral("the terminal already held a selection, so a "
                                       "disabled Copy would prove nothing: %1")
                            .arg(empty)));
    QVERIFY2(empty.contains(QStringLiteral("|copyDisabled=1")),
             qPrintable(QStringLiteral("Copy was offered with nothing selected: %1").arg(empty)));

    // Choosing Select All closes the menu and leaves a selection behind.
    QCOMPARE(evalJs(QString::fromLatin1(kJsChooseSelectAll)), QStringLiteral("CHOSEN"));
    QTest::qWait(250);

    const QString selected = openTerminalMenu();
    QVERIFY2(selected.contains(QStringLiteral("|selection=1")),
             qPrintable(QStringLiteral("Select All left the terminal with no selection: %1")
                            .arg(selected)));
    QVERIFY2(selected.contains(QStringLiteral("|copyDisabled=0")),
             qPrintable(QStringLiteral("Copy stayed disabled with the whole screen "
                                       "selected: %1")
                            .arg(selected)));

    closeTerminalMenu();
}

// (10) REAL mouse input, from Qt, through the QML pane, into the page.
//
// Everything above dispatches events inside the document, which cannot see the
// two things between a user's mouse and that document: TerminalPaneView.qml
// puts a full-size MouseArea over the pane to notice which pane is being worked
// in, and the WebEngineView underneath has a context menu of its own (Reload,
// Back, View Source) that Qt shows unless the request is accepted. Either one
// could swallow the button or cover the page's menu, and neither is reachable
// from JavaScript. So this case clicks the WINDOW.
void TstTerminalPage::realMouseInputTravelsThroughTheQmlPane()
{
    auto* window = qobject_cast<QQuickWindow*>(m_window.get());
    QVERIFY2(window != nullptr, "the test shell is not a QQuickWindow");
    if (!QTest::qWaitForWindowExposed(window))
        QSKIP("the window was never exposed, so real input cannot be delivered");

    QCOMPARE(evalJs(QString::fromLatin1(kJsInstallInputSpy)), QStringLiteral("SPY"));

    // Well inside the terminal: below the pane header, away from every border.
    const QPoint point(window->width() / 2, window->height() / 2 + 40);

    // The left button must still arrive. It is the control for the claim below
    // and a claim in its own right: the pane's MouseArea observes presses and
    // then declines them, so a press that stopped there would leave the
    // terminal unclickable.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
    QVERIFY2(waitForJs(QString::fromLatin1(kJsReadInputSpy),
                       QStringLiteral("mousedown:0"), kProbeTimeoutMs),
             qPrintable(QStringLiteral("a real left click never reached the terminal page; "
                                       "the page saw: %1")
                            .arg(evalJs(QString::fromLatin1(kJsReadInputSpy)))));


    // The selection the user asked for, made with real input.
    //
    // Mouse reporting is switched on first, because that is the state that
    // makes this interesting: with a program holding the mouse, a plain drag is
    // its business, and the modifier is the documented way to take one drag
    // back for the page. The screen is filled first so the drag crosses real
    // characters rather than blank cells.
    for (int line = 0; line < 24; ++line) {
        m_controller->ingestOutput(
            QByteArrayLiteral("selectable terminal text on a line of its own\r\n"));
    }
    m_controller->ingestOutput(QByteArrayLiteral("\x1b[?1000h"));
    QTest::qWait(500);
    // An earlier case leaves the whole screen selected; a plain press drops it,
    // so what this case observes can only be what this case selected.
    m_controller->ingestOutput(QByteArrayLiteral("\x1b[?1000l"));
    QTest::qWait(200);
    QCOMPARE(evalJs(QString::fromLatin1(kJsPlainLeftPress)), QStringLiteral("PRESSED"));
    QTest::qWait(200);
    QVERIFY2(evalJs(QString::fromLatin1(kJsMenuState)).contains(QStringLiteral("|selection=0")),
             "the terminal still held a selection, so the drag below would prove nothing");
    m_controller->ingestOutput(QByteArrayLiteral("\x1b[?1000h"));
    QTest::qWait(300);

    // The modifier is not the same everywhere, and that is xterm.js's rule, not
    // this application's: it forces a local selection on Alt (the Option key)
    // on macOS and on Shift on every other platform. A test that hardcoded
    // Shift passed on Linux and failed on macOS for the correct reason — the
    // drag went to the program, exactly as an unmodified drag should.
#ifdef Q_OS_MACOS
    constexpr Qt::KeyboardModifier kSelectionModifier = Qt::AltModifier;
#else
    constexpr Qt::KeyboardModifier kSelectionModifier = Qt::ShiftModifier;
#endif
    const QPoint dragStart(120, window->height() / 2);
    QTest::mousePress(window, Qt::LeftButton, kSelectionModifier, dragStart);
    for (int step = 1; step <= 6; ++step) {
        QTest::mouseMove(window, dragStart + QPoint(step * 60, step * 4));
        QTest::qWait(30);
    }
    const QPoint dragEnd = dragStart + QPoint(360, 24);
    QTest::mouseRelease(window, Qt::LeftButton, kSelectionModifier, dragEnd);

    QString dragged;
    QElapsedTimer dragClock;
    dragClock.start();
    while (dragClock.elapsed() < kProbeTimeoutMs) {
        dragged = evalJs(QString::fromLatin1(kJsMenuState));
        if (dragged.contains(QStringLiteral("|selection=1")))
            break;
        QTest::qWait(50);
    }
    QVERIFY2(dragged.contains(QStringLiteral("|selection=1")),
             qPrintable(QStringLiteral("a real Shift-drag left no selection behind: %1")
                            .arg(dragged)));
    // The complaint that started this: the selection disappeared the moment the
    // button came up. A second later it must still be there.
    QTest::qWait(1200);
    QVERIFY2(evalJs(QString::fromLatin1(kJsMenuState)).contains(QStringLiteral("|selection=1")),
             "the selection was dropped shortly after the button was released");
    m_controller->ingestOutput(QByteArrayLiteral("\x1b[?1000l"));
    // And the right button opens the page's own menu, from a real press.
    QCOMPARE(evalJs(QString::fromLatin1(kJsInstallInputSpy)), QStringLiteral("SPY"));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point);
    QString state;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kProbeTimeoutMs) {
        state = evalJs(QString::fromLatin1(kJsMenuState));
        if (state.startsWith(QStringLiteral("menu=1")))
            break;
        QTest::qWait(50);
    }
    const QString seen = evalJs(QString::fromLatin1(kJsReadInputSpy));
    qInfo().noquote() << "the page saw, from real input:" << seen;
    QVERIFY2(state.startsWith(QStringLiteral("menu=1")) && state.contains(QStringLiteral("|open=1")),
             qPrintable(QStringLiteral("a real right click did not open the page's menu "
                                       "(state %1; the page saw %2)")
                            .arg(state, seen)));

    closeTerminalMenu();
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

// (10) The whole slice in one go: a REAL remote shell rendered by the REAL pane.
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
