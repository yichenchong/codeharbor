#include <QtTest/QtTest>

#include <QByteArray>
#include <QList>
#include <QSignalSpy>

#include "SessionState.h"
#include "TerminalController.h"

using namespace ch;

class TstTerminalController : public QObject {
    Q_OBJECT

private slots:
    void flushesOnSizeThreshold();
    void sizeFlushCancelsTimerNoDoubleFlushPreservesOrder();
    void flushesOnTimeThreshold();
    void emptyOutputNeverFlushes();
    void hiddenDrainRetainsCapAndEvictsOldest();
    void hiddenBufferReplaysOnBecomingVisible();
    void stateTransitionsEmitInOrder();
    void unchangedStateDoesNotEmit();
    void tmuxTargetFormat();
    void tmuxNewSessionCommandFormat();
    void tmuxNewSessionCommandEscapesShellMetacharacters();
    void reconnectBackoffSchedule();
};

// A single ingest at or above the size cap flushes synchronously (SPEC 5.5).
void TstTerminalController::flushesOnSizeThreshold()
{
    TerminalController controller;
    QSignalSpy spy(&controller, &TerminalController::flushReady);

    const QByteArray chunk(TerminalController::kFlushSizeBytes, 'x');
    controller.ingestOutput(chunk);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), chunk);
}

// A size-triggered flush mid-stream must cancel the armed time-timer so no stale
// timeout produces a second (empty) flush, and it must coalesce the earlier
// buffered bytes with the size-crossing chunk in arrival order. A later ingest
// re-arms the timer, proving it restarts cleanly after a flush (SPEC 5.5).
void TstTerminalController::sizeFlushCancelsTimerNoDoubleFlushPreservesOrder()
{
    TerminalController controller;
    QSignalSpy spy(&controller, &TerminalController::flushReady);

    const QByteArray head("abc");
    const QByteArray bulk(TerminalController::kFlushSizeBytes, 'x');
    controller.ingestOutput(head); // sub-threshold: arms the time-timer
    controller.ingestOutput(bulk); // crosses the size cap: flush now

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), head + bulk); // order preserved

    // The armed timer was cancelled by the size flush: no empty second flush.
    QVERIFY(!spy.wait(50));
    QCOMPARE(spy.count(), 1);

    // A fresh sub-threshold ingest re-arms and fires the timer independently.
    const QByteArray tail("def");
    controller.ingestOutput(tail);
    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toByteArray(), tail);
}

// Sub-threshold output flushes once the time window elapses (SPEC 5.5).
void TstTerminalController::flushesOnTimeThreshold()
{
    TerminalController controller;
    QSignalSpy spy(&controller, &TerminalController::flushReady);

    const QByteArray chunk("small");
    controller.ingestOutput(chunk);
    QCOMPARE(spy.count(), 0); // below size cap: nothing yet

    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), chunk);
}

// Empty input is a no-op; it neither arms the timer nor emits (edge case).
void TstTerminalController::emptyOutputNeverFlushes()
{
    TerminalController controller;
    QSignalSpy spy(&controller, &TerminalController::flushReady);

    controller.ingestOutput(QByteArray());
    QVERIFY(!spy.wait(50));
    QCOMPARE(spy.count(), 0);
}

// While hidden, flushed output accumulates in the rolling buffer, capped at
// kHiddenBufferMaxBytes with the oldest bytes evicted first (SPEC 5.4/5.5).
void TstTerminalController::hiddenDrainRetainsCapAndEvictsOldest()
{
    TerminalController controller;
    controller.setViewVisible(false);
    QSignalSpy spy(&controller, &TerminalController::flushReady);

    // First fill the buffer to the cap with 'A', then push a distinct block of
    // 'B' that must evict an equal amount of the oldest 'A' bytes.
    const QByteArray fill(TerminalController::kHiddenBufferMaxBytes, 'A');
    const QByteArray tail(TerminalController::kFlushSizeBytes, 'B');
    controller.ingestOutput(fill); // >= size cap -> flush into hidden buffer
    controller.ingestOutput(tail); // >= size cap -> flush into hidden buffer

    // Hidden output is not delivered to the (suspended) view.
    QCOMPARE(spy.count(), 0);

    const QByteArray &hidden = controller.hiddenBuffer();
    QCOMPARE(hidden.size(), static_cast<qsizetype>(TerminalController::kHiddenBufferMaxBytes));
    // Newest bytes are retained at the tail...
    QCOMPARE(hidden.right(TerminalController::kFlushSizeBytes), tail);
    // ...and the oldest 'A' bytes at the front were evicted, leaving the rest.
    QCOMPARE(hidden.at(0), 'A');
    QCOMPARE(hidden.count('B'), static_cast<qsizetype>(TerminalController::kFlushSizeBytes));
    QCOMPARE(hidden.count('A'),
             static_cast<qsizetype>(TerminalController::kHiddenBufferMaxBytes
                                    - TerminalController::kFlushSizeBytes));
}

// Becoming visible replays the retained buffer once and clears it (SPEC 5.4).
void TstTerminalController::hiddenBufferReplaysOnBecomingVisible()
{
    TerminalController controller;
    controller.setViewVisible(false);

    const QByteArray chunk(TerminalController::kFlushSizeBytes, 'z');
    controller.ingestOutput(chunk);
    QVERIFY(!controller.hiddenBuffer().isEmpty());

    QSignalSpy spy(&controller, &TerminalController::flushReady);
    controller.setViewVisible(true);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), chunk);
    QVERIFY(controller.hiddenBuffer().isEmpty());
}

// The lifecycle emits stateChanged in the SPEC 5.6 transition order.
void TstTerminalController::stateTransitionsEmitInOrder()
{
    TerminalController controller;
    QVERIFY(controller.state() == TerminalState::Unloaded);

    QList<TerminalState> seen;
    connect(&controller, &TerminalController::stateChanged, this,
            [&seen](TerminalState s) { seen.append(s); });

    const QList<TerminalState> sequence = {
        TerminalState::Connecting,     TerminalState::Authenticating,
        TerminalState::OpeningChannel, TerminalState::AttachingTmux,
        TerminalState::Ready,          TerminalState::Disconnected,
        TerminalState::Reconnecting,   TerminalState::Error,
    };
    for (TerminalState s : sequence)
        controller.setState(s);

    QVERIFY(seen == sequence);
    QVERIFY(controller.state() == TerminalState::Error);
}

// Re-setting the current state is a no-op and emits nothing (edge case).
void TstTerminalController::unchangedStateDoesNotEmit()
{
    TerminalController controller;
    controller.setState(TerminalState::Ready);

    QSignalSpy spy(&controller, &TerminalController::stateChanged);
    controller.setState(TerminalState::Ready);
    QCOMPARE(spy.count(), 0);
}

// Stable tmux target: ch_<devSessionId>_<terminalId> (SPEC 5.2).
void TstTerminalController::tmuxTargetFormat()
{
    const QString target = TerminalController::tmuxTarget(
        DevSessionId{QStringLiteral("dev1")}, TerminalId{QStringLiteral("term1")});
    QCOMPARE(target, QStringLiteral("ch_dev1_term1"));
}

// Attach-or-create command formatting (SPEC 5.2).
void TstTerminalController::tmuxNewSessionCommandFormat()
{
    const QString command = TerminalController::tmuxNewSessionCommand(
        DevSessionId{QStringLiteral("dev1")}, TerminalId{QStringLiteral("term1")},
        QStringLiteral("/home/dev/project"));
    QCOMPARE(command,
             QStringLiteral("tmux new-session -A -s 'ch_dev1_term1' -c '/home/dev/project'"));
}

// A workingDir carrying a single quote and shell metacharacters must be safely
// single-quoted (embedded quote rewritten as '\'') so nothing escapes the
// quoting and injects shell (SPEC 5.2 hardening).
void TstTerminalController::tmuxNewSessionCommandEscapesShellMetacharacters()
{
    const QString command = TerminalController::tmuxNewSessionCommand(
        DevSessionId{QStringLiteral("dev1")}, TerminalId{QStringLiteral("term1")},
        QStringLiteral("/home/dev/it's here; rm -rf /"));
    QCOMPARE(command,
             QStringLiteral("tmux new-session -A -s 'ch_dev1_term1' "
                            "-c '/home/dev/it'\\''s here; rm -rf /'"));
}

// Reconnect backoff: 1, 2, 5, 10, 30, then 60 thereafter (SPEC 5.6).
void TstTerminalController::reconnectBackoffSchedule()
{
    QCOMPARE(TerminalController::reconnectDelaySeconds(-1), 1);
    QCOMPARE(TerminalController::reconnectDelaySeconds(0), 1);
    QCOMPARE(TerminalController::reconnectDelaySeconds(1), 2);
    QCOMPARE(TerminalController::reconnectDelaySeconds(2), 5);
    QCOMPARE(TerminalController::reconnectDelaySeconds(3), 10);
    QCOMPARE(TerminalController::reconnectDelaySeconds(4), 30);
    QCOMPARE(TerminalController::reconnectDelaySeconds(5), 60);
    QCOMPARE(TerminalController::reconnectDelaySeconds(6), 60);
    QCOMPARE(TerminalController::reconnectDelaySeconds(100), 60);
}

QTEST_GUILESS_MAIN(TstTerminalController)
#include "tst_terminalcontroller.moc"
