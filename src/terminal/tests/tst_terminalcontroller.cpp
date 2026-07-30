#include <QtTest/QtTest>

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QSignalSpy>

#include <cstring>

#include "SessionState.h"
#include "TerminalController.h"

using namespace ch;

namespace {

// Stand-in for the production transport (ch::SshChannelDevice) at the seam the
// controller actually depends on: a sequential QIODevice that can be handed
// bytes as if a remote PTY had produced them, records what was written back,
// and can end its read channel. Using this instead of a QBuffer matters —
// a QBuffer's write() makes the same bytes readable, so a keystroke would come
// straight back as terminal output and every assertion about output would be
// measuring the test's own echo.
class FakeChannel : public QIODevice {
public:
    FakeChannel() { open(QIODevice::ReadWrite | QIODevice::Unbuffered); }

    // Make bytes readable and announce them, as the remote PTY does.
    void pushRemote(const QByteArray& bytes)
    {
        m_incoming.append(bytes);
        emit readyRead();
    }

    // Queue bytes WITHOUT announcing them: this is the window between a channel
    // being opened and the controller subscribing to it.
    void queueRemoteSilently(const QByteArray& bytes) { m_incoming.append(bytes); }

    // The peer went away but the device is still readable, so the controller's
    // last-chance drain can claim the tail (a socket-shaped end).
    void finishRemote() { emit readChannelFinished(); }

    // The local end closed first and only then reported the end, exactly as
    // ch::SshChannelDevice::closeChannel() does.
    void closeRemote()
    {
        close();
        emit readChannelFinished();
    }

    const QByteArray& written() const { return m_written; }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override
    {
        return m_incoming.size() + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 taken = qMin<qint64>(maxSize, m_incoming.size());
        if (taken > 0) {
            std::memcpy(data, m_incoming.constData(), static_cast<size_t>(taken));
            m_incoming.remove(0, taken);
        }
        return taken;
    }
    qint64 writeData(const char* data, qint64 maxSize) override
    {
        m_written.append(data, maxSize);
        return maxSize;
    }

private:
    QByteArray m_incoming;
    QByteArray m_written;
};

} // namespace

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
    void tmuxNewSessionCommandEnablesMouseForThisSessionOnly();
    void tmuxNewSessionCommandEscapesShellMetacharacters();
    void tmuxNewSessionCommandEscapesSubstitutionBacktickNewline();
    void tmuxCommandEscapesAdversarialIds();
    void hiddenReplayPrecedesLaterVisibleOutput();
    void reconnectBackoffSchedule();
    void isLiveStateClassifiesEveryState();
    void transportOutputIsIngestedIncludingBytesBufferedBeforeAttach();
    void detachedTransportStopsFeedingThePane();
    void destroyedTransportDetachesInsteadOfDangling();
    void transportSwapKeepsBufferedOutputAndGeometry();
    void channelEndClaimsTheFinalBytes();
    void channelEndDropsOnlyALivePane();
    void silentAttachIsBoundedAndReportedAsAnError();
    void sendInputNeedsAWritableTransport();
    void resizeRejectsNonPositiveGeometry();
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

// Attach-or-create command formatting (SPEC 5.2). The invocation carries TWO
// tmux commands separated by an escaped semicolon: the attach itself, and the
// mouse-reporting option that makes the wheel scroll tmux's history.
void TstTerminalController::tmuxNewSessionCommandFormat()
{
    const QString command = TerminalController::tmuxNewSessionCommand(
        DevSessionId{QStringLiteral("dev1")}, TerminalId{QStringLiteral("term1")},
        QStringLiteral("/home/dev/project"));
    QCOMPARE(command,
             QStringLiteral("tmux new-session -A -s 'ch_dev1_term1' -c '/home/dev/project'"
                            " \\; set-option -t '=ch_dev1_term1:' mouse on"));
}

// Mouse reporting is what routes a wheel turn into tmux's own scrollback instead
// of letting the renderer translate it into cursor keys — but it must reach ONLY
// the session this command creates. `-g` would write the user's global tmux
// option (this command names no socket, so the session lives on their default
// server), and an unpinned target would resolve by fnmatch onto a session
// somebody else created (SPEC 5.2 hardening).
void TstTerminalController::tmuxNewSessionCommandEnablesMouseForThisSessionOnly()
{
    const QString command = TerminalController::tmuxNewSessionCommand(
        DevSessionId{QStringLiteral("dev1")}, TerminalId{QStringLiteral("term1")},
        QStringLiteral("/w"));

    // The option is set, and it is set on this session's target.
    QVERIFY(command.contains(QStringLiteral("set-option -t '=ch_dev1_term1:' mouse on")));
    // No global scope, in either spelling tmux accepts.
    QVERIFY(!command.contains(QStringLiteral("-g")));
    QVERIFY(!command.contains(QStringLiteral("--global")));
    // One tmux invocation, two tmux commands: the separator is an ESCAPED
    // semicolon, so the remote shell hands tmux a literal `;` argument instead
    // of ending the shell command there.
    QVERIFY(command.contains(QStringLiteral(" \\; ")));
    QCOMPARE(command.count(QLatin1Char(';')), 1);
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
                            "-c '/home/dev/it'\\''s here; rm -rf /'"
                            " \\; set-option -t '=ch_dev1_term1:' mouse on"));
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

// Command substitution, backticks, and embedded newlines in the working
// directory are neutralized by single-quoting: inside single quotes the shell
// treats $(...), `...`, and a literal newline as data, so nothing executes and
// no argument splits on the newline (SPEC 5.2 hardening).
void TstTerminalController::tmuxNewSessionCommandEscapesSubstitutionBacktickNewline()
{
    const QString command = TerminalController::tmuxNewSessionCommand(
        DevSessionId{QStringLiteral("dev1")}, TerminalId{QStringLiteral("term1")},
        QStringLiteral("/w/$(rm -rf ~)`whoami`\nnext"));
    QCOMPARE(command,
             QStringLiteral("tmux new-session -A -s 'ch_dev1_term1' "
                            "-c '/w/$(rm -rf ~)`whoami`\nnext'"
                            " \\; set-option -t '=ch_dev1_term1:' mouse on"));
}

// Adversarial dev-session / terminal IDs carrying a quote and metacharacters
// are embedded verbatim into the raw tmux target, then single-quote escaped as
// a whole for the shell command, so a quote in an ID cannot break out of the
// quoting and inject a command (SPEC 5.2 hardening).
void TstTerminalController::tmuxCommandEscapesAdversarialIds()
{
    const DevSessionId dev{QStringLiteral("dev'; rm -rf / #")};
    const TerminalId term{QStringLiteral("t`whoami`$(id)")};

    // The identity helper keeps IDs verbatim; escaping is the command's job.
    QCOMPARE(TerminalController::tmuxTarget(dev, term),
             QStringLiteral("ch_dev'; rm -rf / #_t`whoami`$(id)"));

    const QString command =
        TerminalController::tmuxNewSessionCommand(dev, term, QStringLiteral("/w"));
    QCOMPARE(command,
             QStringLiteral("tmux new-session -A -s "
                            "'ch_dev'\\''; rm -rf / #_t`whoami`$(id)' -c '/w'"
                            " \\; set-option -t "
                            "'=ch_dev'\\''; rm -rf / #_t`whoami`$(id):' mouse on"));
}

// On becoming visible, the retained hidden buffer replays first and exactly
// once; output that was still pending (sub-threshold, not yet flushed) is NOT
// part of that replay and flushes afterwards, preserving order across the
// suspend/resume boundary (SPEC 5.4/5.5).
void TstTerminalController::hiddenReplayPrecedesLaterVisibleOutput()
{
    TerminalController controller;
    controller.setViewVisible(false);

    const QByteArray older(TerminalController::kFlushSizeBytes, 'A');
    controller.ingestOutput(older); // >= size cap -> flushed into hidden buffer
    QCOMPARE(controller.hiddenBuffer(), older);

    QSignalSpy spy(&controller, &TerminalController::flushReady);
    const QByteArray newer("B"); // sub-threshold: sits in m_pending, not hidden
    controller.ingestOutput(newer);

    controller.setViewVisible(true); // replays hidden 'A' immediately, once
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), older);
    QVERIFY(controller.hiddenBuffer().isEmpty());

    // The pending 'B' flushes on its own timer to the now-visible view, after A.
    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toByteArray(), newer);
}

// The single definition of "this pane has a channel to lose" (SPEC 5.6). Both
// onTransportFinished() and TerminalFactory::detach() drop a pane to
// Disconnected only from these states, so the classification is pinned here
// state by state rather than implicitly through one caller.
void TstTerminalController::isLiveStateClassifiesEveryState()
{
    QVERIFY(TerminalController::isLiveState(TerminalState::OpeningChannel));
    QVERIFY(TerminalController::isLiveState(TerminalState::AttachingTmux));
    QVERIFY(TerminalController::isLiveState(TerminalState::Ready));

    QVERIFY(!TerminalController::isLiveState(TerminalState::Unloaded));
    QVERIFY(!TerminalController::isLiveState(TerminalState::Connecting));
    QVERIFY(!TerminalController::isLiveState(TerminalState::Authenticating));
    QVERIFY(!TerminalController::isLiveState(TerminalState::Disconnected));
    QVERIFY(!TerminalController::isLiveState(TerminalState::Reconnecting));
    QVERIFY(!TerminalController::isLiveState(TerminalState::Error));
}

// Everything the transport emits must reach the buffer — including bytes that
// arrived BEFORE the controller subscribed. tmux redraws the whole pane the
// instant it attaches, so those first bytes are the entire visible screen and
// losing them leaves a blank terminal until the user presses a key.
void TstTerminalController::transportOutputIsIngestedIncludingBytesBufferedBeforeAttach()
{
    TerminalController controller;
    QSignalSpy spy(&controller, &TerminalController::flushReady);

    FakeChannel channel;
    // Already sitting in the channel, never announced: only setTransport()'s own
    // drain can rescue these.
    channel.queueRemoteSilently(QByteArrayLiteral("first screenful"));
    controller.setTransport(&channel);
    QCOMPARE(controller.transport(), static_cast<QIODevice*>(&channel));

    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("first screenful"));

    // And live output afterwards, through readyRead.
    channel.pushRemote(QByteArrayLiteral("later"));
    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toByteArray(), QByteArrayLiteral("later"));
}

// Detaching must actually unsubscribe: a channel the factory has released still
// exists for a turn of the event loop, and its trailing bytes belong to nobody.
void TstTerminalController::detachedTransportStopsFeedingThePane()
{
    TerminalController controller;
    FakeChannel channel;
    controller.setTransport(&channel);

    controller.setTransport(nullptr);
    QVERIFY(controller.transport() == nullptr);

    QSignalSpy spy(&controller, &TerminalController::flushReady);
    channel.pushRemote(QByteArrayLiteral("orphaned"));
    QVERIFY(!spy.wait(100));
    QCOMPARE(spy.count(), 0);
}

// Transport ownership stays with the caller, so a channel can be destroyed under
// a live pane. The controller must notice rather than disconnect() a freed
// pointer on the next attach.
void TstTerminalController::destroyedTransportDetachesInsteadOfDangling()
{
    TerminalController controller;
    auto* channel = new FakeChannel;
    controller.setTransport(channel);
    QCOMPARE(controller.transport(), static_cast<QIODevice*>(channel));

    delete channel;
    QVERIFY(controller.transport() == nullptr);

    // Both of these would be a use-after-free if the pointer were raw.
    QVERIFY(!controller.sendInput(QByteArrayLiteral("x")));
    FakeChannel replacement;
    controller.setTransport(&replacement);
    QCOMPARE(controller.transport(), static_cast<QIODevice*>(&replacement));
}

// The reconnect case (SPEC 5.6): a new channel is swapped in under the same
// pane. Neither the retained scrollback nor the pane geometry may be lost —
// the controller is the only thing that still knows the size, because the fresh
// PTY opens at the channel default.
void TstTerminalController::transportSwapKeepsBufferedOutputAndGeometry()
{
    TerminalController controller;
    controller.setViewVisible(false); // renderer suspended, so output is retained

    FakeChannel first;
    controller.setTransport(&first);
    controller.resize(200, 60);
    QCOMPARE(controller.columns(), 200);
    QCOMPARE(controller.rows(), 60);

    first.pushRemote(QByteArray(TerminalController::kFlushSizeBytes, 'A'));
    QTRY_COMPARE(controller.hiddenBuffer().size(),
                 static_cast<qsizetype>(TerminalController::kFlushSizeBytes));

    // The drop, then the fresh channel.
    controller.setTransport(nullptr);
    QCOMPARE(controller.hiddenBuffer().size(),
             static_cast<qsizetype>(TerminalController::kFlushSizeBytes));

    FakeChannel second;
    controller.setTransport(&second);
    QCOMPARE(controller.columns(), 200);
    QCOMPARE(controller.rows(), 60);

    second.pushRemote(QByteArray(TerminalController::kFlushSizeBytes, 'B'));
    QTRY_COMPARE(controller.hiddenBuffer().size(),
                 static_cast<qsizetype>(2 * TerminalController::kFlushSizeBytes));
    // Old scrollback first, new channel's output after it.
    QCOMPARE(controller.hiddenBuffer().at(0), 'A');
    QCOMPARE(controller.hiddenBuffer().back(), 'B');
}

// A remote process that prints and immediately exits delivers its last bytes
// just before the read channel ends. Those bytes are the whole point of running
// the command, so the drop handler drains once more before reporting.
void TstTerminalController::channelEndClaimsTheFinalBytes()
{
    TerminalController controller;
    controller.setState(TerminalState::Ready);
    QSignalSpy spy(&controller, &TerminalController::flushReady);

    FakeChannel channel;
    controller.setTransport(&channel);
    // Queued but never announced, then the channel ends while still readable.
    channel.queueRemoteSilently(QByteArrayLiteral("goodbye"));
    channel.finishRemote();

    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("goodbye"));
    QVERIFY(controller.state() == TerminalState::Disconnected);
}

// A channel that ends is a dropped connection only for a pane that had one
// (SPEC 5.6). Every other state must be left exactly as it was: reporting
// "disconnected" for a pane that never attached, or overwriting an Error with
// it, would replace the real reason with a misleading one.
void TstTerminalController::channelEndDropsOnlyALivePane()
{
    const QList<TerminalState> live = {TerminalState::OpeningChannel,
                                       TerminalState::AttachingTmux,
                                       TerminalState::Ready};
    for (TerminalState state : live) {
        TerminalController controller;
        controller.setState(state);
        FakeChannel channel;
        controller.setTransport(&channel);
        channel.closeRemote();
        QVERIFY2(controller.state() == TerminalState::Disconnected,
                 qPrintable(toString(state)));
    }

    const QList<TerminalState> untouched = {TerminalState::Unloaded,
                                            TerminalState::Connecting,
                                            TerminalState::Authenticating,
                                            TerminalState::Disconnected,
                                            TerminalState::Reconnecting,
                                            TerminalState::Error};
    for (TerminalState state : untouched) {
        TerminalController controller;
        controller.setState(state);
        FakeChannel channel;
        controller.setTransport(&channel);
        QSignalSpy spy(&controller, &TerminalController::stateChanged);
        channel.closeRemote();
        QVERIFY2(controller.state() == state, qPrintable(toString(state)));
        QCOMPARE(spy.count(), 0);
    }
}

// The transition out of AttachingTmux is driven by the PANE's first bytes
// (TerminalFactory::attach()), and a channel that ends drives Disconnected. A
// tmux attach that produces neither — a tmux server wedged on its socket, or a
// login shell stalled in an rc file on an unresponsive mount — is invisible to
// both, and used to leave the pane advertising "attaching_tmux" for the rest of
// the process's life. The window is bounded instead, and the pane says so.
void TstTerminalController::silentAttachIsBoundedAndReportedAsAnError()
{
    // The default is the documented one-minute budget, and a negative window is
    // not a negative timer: it is "no bound".
    TerminalController defaults;
    QCOMPARE(defaults.attachTimeoutMs(), TerminalController::kAttachTimeoutMs);
    defaults.setAttachTimeoutMs(-1);
    QCOMPARE(defaults.attachTimeoutMs(), 0);

    // (1) THE STALL. The channel stays open the whole time, so there is no
    // end-of-stream for the drop path to lean on: only the bound can end this.
    TerminalController controller;
    controller.setAttachTimeoutMs(50);
    QSignalSpy timedOut(&controller, &TerminalController::attachTimedOut);
    controller.setState(TerminalState::OpeningChannel);
    controller.setState(TerminalState::AttachingTmux);
    FakeChannel channel;
    controller.setTransport(&channel);

    QVERIFY(timedOut.wait(2000));
    QVERIFY(controller.state() == TerminalState::Error);
    QCOMPARE(timedOut.count(), 1);
    // The Error it produced must not re-arm the watchdog on itself.
    QTest::qWait(200);
    QCOMPARE(timedOut.count(), 1);

    // (2) NO FALSE POSITIVE. A pane whose first bytes arrive inside the window
    // is Ready, and Ready stops the clock: reporting a working terminal as
    // failed is worse than the stall this guards against.
    TerminalController live;
    live.setAttachTimeoutMs(50);
    QSignalSpy liveTimedOut(&live, &TerminalController::attachTimedOut);
    live.setState(TerminalState::AttachingTmux);
    live.setState(TerminalState::Ready);
    QTest::qWait(200);
    QVERIFY(live.state() == TerminalState::Ready);
    QCOMPARE(liveTimedOut.count(), 0);

    // (3) A REAL REASON WINS. A pane that dropped while attaching keeps
    // Disconnected; the expired window must not overwrite it with Error.
    TerminalController dropped;
    dropped.setAttachTimeoutMs(50);
    dropped.setState(TerminalState::AttachingTmux);
    dropped.setState(TerminalState::Disconnected);
    QTest::qWait(200);
    QVERIFY(dropped.state() == TerminalState::Disconnected);

    // (4) The window can be changed WHILE a pane is attaching, and takes effect
    // for that pane rather than silently applying to the next one.
    TerminalController retimed;
    retimed.setState(TerminalState::AttachingTmux);
    retimed.setAttachTimeoutMs(50);
    QTRY_VERIFY(retimed.state() == TerminalState::Error);

    // (5) 0 restores "wait for the first byte for as long as it takes", which is
    // what a host on a very slow link opts into.
    TerminalController unbounded;
    unbounded.setAttachTimeoutMs(0);
    unbounded.setState(TerminalState::AttachingTmux);
    QTest::qWait(200);
    QVERIFY(unbounded.state() == TerminalState::AttachingTmux);
}

// Keystrokes need a transport that is open and writable; without one the pane
// must say so rather than silently swallow what the user typed.
void TstTerminalController::sendInputNeedsAWritableTransport()
{
    TerminalController controller;
    QVERIFY(!controller.sendInput(QByteArrayLiteral("ls\n")));

    FakeChannel channel;
    controller.setTransport(&channel);
    QVERIFY(controller.sendInput(QByteArrayLiteral("ls\n")));
    QCOMPARE(channel.written(), QByteArrayLiteral("ls\n"));

    // Nothing to send is not a failure: the pane is writable.
    QVERIFY(controller.sendInput(QByteArray()));
    QCOMPARE(channel.written(), QByteArrayLiteral("ls\n"));

    // Once the channel is closed the write must be refused, not attempted.
    channel.closeRemote();
    QVERIFY(!controller.sendInput(QByteArrayLiteral("more")));
    QCOMPARE(channel.written(), QByteArrayLiteral("ls\n"));
}

// A renderer that has not been laid out yet reports 0 rows and columns. Taking
// that at face value would resize the remote grid to nothing, so it is refused
// and the last real geometry is kept — that is the size a reconnect re-applies.
void TstTerminalController::resizeRejectsNonPositiveGeometry()
{
    TerminalController controller;
    QCOMPARE(controller.columns(), 0); // never reported
    QCOMPARE(controller.rows(), 0);

    QVERIFY(!controller.resize(0, 24));
    QVERIFY(!controller.resize(80, 0));
    QVERIFY(!controller.resize(-1, -1));
    QCOMPARE(controller.columns(), 0);
    QCOMPARE(controller.rows(), 0);

    // A real size is recorded. resize() returns false here because a plain
    // QIODevice cannot carry an SSH window-change — only ch::SshChannelDevice
    // can — so the recorded geometry, not the return value, is the contract.
    FakeChannel channel;
    controller.setTransport(&channel);
    controller.resize(132, 43);
    QCOMPARE(controller.columns(), 132);
    QCOMPARE(controller.rows(), 43);

    controller.resize(0, 0);
    QCOMPARE(controller.columns(), 132);
    QCOMPARE(controller.rows(), 43);
}

QTEST_GUILESS_MAIN(TstTerminalController)
#include "tst_terminalcontroller.moc"
