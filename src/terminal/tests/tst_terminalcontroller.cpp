#include <QtTest/QtTest>

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QSignalSpy>
#include <QStringDecoder>

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
    void outputReceivedFiresOnEveryIngestWhateverTheRendererIsDoing();
    void hiddenDrainRetainsCapAndEvictsOldest();
    void hiddenBufferReplaysOnBecomingVisible();
    void visibleOutputPastTheAckWindowIsRetainedNotEmitted();
    void acknowledgementsReleaseRetainedOutputInOrder();
    void aRendererThatNeverAcknowledgesDegradesToTheRollingBuffer();
    void aRendererThatResumesAcknowledgingRecovers();
    void visibilityChangesResetTheAcknowledgementAccount();
    void stateTransitionsEmitInOrder();
    void unchangedStateDoesNotEmit();
    void tmuxNewSessionCommandFormat();
    void tmuxNewSessionCommandEnablesMouseForThisSessionOnly();
    void tmuxNewSessionCommandEscapesShellMetacharacters();
    void tmuxNewSessionCommandEscapesSubstitutionBacktickNewline();
    void tmuxCommandEscapesAdversarialIds();
    void hiddenReplayPrecedesLaterVisibleOutput();
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
    void hiddenEvictionResumesOnACleanBoundary();
    void flushBoundariesNeverSplitAMultiByteCharacter();
    void anEvictionCannotOrphanHalfOfACharacterInTheDecoder();
    void releasingRetainedOutputStaysInsideTheCreditWindow();
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

namespace {

// Emit until exactly kMaxUnacknowledgedBytes is outstanding, acknowledging
// nothing. Shared by the flow-control tests below, which all start from a
// renderer that is at its credit limit.
void fillTheAckWindow(TerminalController &controller)
{
    const QByteArray chunk(TerminalController::kFlushSizeBytes, 'A');
    const int batches =
        TerminalController::kMaxUnacknowledgedBytes / TerminalController::kFlushSizeBytes;
    for (int i = 0; i < batches; ++i)
        controller.ingestOutput(chunk);
    QCOMPARE(controller.unacknowledgedBytes(),
             static_cast<qint64>(TerminalController::kMaxUnacknowledgedBytes));
    QVERIFY(controller.hiddenBuffer().isEmpty());
}

} // namespace

// outputReceived() is the pane's liveness signal, and ch::TerminalFactory
// forwards it to ch::AgentStatusMonitor for SPEC 6.6 activity detection on the
// adapterless "generic" harness — the only way such a pane ever gets an agent
// state, since no adapter produces an event for it.
//
// It must therefore fire for output the RENDERER never sees. flushReady() is
// silent for a hidden pane and for a visible one held back on acknowledgements,
// both of which are very much alive, so deriving liveness from it would report
// the opposite of the truth exactly when the user is not looking. Empty input
// is still nothing at all.
void TstTerminalController::outputReceivedFiresOnEveryIngestWhateverTheRendererIsDoing()
{
    TerminalController controller;
    QSignalSpy live(&controller, &TerminalController::outputReceived);
    QSignalSpy flushes(&controller, &TerminalController::flushReady);

    controller.ingestOutput(QByteArrayLiteral("x"));
    QCOMPARE(live.count(), 1);
    // Sub-threshold, so nothing has reached the renderer yet.
    QCOMPARE(flushes.count(), 0);

    // Empty output is a no-op here too.
    controller.ingestOutput(QByteArray());
    QCOMPARE(live.count(), 1);

    // Hidden: everything goes to the rolling buffer, and the pane is still
    // demonstrably producing.
    controller.setViewVisible(false);
    const int flushesBefore = flushes.count();
    controller.ingestOutput(QByteArray(TerminalController::kFlushSizeBytes, 'y'));
    QCOMPARE(live.count(), 2);
    QCOMPARE(flushes.count(), flushesBefore);

    // Visible but too far behind on acknowledgements: same story. A fresh pane,
    // because fillTheAckWindow() starts from an empty credit account.
    TerminalController atLimit;
    QSignalSpy limitLive(&atLimit, &TerminalController::outputReceived);
    QSignalSpy limitFlushes(&atLimit, &TerminalController::flushReady);
    fillTheAckWindow(atLimit);
    const int flushesAtLimit = limitFlushes.count();
    const int liveAtLimit = limitLive.count();
    atLimit.ingestOutput(QByteArray(TerminalController::kFlushSizeBytes, 'z'));
    QCOMPARE(limitLive.count(), liveAtLimit + 1);
    QCOMPARE(limitFlushes.count(), flushesAtLimit);
}

// The core of the flow control: a VISIBLE pane stops emitting once too much of
// what it already emitted is unacknowledged, and what it withholds goes into
// the same rolling buffer a hidden pane uses. Without this the controller emits
// unconditionally and a runaway remote process queues an unbounded amount of
// data in the WebChannel transport and inside Chromium.
void TstTerminalController::visibleOutputPastTheAckWindowIsRetainedNotEmitted()
{
    TerminalController controller; // visible by default: a renderer is listening
    QSignalSpy spy(&controller, &TerminalController::flushReady);

    fillTheAckWindow(controller);
    const int batches =
        TerminalController::kMaxUnacknowledgedBytes / TerminalController::kFlushSizeBytes;
    QCOMPARE(spy.count(), batches); // everything inside the window went straight out

    // One batch past the window. It must be RETAINED, not emitted.
    const QByteArray over(TerminalController::kFlushSizeBytes, 'B');
    controller.ingestOutput(over);
    QCOMPARE(spy.count(), batches);
    QCOMPARE(controller.hiddenBuffer(), over);
    QCOMPARE(controller.unacknowledgedBytes(),
             static_cast<qint64>(TerminalController::kMaxUnacknowledgedBytes));
}

// Acknowledgements are what release it again, and the release must preserve
// the byte stream exactly: same bytes, same order, no batch dropped and none
// delivered twice.
void TstTerminalController::acknowledgementsReleaseRetainedOutputInOrder()
{
    TerminalController controller;
    fillTheAckWindow(controller);

    QSignalSpy spy(&controller, &TerminalController::flushReady);

    // Five distinguishable batches, all produced while the renderer is at its
    // limit, so all of them are retained.
    QByteArray expected;
    for (char label = '0'; label < '5'; ++label) {
        const QByteArray batch(TerminalController::kFlushSizeBytes, label);
        controller.ingestOutput(batch);
        expected += batch;
    }
    QCOMPARE(spy.count(), 0);
    QCOMPARE(controller.hiddenBuffer(), expected);

    // A no-op acknowledgement is exactly that: nothing was consumed, so the
    // window is still full and nothing is released.
    controller.acknowledgeOutput(0);
    QCOMPARE(spy.count(), 0);

    // A real one releases retained output — but only as much of it as the
    // credit it just freed, so the release cannot itself blow the window it is
    // policing. One batch's worth acknowledged, one batch's worth released.
    controller.acknowledgeOutput(TerminalController::kFlushSizeBytes);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(),
             expected.left(TerminalController::kFlushSizeBytes));
    QCOMPARE(controller.unacknowledgedBytes(),
             static_cast<qint64>(TerminalController::kMaxUnacknowledgedBytes));

    // The remaining four batches drain the same way, in arrival order, with no
    // byte dropped and none delivered twice.
    QByteArray delivered = spy.at(0).at(0).toByteArray();
    while (!controller.hiddenBuffer().isEmpty()) {
        const int before = spy.count();
        controller.acknowledgeOutput(TerminalController::kFlushSizeBytes);
        QCOMPARE(spy.count(), before + 1);
        delivered += spy.at(before).at(0).toByteArray();
    }
    QCOMPARE(delivered, expected);

    // A further acknowledgement must not re-deliver anything: the buffer is
    // empty, and everything released was charged to the account.
    const int settled = spy.count();
    controller.acknowledgeOutput(TerminalController::kFlushSizeBytes);
    QCOMPARE(spy.count(), settled);

    // A nonsense acknowledgement from a page that over-reports cannot drive the
    // account negative; it only hands this pane back its full credit.
    controller.acknowledgeOutput(1'000'000'000);
    QCOMPARE(controller.unacknowledgedBytes(), static_cast<qint64>(0));
}

// A renderer that never acknowledges (crashed, wedged, or a WebEngineView whose
// page is gone) must not stall the read pump and must not leak. It degrades to
// exactly the hidden-pane behaviour: the bounded rolling buffer with
// oldest-first eviction, while the transport keeps being drained.
void TstTerminalController::aRendererThatNeverAcknowledgesDegradesToTheRollingBuffer()
{
    TerminalController controller;
    FakeChannel channel;
    controller.setTransport(&channel);
    fillTheAckWindow(controller);

    // Four times the rolling buffer, pushed through the real transport path.
    const QByteArray chunk(TerminalController::kHiddenBufferMaxBytes / 8, 'x');
    for (int i = 0; i < 32; ++i)
        channel.pushRemote(chunk);

    // The pump never stalled: every byte the channel offered was claimed.
    QCOMPARE(channel.bytesAvailable(), static_cast<qint64>(0));
    // And memory did not grow with the output: the buffer is at its cap, not at
    // the 8 MiB the remote produced.
    QCOMPARE(controller.hiddenBuffer().size(),
             static_cast<qsizetype>(TerminalController::kHiddenBufferMaxBytes));
}

// ...and when the renderer comes back, it resynchronises: the retained buffer
// is replayed and normal flow resumes.
void TstTerminalController::aRendererThatResumesAcknowledgingRecovers()
{
    TerminalController controller;
    fillTheAckWindow(controller);

    QSignalSpy spy(&controller, &TerminalController::flushReady);
    const QByteArray stalled(TerminalController::kFlushSizeBytes, 'S');
    controller.ingestOutput(stalled);
    QCOMPARE(spy.count(), 0);

    // The page starts answering again.
    controller.acknowledgeOutput(TerminalController::kMaxUnacknowledgedBytes);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), stalled);

    // The released batch is itself charged to the account, so a healthy
    // renderer has to keep acknowledging — and once it does, output flows
    // straight through again.
    controller.acknowledgeOutput(stalled.size());
    QCOMPARE(controller.unacknowledgedBytes(), static_cast<qint64>(0));

    const QByteArray live(TerminalController::kFlushSizeBytes, 'L');
    controller.ingestOutput(live);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toByteArray(), live);
    QVERIFY(controller.hiddenBuffer().isEmpty());
}

// The flow control composes with the visibility logic rather than fighting it.
// A renderer that is replaced (a page reload: hidden on the way out, visible
// again on the mount handshake) starts with a clean account — otherwise the new
// page would inherit the debt of the one it replaced and the pane would stay
// blank behind a renderer that is plainly on screen.
void TstTerminalController::visibilityChangesResetTheAcknowledgementAccount()
{
    TerminalController controller;
    fillTheAckWindow(controller);

    QSignalSpy spy(&controller, &TerminalController::flushReady);
    controller.setViewVisible(false);
    QCOMPARE(controller.unacknowledgedBytes(), static_cast<qint64>(0));

    const QByteArray missed(TerminalController::kFlushSizeBytes, 'M');
    controller.ingestOutput(missed);
    QCOMPARE(spy.count(), 0);
    QCOMPARE(controller.hiddenBuffer(), missed);

    // The replay happens on the handshake even though the previous renderer
    // never acknowledged a single byte.
    controller.setViewVisible(true);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), missed);
    QCOMPARE(controller.unacknowledgedBytes(), static_cast<qint64>(missed.size()));
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
        TerminalState::OpeningChannel, TerminalState::AttachingTmux,
        TerminalState::Ready,          TerminalState::Disconnected,
        TerminalState::Error,
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

// Attach-or-create command formatting (SPEC 5.2). The invocation carries TWO
// tmux commands separated by an escaped semicolon: the attach itself, and the
// mouse-reporting option that makes the wheel scroll tmux's history.
//
// The target is now a PARAMETER, not something this class derives. It used to
// be built here as `ch_<devSessionId>_<terminalId>` from the layout pane id, and
// that made the client a second minting site for a terminal's identity beside
// the server's — with recycling layout ids, which is how two client machines
// ended up attaching one shell. The one minting site is codeharbord's
// mintTmuxTarget(); this function only formats a command around whatever it is
// given.
void TstTerminalController::tmuxNewSessionCommandFormat()
{
    const QString command = TerminalController::tmuxNewSessionCommand(
        QStringLiteral("ch_dev1_term1"), QStringLiteral("/home/dev/project"));
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
        QStringLiteral("ch_dev1_term1"), QStringLiteral("/w"));

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
        QStringLiteral("ch_dev1_term1"), QStringLiteral("/home/dev/it's here; rm -rf /"));
    QCOMPARE(command,
             QStringLiteral("tmux new-session -A -s 'ch_dev1_term1' "
                            "-c '/home/dev/it'\\''s here; rm -rf /'"
                            " \\; set-option -t '=ch_dev1_term1:' mouse on"));
}

// Command substitution, backticks, and embedded newlines in the working
// directory are neutralized by single-quoting: inside single quotes the shell
// treats $(...), `...`, and a literal newline as data, so nothing executes and
// no argument splits on the newline (SPEC 5.2 hardening).
void TstTerminalController::tmuxNewSessionCommandEscapesSubstitutionBacktickNewline()
{
    const QString command = TerminalController::tmuxNewSessionCommand(
        QStringLiteral("ch_dev1_term1"), QStringLiteral("/w/$(rm -rf ~)`whoami`\nnext"));
    QCOMPARE(command,
             QStringLiteral("tmux new-session -A -s 'ch_dev1_term1' "
                            "-c '/w/$(rm -rf ~)`whoami`\nnext'"
                            " \\; set-option -t '=ch_dev1_term1:' mouse on"));
}

// An adversarial TARGET — one carrying a quote and shell metacharacters — is
// single-quote escaped as a whole, in both places it appears, so a quote in it
// cannot break out of the quoting and inject a command (SPEC 5.2 hardening).
//
// This used to feed adversarial dev-session and terminal IDS through the
// deleted tmuxTarget() helper. Those inputs can no longer reach here: the target
// is minted by codeharbord and validated against tmux's own grammar there
// (isSafeTmuxTarget in remote/src/tmux.ts rejects everything below), so a string
// like this is now impossible rather than merely escaped. The escaping is still
// tested, and still on purpose: it is the last line of defence for a value that
// crosses a machine boundary before it reaches a remote shell, and it must not
// quietly rot away behind the new validation.
void TstTerminalController::tmuxCommandEscapesAdversarialIds()
{
    const QString target = QStringLiteral("ch_dev'; rm -rf / #_t`whoami`$(id)");

    const QString command =
        TerminalController::tmuxNewSessionCommand(target, QStringLiteral("/w"));
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
    QVERIFY(!TerminalController::isLiveState(TerminalState::Disconnected));
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
                                            TerminalState::Disconnected,
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

// The rolling hidden buffer is replayed VERBATIM into the renderer when the
// pane comes back (SPEC 5.4), so the first byte kept after an eviction is the
// first byte xterm.js's parser sees. Cutting at the raw overflow offset lands
// wherever the byte count happens to fall — in the middle of a multi-byte UTF-8
// character (whose leftover continuation bytes decode to U+FFFD) or in the
// middle of an ANSI escape sequence (whose leftover bytes are printed as
// literal text). The cut is moved forward to a point that can be inside
// neither.
void TstTerminalController::hiddenEvictionResumesOnACleanBoundary()
{
    constexpr qsizetype kCap = TerminalController::kHiddenBufferMaxBytes;

    // (1) A line feed within the resync window is the preferred resume point:
    // no escape sequence contains one, and it is ASCII, so the byte after it
    // starts both a fresh line and a fresh character.
    {
        TerminalController controller;
        controller.setViewVisible(false);

        QByteArray fill(kCap, 'A');
        const QByteArray tail(TerminalController::kFlushSizeBytes, 'B');
        // The raw cut would land at index tail.size(); the only line feed sits
        // ten bytes past it, comfortably inside the window.
        const qsizetype newline = tail.size() + 10;
        fill[newline] = '\n';

        controller.ingestOutput(fill); // >= size cap: flushes into the hidden buffer
        controller.ingestOutput(tail); // >= size cap: overflows it

        const QByteArray &hidden = controller.hiddenBuffer();
        // Everything up to AND INCLUDING the line feed went, so eleven bytes
        // more than the raw overflow.
        QCOMPARE(hidden.size(), kCap - 11);
        QVERIFY(!hidden.contains('\n'));
        // The newest bytes are untouched: only the oldest end is trimmed.
        QCOMPARE(hidden.right(tail.size()), tail);
    }

    // (2) A full-screen TUI can redraw for a long time without emitting a line
    // feed. With no line feed in the window the cut must still land on a
    // character boundary.
    {
        TerminalController controller;
        controller.setViewVisible(false);

        // "é" is C3 A9, so every ODD index in this fill is a continuation byte.
        const QByteArray fill = QByteArrayLiteral("\xC3\xA9").repeated(kCap / 2);
        QCOMPARE(fill.size(), kCap);
        // An ODD overflow therefore puts the raw cut inside a character.
        const QByteArray tail(TerminalController::kFlushSizeBytes + 1, 'z');
        QCOMPARE(tail.size() % 2, static_cast<qsizetype>(1));

        controller.ingestOutput(fill);
        controller.ingestOutput(tail);

        const QByteArray &hidden = controller.hiddenBuffer();
        // Exactly one extra byte was dropped to reach the boundary.
        QCOMPARE(hidden.size(), kCap - 1);
        QCOMPARE(hidden.at(0), '\xC3'); // a lead byte, not an orphaned tail
        // Which is the point: decoding the replay produces no replacement
        // characters. Cutting at the raw offset would open it with U+FFFD.
        QVERIFY(!QString::fromUtf8(hidden).contains(QChar(0xFFFD)));
    }

    // (3) The resume point must be searched for FORWARD of the overflow offset,
    // never from the start of the buffer. A line feed that sits BEFORE the cut
    // is not a resume point at all — resuming there keeps bytes the eviction
    // exists to remove, so the buffer stays over its cap and the "rolling
    // buffer is bounded" guarantee quietly stops holding.
    {
        TerminalController controller;
        controller.setViewVisible(false);

        QByteArray fill(kCap, 'A');
        const QByteArray tail(TerminalController::kFlushSizeBytes, 'B');
        // The raw cut lands at index tail.size(). One line feed sits ten bytes
        // SHORT of it (a decoy) and one ten bytes past it (the real one).
        fill[tail.size() - 10] = '\n';
        fill[tail.size() + 10] = '\n';

        controller.ingestOutput(fill);
        controller.ingestOutput(tail);

        const QByteArray &hidden = controller.hiddenBuffer();
        // The LATER line feed was used, so the buffer is back under its cap.
        QVERIFY(hidden.size() <= kCap);
        QCOMPARE(hidden.size(), kCap - 11);
        QVERIFY(!hidden.contains('\n')); // both decoy and resume point are gone
        QCOMPARE(hidden.right(tail.size()), tail);
    }

    // (4) The forward scan is BOUNDED by kHiddenResyncWindowBytes, and the bound
    // has to hold even for a stream that offers no resume point at all. Without
    // it, a pathological pane — one that emits no line feed and nothing that
    // reads as a character boundary — would walk the scan across the whole
    // buffer and evict far more scrollback than the overflow called for.
    {
        TerminalController controller;
        controller.setViewVisible(false);

        // Every byte is a UTF-8 CONTINUATION byte (10xxxxxx), so the boundary
        // scan can never stop on its own, and there is no line feed either.
        const QByteArray fill(kCap, '\x80');
        const QByteArray tail(TerminalController::kFlushSizeBytes, 'z');

        controller.ingestOutput(fill);
        controller.ingestOutput(tail);

        const QByteArray &hidden = controller.hiddenBuffer();
        // The overflow plus exactly one window, and not a byte more.
        QCOMPARE(hidden.size(), kCap - TerminalController::kHiddenResyncWindowBytes);
        QCOMPARE(hidden.right(tail.size()), tail);
    }
}

// A flush must not END in the middle of a multi-byte UTF-8 character, which is
// the same rule the eviction obeys at the other end of the buffer. The bytes
// that would be cut off are held back in the pending buffer and ride on the
// next flush, so a character is emitted whole or not at all.
void TstTerminalController::flushBoundariesNeverSplitAMultiByteCharacter()
{
    // (1) The ordinary case: a batch that would end two bytes into a
    // three-byte character is cut before it, and the fragment is completed by
    // the next one.
    {
        TerminalController controller;
        QSignalSpy spy(&controller, &TerminalController::flushReady);

        controller.ingestOutput(QByteArrayLiteral("abc\xE2\x82"));
        QVERIFY(spy.wait(1000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("abc"));

        controller.ingestOutput(QByteArrayLiteral("\xAC ok"));
        QVERIFY(spy.wait(1000));
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toByteArray(), QByteArrayLiteral("\xE2\x82\xAC ok"));
    }

    // (2) When the pending buffer is NOTHING but the head of a character there
    // is no batch to emit at all, and the timer firing must not produce an
    // empty one either.
    {
        TerminalController controller;
        QSignalSpy spy(&controller, &TerminalController::flushReady);

        controller.ingestOutput(QByteArrayLiteral("\xF0\x9F"));
        QVERIFY(!spy.wait(100));
        QCOMPARE(spy.count(), 0);

        controller.ingestOutput(QByteArrayLiteral("\x98\x80"));
        QVERIFY(spy.wait(1000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("\xF0\x9F\x98\x80"));
    }

    // (3) A terminal carries arbitrary bytes, not only text. Anything that
    // cannot be the start of a character must go out immediately: holding it
    // would stall a binary stream (a `cat` of a JPEG) behind a completion that
    // is never coming.
    {
        TerminalController controller;
        QSignalSpy spy(&controller, &TerminalController::flushReady);

        // 0xFF is not a legal lead byte, and a lone 0x80 is an orphaned
        // continuation byte with no lead at all.
        controller.ingestOutput(QByteArrayLiteral("\xFF\xFE\x80"));
        QVERIFY(spy.wait(1000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("\xFF\xFE\x80"));
    }

    // (4) The same holds on the retained path: the rolling buffer of a hidden
    // pane is built out of the same batches, so it never ends mid-character
    // either.
    {
        TerminalController controller;
        controller.setViewVisible(false);

        controller.ingestOutput(QByteArrayLiteral("hi\xE2\x82"));
        QTest::qWait(50);
        QCOMPARE(controller.hiddenBuffer(), QByteArrayLiteral("hi"));
    }
}

// WHY (1) above matters, end to end. ch::TerminalBridge decodes statefully, so
// a character split across two batches is normally harmless: the decoder holds
// the lead bytes and completes them from the next batch. It stops being
// harmless when the continuation bytes are still only in the RETAINED buffer
// and an overflow evicts them — the decoder then completes its half-character
// from whatever survived the eviction and paints one wrong glyph. Never
// emitting a half-character closes that off at the source, with no extra
// signalling between the controller and the bridge.
void TstTerminalController::anEvictionCannotOrphanHalfOfACharacterInTheDecoder()
{
    constexpr int kFlush = TerminalController::kFlushSizeBytes;
    constexpr qsizetype kCap = TerminalController::kHiddenBufferMaxBytes;

    TerminalController controller; // visible: a renderer is listening
    QSignalSpy spy(&controller, &TerminalController::flushReady);

    // A batch that reaches the size threshold and would end two bytes into a
    // euro sign, so it goes straight out to the renderer.
    QByteArray straddling(kFlush - 2, 'B');
    straddling += QByteArrayLiteral("\xE2\x82");
    controller.ingestOutput(straddling);
    QCOMPARE(spy.count(), 1);

    // The pane is hidden the moment after (a tab switch), so the euro sign's
    // third byte and everything behind it is retained instead of emitted...
    controller.setViewVisible(false);
    controller.ingestOutput(QByteArrayLiteral("\xAC"));
    // ...and then the remote floods the pane, overflowing the rolling buffer
    // so its front — where that third byte sits — is evicted.
    controller.ingestOutput(QByteArray(kCap, 'C'));
    QCOMPARE(controller.hiddenBuffer().size(), kCap);

    // The pane comes back and the retained buffer replays.
    controller.setViewVisible(true);
    QVERIFY(spy.count() > 1);

    // Decode everything the controller emitted exactly as the bridge does:
    // one stateful decoder, fed batch by batch in order.
    auto decoder = QStringDecoder(QStringDecoder::Utf8);
    QString rendered;
    for (const QList<QVariant> &args : spy)
        rendered += decoder.decode(args.at(0).toByteArray());

    // No replacement character anywhere. Losing the oldest scrollback to an
    // eviction is expected and documented; being left mid-character across it
    // is not, and it is what puts a wrong glyph at the top of the replay.
    QVERIFY(!rendered.contains(QChar(0xFFFD)));
    // Every emitted byte was ASCII, so the decode is one character per byte:
    // no half-character was ever handed over to begin with.
    qsizetype emitted = 0;
    for (const QList<QVariant> &args : spy)
        emitted += args.at(0).toByteArray().size();
    QCOMPARE(rendered.size(), emitted);
}

// Releasing the retained buffer is subject to the SAME credit window as an
// ordinary flush. It used to hand the whole buffer over in one batch, which
// could put up to kHiddenBufferMaxBytes into a renderer that had credit for a
// fraction of it — the flow control's own bound broken by the mechanism that
// exists to enforce it, and precisely the unbounded WebChannel/Chromium queue
// kMaxUnacknowledgedBytes is there to prevent.
void TstTerminalController::releasingRetainedOutputStaysInsideTheCreditWindow()
{
    constexpr qsizetype kCap = TerminalController::kHiddenBufferMaxBytes;
    constexpr qsizetype kWindow = TerminalController::kMaxUnacknowledgedBytes;

    // (1) A full rolling buffer replayed to a renderer with a full window of
    // credit is handed over a window at a time, not all at once.
    {
        TerminalController controller;
        controller.setViewVisible(false);

        const QByteArray fill(kCap, 'A'); // no line feed: the exact-bound case
        controller.ingestOutput(fill);
        QCOMPARE(controller.hiddenBuffer().size(), kCap);

        QSignalSpy spy(&controller, &TerminalController::flushReady);
        controller.setViewVisible(true);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray().size(), kWindow);
        QCOMPARE(controller.unacknowledgedBytes(), static_cast<qint64>(kWindow));
        QCOMPARE(controller.hiddenBuffer().size(), kCap - kWindow);

        // Nothing is lost by cutting: the rest drains as the renderer
        // acknowledges, in order, and reassembles into exactly what went in.
        QByteArray delivered = spy.at(0).at(0).toByteArray();
        while (!controller.hiddenBuffer().isEmpty()) {
            const int before = spy.count();
            controller.acknowledgeOutput(controller.unacknowledgedBytes());
            QCOMPARE(spy.count(), before + 1);
            QVERIFY(spy.at(before).at(0).toByteArray().size() <= kWindow);
            delivered += spy.at(before).at(0).toByteArray();
        }
        QCOMPARE(delivered, fill);
    }

    // (2) The cut goes through the same resync rule the eviction uses, so it
    // prefers a line feed just past the window. That is the only way a release
    // may exceed the window, and it is bounded by the resync window — a couple
    // of terminal lines, not a couple of megabytes.
    {
        TerminalController controller;
        controller.setViewVisible(false);

        QByteArray fill(kCap, 'A');
        const qsizetype newline = kWindow + 10;
        fill[newline] = '\n';
        controller.ingestOutput(fill);

        QSignalSpy spy(&controller, &TerminalController::flushReady);
        controller.setViewVisible(true);

        QCOMPARE(spy.count(), 1);
        const QByteArray first = spy.at(0).at(0).toByteArray();
        // Up to and including the line feed, and no further.
        QCOMPARE(first.size(), newline + 1);
        QVERIFY(first.size()
                <= kWindow + TerminalController::kHiddenResyncWindowBytes);
    }
}

QTEST_GUILESS_MAIN(TstTerminalController)
#include "tst_terminalcontroller.moc"
