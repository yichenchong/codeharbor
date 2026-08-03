#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QVariantList>

#include <memory>
#include "LogBuffer.h"

using namespace ch;

class TstLogView final : public QObject {
    Q_OBJECT

private slots:
    void messageSurvivesClosedSheet();
};

void TstLogView::messageSurvivesClosedSheet()
{
    LogBuffer buffer;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testBuffer"), &buffer);
    QQmlComponent component(&engine,
                           QUrl(QStringLiteral("qrc:/qt/qml/CodeHarbor/LogView.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    std::unique_ptr<QObject> view(component.create());
    QVERIFY2(view != nullptr, qPrintable(component.errorString()));
    QQuickItem* const item = qobject_cast<QQuickItem*>(view.get());
    QVERIFY(item != nullptr);
    item->setProperty("logBuffer", QVariant::fromValue(&buffer));
    item->setProperty("shown", false);

    buffer.appendRemote(QStringLiteral("daemon"), QStringLiteral("rpc"),
                        QStringLiteral("bridge"), QStringLiteral("message while closed"));
    QTRY_VERIFY_WITH_TIMEOUT(
        item->property("visibleText").toString().contains(QStringLiteral("message while closed")),
        1000);
    QCOMPARE(item->property("shown").toBool(), false);

    item->setProperty("shown", true);
    QTRY_VERIFY_WITH_TIMEOUT(
        item->property("visibleText").toString().contains(QStringLiteral("message while closed")),
        1000);
}

QTEST_MAIN(TstLogView)
#include "tst_logview.moc"
