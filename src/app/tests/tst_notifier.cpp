#include "Notifier.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QtGlobal>

#include <atomic>

namespace {
// Counts qWarning/qCritical/qFatal emitted while installed. The no-bus path
// must be *silent*: a user running headless (CI, ssh session, no daemon) must
// not see log noise every time an agent wants attention.
std::atomic<int> g_warnings{0};
QtMessageHandler g_previous = nullptr;

void countingHandler(QtMsgType type, const QMessageLogContext& ctx,
                     const QString& msg)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        g_warnings.fetch_add(1);
    if (g_previous)
        g_previous(type, ctx, msg);
}
} // namespace

// Notifier's no-backend behaviour, gating and coalescing (SPEC 6.2).
//
// main() below forces DBUS_SESSION_BUS_ADDRESS to an unusable value before
// QCoreApplication exists, so QDBusConnection::sessionBus() cannot connect and
// every case here runs against the *headless* code path regardless of whether
// the developer's machine happens to have a session bus. Real delivery over a
// live bus is covered by the separately-labelled tst_notifierlive.
class TestNotifier : public QObject {
    Q_OBJECT

private slots:
    void availableIsFalseAndNotifyIsSilent();
    void disabledSuppressesEverything();
    void identicalRapidNotificationsCoalesce();
    void distinctNotificationsAreNotCoalesced();
    void coalescingExpiresWithTheWindow();
    void zeroWindowDisablesCoalescing();
};

void TestNotifier::availableIsFalseAndNotifyIsSilent()
{
    g_warnings.store(0);
    g_previous = qInstallMessageHandler(countingHandler);

    ch::Notifier notifier;
    QVERIFY(!notifier.available());

    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    notifier.notify(QStringLiteral("Agent waiting"),
                    QStringLiteral("api-refactor needs input"));
    // The no-op is only about the OS bubble: the observable in-process signal
    // still fires so the rest of the app can react uniformly.
    QCOMPARE(raised.count(), 1);

    // Give any (nonexistent) async DBus machinery a chance to complain.
    QCoreApplication::processEvents();

    qInstallMessageHandler(g_previous);
    QCOMPARE(g_warnings.load(), 0);
}

void TestNotifier::disabledSuppressesEverything()
{
    ch::Notifier notifier;
    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    QSignalSpy coalesced(&notifier, &ch::Notifier::notificationCoalesced);
    QSignalSpy enabled(&notifier, &ch::Notifier::enabledChanged);

    QVERIFY(notifier.isEnabled());
    notifier.setEnabled(false);
    QCOMPARE(enabled.count(), 1);
    QCOMPARE(enabled.at(0).at(0).toBool(), false);

    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    QCOMPARE(raised.count(), 0);
    QCOMPARE(coalesced.count(), 0);

    // Setting the same value again is not a change.
    notifier.setEnabled(false);
    QCOMPARE(enabled.count(), 1);

    // Re-enabling lets the very next notification through as a fresh bubble:
    // the suppressed ones never became "last", so nothing coalesces against.
    notifier.setEnabled(true);
    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    QCOMPARE(raised.count(), 1);
    QCOMPARE(coalesced.count(), 0);
}

void TestNotifier::identicalRapidNotificationsCoalesce()
{
    ch::Notifier notifier;
    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    QSignalSpy coalesced(&notifier, &ch::Notifier::notificationCoalesced);

    for (int i = 0; i < 5; ++i)
        notifier.notify(QStringLiteral("Agent idle"),
                        QStringLiteral("build-fix finished"));

    QCOMPARE(raised.count(), 1);
    QCOMPARE(coalesced.count(), 4);
    QCOMPARE(raised.at(0).at(0).toString(), QStringLiteral("Agent idle"));
    QCOMPARE(raised.at(0).at(1).toString(), QStringLiteral("build-fix finished"));
}

void TestNotifier::distinctNotificationsAreNotCoalesced()
{
    ch::Notifier notifier;
    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    QSignalSpy coalesced(&notifier, &ch::Notifier::notificationCoalesced);

    // Differing body only.
    notifier.notify(QStringLiteral("Agent idle"), QStringLiteral("one"));
    notifier.notify(QStringLiteral("Agent idle"), QStringLiteral("two"));
    // Differing title only.
    notifier.notify(QStringLiteral("Agent waiting"), QStringLiteral("two"));
    QCOMPARE(raised.count(), 3);
    QCOMPARE(coalesced.count(), 0);

    // Only the most recent pair is tracked, so alternating pairs never
    // coalesce against each other.
    notifier.notify(QStringLiteral("Agent idle"), QStringLiteral("one"));
    QCOMPARE(raised.count(), 4);
    QCOMPARE(coalesced.count(), 0);
}

void TestNotifier::coalescingExpiresWithTheWindow()
{
    ch::Notifier notifier;
    notifier.setCoalesceWindowMs(40);
    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    QSignalSpy coalesced(&notifier, &ch::Notifier::notificationCoalesced);

    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    QCOMPARE(raised.count(), 1);
    QCOMPARE(coalesced.count(), 1);

    QTest::qWait(80);
    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    QCOMPARE(raised.count(), 2);
    QCOMPARE(coalesced.count(), 1);
}

void TestNotifier::zeroWindowDisablesCoalescing()
{
    ch::Notifier notifier;
    notifier.setCoalesceWindowMs(0);
    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    QSignalSpy coalesced(&notifier, &ch::Notifier::notificationCoalesced);

    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    QCOMPARE(raised.count(), 2);
    QCOMPARE(coalesced.count(), 0);
}

int main(int argc, char** argv)
{
    // Must happen before QCoreApplication: QDBusConnection::sessionBus()
    // resolves and caches the address on first use.
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/codeharbor-no-bus");
    QCoreApplication app(argc, argv);
    TestNotifier tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_notifier.moc"
