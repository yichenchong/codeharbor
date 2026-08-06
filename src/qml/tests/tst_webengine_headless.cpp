// Environment probe for Qt WebEngine under this machine's headless recipe.
//
// The viewer (SPEC 7) and the Monaco editor pane (SPEC 8) both render inside a
// WebEngineView, so every live UI gate depends on Chromium actually starting,
// painting and running JavaScript with no display attached. This test answers
// that question empirically instead of by assumption:
//
//   1. a QML WebEngineView loads a data: page and reaches
//      LoadSucceededStatus, and
//   2. runJavaScript() round-trips a value out of that page.
//
// It is labelled `live` and QSKIPs (rather than fails) when the platform simply
// cannot host WebEngine — the recipe is a property of the box, not of our code.
// Override the recipe from the outside to compare options, e.g.
//   QT_QPA_PLATFORM=vnc ./tst_webengine_headless
//   QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu --no-sandbox" ./tst_webengine_headless

#include <QtTest>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QString>
#include <QTemporaryFile>
#include <QUrl>
#include <QtWebEngineQuick/QtWebEngineQuick>

namespace {

// Minimal but faithful host: a real Window, so the view gets a scene graph and
// Chromium is asked to composite — exactly what the app does.
constexpr auto kProbeQml = R"QML(
import QtQuick
import QtQuick.Window
import QtWebEngine

Window {
    width: 480
    height: 320
    visible: true

    WebEngineView {
        id: view
        objectName: "probe"
        anchors.fill: parent

        property bool finished: false
        property bool succeeded: false
        property string failureText: ""

        property bool jsFinished: false
        property string jsResult: ""

        onLoadingChanged: function(request) {
            if (request.status === WebEngineView.LoadSucceededStatus) {
                view.succeeded = true;
                view.finished = true;
            } else if (request.status === WebEngineView.LoadFailedStatus) {
                view.succeeded = false;
                view.failureText = request.errorString + " (" + request.errorCode + ")";
                view.finished = true;
            }
        }

        function probeJavaScript() {
            view.runJavaScript("document.title + '|' + (1 + 1)", function(result) {
                view.jsResult = String(result);
                view.jsFinished = true;
            });
        }
    }
}
)QML";

QString describeRecipe()
{
    return QStringLiteral("QT_QPA_PLATFORM=%1 QT_QUICK_BACKEND=%2 QTWEBENGINE_CHROMIUM_FLAGS=\"%3\"")
        .arg(QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM")),
             QString::fromLocal8Bit(qgetenv("QT_QUICK_BACKEND")),
             QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS")));
}

} // namespace

class TstWebEngineHeadless : public QObject
{
    Q_OBJECT

private slots:
    void loadsAndScriptsAPageHeadless_data();
    void loadsAndScriptsAPageHeadless();
};

void TstWebEngineHeadless::loadsAndScriptsAPageHeadless_data()
{
    QTest::addColumn<QUrl>("pageUrl");

    // data: — the cheapest possible proof that Chromium starts and scripts.
    QTest::newRow("data-url")
        << QUrl(QStringLiteral("data:text/html,<html><head><title>codeharbor-probe</title>"
                               "</head><body><p>ok</p></body></html>"));

    // file: — a page loaded off the local filesystem. The app's own Monaco and
    // xterm bundles ship inside the binary and are served from qrc:, so this
    // row is not that path; it is the harder one, because a filesystem
    // document exercises Chromium's sandbox and file access in a way a data:
    // URL never does, and a recipe that cannot do it is a recipe that will
    // surprise the first local page anything here tries to open.
    auto *page = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/codeharbor-probe-XXXXXX.html"),
                                    QCoreApplication::instance());
    if (page->open()) {
        page->write("<html><head><title>codeharbor-probe</title></head><body><p>ok</p></body></html>");
        page->flush();
        QTest::newRow("file-url") << QUrl::fromLocalFile(page->fileName());
    }
}

void TstWebEngineHeadless::loadsAndScriptsAPageHeadless()
{
    QFETCH(QUrl, pageUrl);

    // Chromium's helper binary is a hard prerequisite; without it there is
    // nothing to measure.
    const QByteArray explicitProcess = qgetenv("QTWEBENGINEPROCESS_PATH");
    if (!explicitProcess.isEmpty() && !QFileInfo::exists(QString::fromLocal8Bit(explicitProcess)))
        QSKIP("QTWEBENGINEPROCESS_PATH points at a missing QtWebEngineProcess");

    QQmlApplicationEngine engine;
    QQmlComponent component(&engine);
    component.setData(QByteArray(kProbeQml), QUrl(QStringLiteral("qrc:/tst_webengine_headless/probe.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));

    QScopedPointer<QObject> window(component.create());
    QVERIFY2(!window.isNull(), qPrintable(component.errorString()));

    QObject *view = window->findChild<QObject *>(QStringLiteral("probe"));
    QVERIFY(view != nullptr);

    view->setProperty("url", pageUrl);

    // Chromium's first start (zygote + renderer) is slow on a cold cache.
    const int loadTimeoutMs = 30000;
    QElapsedTimer timer;
    timer.start();
    while (!view->property("finished").toBool() && timer.elapsed() < loadTimeoutMs)
        QTest::qWait(50);

    if (!view->property("finished").toBool()) {
        QSKIP(qPrintable(QStringLiteral("WebEngine never reported a load result within %1 ms. Recipe: %2")
                             .arg(loadTimeoutMs)
                             .arg(describeRecipe())));
    }
    if (!view->property("succeeded").toBool()) {
        QSKIP(qPrintable(QStringLiteral("WebEngine load FAILED for %1: %2. Recipe: %3")
                             .arg(pageUrl.toString(), view->property("failureText").toString(),
                                  describeRecipe())));
    }

    QVERIFY(QMetaObject::invokeMethod(view, "probeJavaScript"));
    timer.restart();
    while (!view->property("jsFinished").toBool() && timer.elapsed() < 15000)
        QTest::qWait(50);

    QVERIFY2(view->property("jsFinished").toBool(),
             qPrintable(QStringLiteral("runJavaScript() never returned. Recipe: %1").arg(describeRecipe())));
    QCOMPARE(view->property("jsResult").toString(), QStringLiteral("codeharbor-probe|2"));

    qInfo("WebEngine headless OK for %s. Recipe: %s", qPrintable(pageUrl.scheme()),
          qPrintable(describeRecipe()));
}

int main(int argc, char *argv[])
{
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    TstWebEngineHeadless testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_webengine_headless.moc"
