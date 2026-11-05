#include "Notifier.h"

#include <QCoreApplication>
#include <QDebug>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>

#if CH_HAVE_DBUS
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

// LIVE gate: requires acknowledgements from the real session's notification
// daemon. A successful D-Bus reply proves acceptance, not that a compositor
// displayed a bubble (desktop policy may suppress it).
//
// A machine with no session bus and no daemon (headless CI, a bare SSH login)
// is a legitimate environment, so this must not fail there. But a QSKIP that
// slides past in a green run is precisely how this project has shipped
// unproven capability before, so the skip is made impossible to misread:
//
//   * it prints an unmissable stderr banner containing kNotVerifiedToken;
//   * src/app/tests/CMakeLists.txt matches that token with
//     SKIP_REGULAR_EXPRESSION, so ctest reports the target as *Skipped*, not
//     Passed — it is absent from the pass count and called out in the summary;
//   * the target carries its OWN `desktop` label, not `live`. Every other
//     `live` test needs the SSH fixture; this one needs a desktop session bus,
//     an unrelated prerequisite. So a green `ctest -L live` no longer contains
//     — and can no longer be read as containing — a notification proof. Ask
//     for it explicitly with `ctest --preset dev -L desktop`.

// The exact string src/app/tests/CMakeLists.txt greps for. Changing it here
// without changing it there turns the skip back into a silent pass.
static constexpr auto kNotVerifiedToken = "NOTIFICATION DELIVERY NOT VERIFIED";

class TestNotifierLive : public QObject {
    Q_OBJECT
private slots:
    void deliversToTheSessionBus();
};

void TestNotifierLive::deliversToTheSessionBus()
{
    ch::Notifier notifier;
    if (!notifier.available()) {
        qWarning().noquote().nospace()
            << "\n"
            << QString(78, u'*') << "\n"
            << kNotVerifiedToken << "\n"
            << "No session bus / no org.freedesktop.Notifications service, so\n"
            << "NOTHING WAS DELIVERED and NOTHING WAS PROVEN about desktop\n"
            << "notification display. This is a legitimate environment and is\n"
            << "NOT a failure -- but it is NOT a pass either: ctest reports\n"
            << "this target as Skipped. Run it on a real desktop session\n"
            << "(ctest --preset dev -L desktop) to actually prove delivery.\n"
            << QString(78, u'*');
        QSKIP("NOTIFICATION DELIVERY NOT VERIFIED: no session bus / no "
              "org.freedesktop.Notifications service");
    }

    notifier.setCoalesceWindowMs(60000);
    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    notifier.notify(QStringLiteral("CodeHarbor"),
                    QStringLiteral("live notifier gate"));
    QCOMPARE(raised.count(), 1);

#if CH_HAVE_DBUS
    // Capture the actual call issued by Notifier, without exposing a production
    // acknowledgement API solely for this test. Copy the pending call before
    // the notifier's finished handler deletes its watcher.
    const auto firstWatchers =
        notifier.findChildren<QDBusPendingCallWatcher*>(QString(),
                                                       Qt::FindDirectChildrenOnly);
    QVERIFY(!firstWatchers.isEmpty());
    const QPointer<QDBusPendingCallWatcher> firstWatcher = firstWatchers.constLast();
    const QDBusPendingCall firstCall = *firstWatcher;
    QTRY_VERIFY_WITH_TIMEOUT(firstCall.isFinished(), 30000);
    const QDBusPendingReply<unsigned int> firstReply = firstCall;
    QVERIFY2(!firstReply.isError(), qPrintable(firstReply.error().message()));
    QVERIFY2(firstReply.value() != 0, "the daemon returned no notification id");
    // Wait for Notifier's finished handler to adopt the id before asking it
    // for a replacement; merely seeing a ready reply does not run that slot.
    QTRY_VERIFY_WITH_TIMEOUT(firstWatcher.isNull(), 30000);
#endif

    QSignalSpy coalesced(&notifier, &ch::Notifier::notificationCoalesced);
    notifier.notify(QStringLiteral("CodeHarbor"),
                    QStringLiteral("live notifier gate"));
    QCOMPARE(coalesced.count(), 1);
    QCOMPARE(raised.count(), 1);
#if CH_HAVE_DBUS
    const auto replacementWatchers =
        notifier.findChildren<QDBusPendingCallWatcher*>(QString(),
                                                       Qt::FindDirectChildrenOnly);
    QVERIFY(!replacementWatchers.isEmpty());
    const QDBusPendingCall replacementCall = *replacementWatchers.constLast();
    QTRY_VERIFY_WITH_TIMEOUT(replacementCall.isFinished(), 30000);
    const QDBusPendingReply<unsigned int> replacementReply = replacementCall;
    QVERIFY2(!replacementReply.isError(),
             qPrintable(replacementReply.error().message()));
    QCOMPARE(replacementReply.value(), firstReply.value());
#endif

    // The positive counterpart to the banner above: a reader scanning the log
    // can tell a daemon acknowledgement apart from a skip without decoding
    // ctest's result column.
    qInfo().noquote()
        << "NOTIFICATION DAEMON ACKNOWLEDGEMENT VERIFIED against "
           "org.freedesktop.Notifications; visual display is not verified";
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
