#include "AppController.h"
#include "CodeharbordClient.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

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

    // UI shell (workstream U) and viewer subsystem (workstream V) share the one
    // client. ViewerModel and ViewerProfiles share the same profiles so QML
    // WebEngineViews and the internal scheme handler use one security context.
    ch::AppController appController(&client);
    ch::ViewerProfiles profiles(&client);
    ch::ViewerModel viewers(&client);
    viewers.setProfiles(&profiles);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &appController);
    engine.rootContext()->setContextProperty(QStringLiteral("viewers"), &viewers);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("CodeHarbor", "Main");

    return app.exec();
}
