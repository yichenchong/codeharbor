#include "Notifier.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

// LIVE gate: actually delivers a notification to whatever daemon owns
// org.freedesktop.Notifications on the session bus. QSKIPs when there is no bus
// or no daemon (headless CI), so the `live` label can be run anywhere.
class TestNotifierLive : public QObject {
    Q_OBJECT
private slots:
    void deliversToTheSessionBus();
};

void TestNotifierLive::deliversToTheSessionBus()
{
    ch::Notifier notifier;
    if (!notifier.available())
        QSKIP("no session bus / no org.freedesktop.Notifications service");

    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    notifier.notify(QStringLiteral("CodeHarbor"),
                    QStringLiteral("live notifier gate"));
    QCOMPARE(raised.count(), 1);

    // Let the async Notify round-trip complete; a failing call would surface
    // as an error reply, which the notifier swallows, so we assert only that
    // the delivery does not crash or hang and the second identical call is
    // coalesced into a replacement rather than a second bubble.
    QTest::qWait(500);

    QSignalSpy coalesced(&notifier, &ch::Notifier::notificationCoalesced);
    notifier.notify(QStringLiteral("CodeHarbor"),
                    QStringLiteral("live notifier gate"));
    QCOMPARE(coalesced.count(), 1);
    QCOMPARE(raised.count(), 1);
    QTest::qWait(500);
}

// Plain QCoreApplication: notifications need no GUI, and QTEST_MAIN would pull
// in QGuiApplication (ch_app links Qt6::Gui) and abort without a display.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TestNotifierLive tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_notifierlive.moc"
