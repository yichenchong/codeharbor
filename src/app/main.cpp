#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtWebEngineQuick/QtWebEngineQuick>
#include <QtQuickControls2/QQuickStyle>

int main(int argc, char *argv[])
{
    // WebEngine must be initialised before the GUI application (SPEC 7).
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setApplicationVersion(QStringLiteral(CODEHARBOR_VERSION));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("CodeHarbor", "Main");

    return app.exec();
}
