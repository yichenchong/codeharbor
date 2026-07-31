#include "Notifier.h"
#include "AgentStatusMonitor.h"

#include <QCoreApplication>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QtGlobal>

#include <atomic>
#include <cstring>

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

// Minimal read-only QIODevice standing in for the agent-status channel: the
// monitor reads AgentEvent JSONL off whatever QIODevice it is given.
class AgentFeed : public QIODevice {
public:
    AgentFeed() { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return m_buf.size() + QIODevice::bytesAvailable();
    }

    void deliver(const QByteArray& line)
    {
        m_buf.append(line);
        emit readyRead();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_buf.size());
        if (n > 0) {
            std::memcpy(data, m_buf.constData(), static_cast<size_t>(n));
            m_buf.remove(0, n);
        }
        return n;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QByteArray m_buf;
};

// One framed AgentEvent JSONL line, exactly as codeharbord emits it.
QByteArray agentEventLine(const QString& state, const QString& dev,
                          const QString& term)
{
    const QJsonObject o{{"version", 1},
                        {"timestamp", "2026-07-26T00:00:00.000Z"},
                        {"harness", "generic"},
                        {"devSessionId", dev},
                        {"terminalId", term},
                        {"state", state},
                        {"event", "tick"}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
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
    void negativeWindowDisablesCoalescing();
    void theWindowIsMeasuredFromTheLastRaise();
    void agentEventRaisesNotificationEndToEnd();
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

// The documented contract is "<= 0 disables coalescing entirely", so a negative
// window has to behave exactly like zero rather than, say, wrapping into a
// gigantic window that swallows every repeat for the rest of the run.
void TestNotifier::negativeWindowDisablesCoalescing()
{
    ch::Notifier notifier;
    notifier.setCoalesceWindowMs(-1);
    QCOMPARE(notifier.coalesceWindowMs(), -1);
    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    QSignalSpy coalesced(&notifier, &ch::Notifier::notificationCoalesced);

    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    notifier.notify(QStringLiteral("A"), QStringLiteral("b"));
    QCOMPARE(raised.count(), 2);
    QCOMPARE(coalesced.count(), 0);
}

// "Every raise restarts the window." The window is therefore measured from the
// last RAISE, not from when the object was built and not from the last call:
// a distinct notification part-way through resets the clock, and a repeat of
// THAT one is judged against its own raise. Getting this wrong would let a
// fresh bubble's first repeat slip through as a second bubble.
void TestNotifier::theWindowIsMeasuredFromTheLastRaise()
{
    ch::Notifier notifier;
    notifier.setCoalesceWindowMs(120);
    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);
    QSignalSpy coalesced(&notifier, &ch::Notifier::notificationCoalesced);

    notifier.notify(QStringLiteral("A"), QStringLiteral("first"));
    QCOMPARE(raised.count(), 1);

    // A DISTINCT notification most of the way through the window: it is raised,
    // and that raise restarts the clock.
    QTest::qWait(90);
    notifier.notify(QStringLiteral("A"), QStringLiteral("second"));
    QCOMPARE(raised.count(), 2);

    // Now past 120 ms since the FIRST raise but well inside 120 ms of the
    // second, so the repeat must coalesce.
    QTest::qWait(20);
    notifier.notify(QStringLiteral("A"), QStringLiteral("second"));
    QCOMPARE(raised.count(), 2);
    QCOMPARE(coalesced.count(), 1);
}

// SPEC 6.2 end to end, over the ONE connection that makes the feature exist:
// main.cpp's `connect(&agentMonitor, &AgentStatusMonitor::notify, &notifier,
// &Notifier::notify)`. Both sides are covered in isolation and neither test
// touches the join, so deleting that line would leave the whole suite green
// and every agent silent. Drive real AgentEvent JSONL in one end and assert a
// notification comes out the other.
void TestNotifier::agentEventRaisesNotificationEndToEnd()
{
    AgentFeed feed;
    ch::AgentStatusMonitor monitor;
    ch::Notifier notifier;
    QObject::connect(&monitor, &ch::AgentStatusMonitor::notify, &notifier,
                     &ch::Notifier::notify);
    monitor.setTransport(&feed);

    QSignalSpy raised(&notifier, &ch::Notifier::notificationRaised);

    // A terminal asking for input is attention-worthy: one bubble.
    feed.deliver(agentEventLine(QStringLiteral("waiting_input"),
                                QStringLiteral("dev-1"),
                                QStringLiteral("term-1")));
    QCOMPARE(raised.count(), 1);
    QVERIFY(!raised.at(0).at(0).toString().isEmpty());
    QVERIFY(!raised.at(0).at(1).toString().isEmpty());

    // The monitor's own transition gate is the first line of rate limiting and
    // it is what keeps a chatty agent from becoming a bubble storm: a repeat of
    // the SAME state is not a transition, so nothing reaches the Notifier at
    // all (this is stronger than coalescing - it never even gets counted).
    feed.deliver(agentEventLine(QStringLiteral("waiting_input"),
                                QStringLiteral("dev-1"),
                                QStringLiteral("term-1")));
    QCOMPARE(raised.count(), 1);

    // A genuine transition into the other attention state gets through.
    feed.deliver(agentEventLine(QStringLiteral("idle_unseen"),
                                QStringLiteral("dev-1"),
                                QStringLiteral("term-1")));
    QCOMPARE(raised.count(), 2);

    // A state nobody needs to be told about raises nothing.
    feed.deliver(agentEventLine(QStringLiteral("running"),
                                QStringLiteral("dev-1"),
                                QStringLiteral("term-1")));
    QCOMPARE(raised.count(), 2);
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
