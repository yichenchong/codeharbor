// Unit gate for the per-pane terminal plumbing (SPEC 5.1-5.3): ch::TerminalFactory
// (controller/bridge minting, ownership, attach refusal, kill command) and
// ch::TerminalBridge (the WebChannel face the xterm.js page talks to).
//
// Nothing here needs a server: attaching a PTY is the live gate's job
// (tst_liveterminalfactory). What IS covered here is everything that must hold
// with no connection at all — including the refusal itself, which is the state
// a real user hits first.

#include <QtTest/QtTest>

#include <QBuffer>
#include <QByteArray>
#include <QPointer>
#include <QSignalSpy>
#include <QString>

#include "SessionState.h"
#include "SshConnectionPool.h"
#include "TerminalBridge.h"
#include "TerminalController.h"
#include "TerminalFactory.h"

using namespace ch;

namespace {

// A writable stand-in for the SSH PTY channel: the controller only requires a
// QIODevice, so input can be inspected with no session in sight.
class FakeTransport : public QBuffer {
public:
    FakeTransport() { open(QIODevice::ReadWrite); }
};

} // namespace

class TstTerminalFactory : public QObject {
    Q_OBJECT

private slots:
    void createParentsTheControllerToThePane();
    void createBridgeWrapsTheControllerAndDiesWithThePane();
    void attachWithoutAConnectionFailsAndReportsWhy();
    void detachAndKillWithoutAnAttachmentAreNoOps();
    void killCommandQuotesAdversarialTargets();
    void bridgeForwardsInputResizeAndVisibility();
    void bridgeHoldsOutputUntilTheRendererIsReady();
    void bridgeDecodesUtf8SplitAcrossFlushes();
    void bridgeReportsStateTransitionsAsStrings();
    void bridgeClearIsAViewOnlyRequest();
};

// Every pane owns its controller: it must be parented to the pane so closing
// the pane releases the controller, its flush timer and its buffers.
void TstTerminalFactory::createParentsTheControllerToThePane()
{
    TerminalFactory factory(nullptr);

    auto* pane = new QObject;
    QPointer<TerminalController> first = factory.create(pane);
    QPointer<TerminalController> second = factory.create(pane);

    QVERIFY(!first.isNull());
    QVERIFY(!second.isNull());
    // Distinct controllers, or two split panes would share one PTY stream.
    QVERIFY(first.data() != second.data());
    QCOMPARE(first->parent(), pane);
    QCOMPARE(second->parent(), pane);
    QVERIFY(first->state() == TerminalState::Unloaded);

    delete pane;
    QVERIFY(first.isNull());
    QVERIFY(second.isNull());
}

void TstTerminalFactory::createBridgeWrapsTheControllerAndDiesWithThePane()
{
    TerminalFactory factory(nullptr);

    auto* pane = new QObject;
    TerminalController* controller = factory.create(pane);
    QPointer<TerminalBridge> bridge = factory.createBridge(controller, pane);

    QVERIFY(!bridge.isNull());
    QCOMPARE(bridge->controller(), controller);
    QCOMPARE(bridge->parent(), pane);
    QVERIFY(!bridge->rendererReady());
    // Freshly minted panes report the controller's own lifecycle state.
    QCOMPARE(bridge->connectionState(), toString(TerminalState::Unloaded));
    // No renderer yet: the controller must buffer instead of flushing at a view
    // that does not exist (SPEC 5.4).
    QVERIFY(!controller->viewVisible());

    // A bridge with nothing to wrap is refused rather than half-built.
    QVERIFY(factory.createBridge(nullptr, pane) == nullptr);

    delete pane;
    QVERIFY(bridge.isNull());
}

// The first thing a real user hits: no server yet. attach() must refuse
// cleanly, say why, and leave the pane untouched — not half-attached.
void TstTerminalFactory::attachWithoutAConnectionFailsAndReportsWhy()
{
    SshConnectionPool pool;
    TerminalFactory factory(&pool);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    QObject pane;
    TerminalController* controller = factory.create(&pane);

    QVERIFY(!factory.connected());
    QVERIFY(!factory.attach(controller, QStringLiteral("dev-1"),
                            QStringLiteral("term-1"), QStringLiteral("/home/u"), 80, 24));

    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.at(0).at(0).value<TerminalController*>(), controller);
    QVERIFY(!errors.at(0).at(1).toString().isEmpty());

    // Nothing was opened, so nothing was recorded and the pane never moved off
    // its initial state.
    QVERIFY(!controller->transport());
    QVERIFY(controller->state() == TerminalState::Unloaded);
    QVERIFY(factory.targetFor(controller).isEmpty());

    // A factory without a pool at all behaves identically (no crash, no signal
    // storm) — that is what a pane bound before the session exists sees.
    TerminalFactory poolless(nullptr);
    QSignalSpy poollessErrors(&poolless, &TerminalFactory::error);
    QVERIFY(!poolless.connected());
    QVERIFY(!poolless.attach(controller, QStringLiteral("dev-1"),
                             QStringLiteral("term-1"), QString(), 80, 24));
    QCOMPARE(poollessErrors.count(), 1);
    // A null controller is refused without reporting an error against nobody.
    QVERIFY(!poolless.attach(nullptr, QStringLiteral("dev-1"),
                             QStringLiteral("term-1"), QString(), 80, 24));
    QCOMPARE(poollessErrors.count(), 1);
}

void TstTerminalFactory::detachAndKillWithoutAnAttachmentAreNoOps()
{
    TerminalFactory factory(nullptr);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    controller->setState(TerminalState::Connecting);

    factory.detach(controller);
    factory.detach(nullptr);
    factory.kill(controller);
    factory.kill(nullptr);

    // A pane that never attached is not "dropped": its state is left alone and
    // no phantom failure is reported.
    QVERIFY(controller->state() == TerminalState::Connecting);
    QVERIFY(!controller->transport());
    QCOMPARE(errors.count(), 0);
}

// kill() hands a command to the remote user's shell. A pane id carrying shell
// metacharacters must stay data (SPEC 5.2, same rule as the attach command).
void TstTerminalFactory::killCommandQuotesAdversarialTargets()
{
    QCOMPARE(TerminalFactory::tmuxKillSessionCommand(QStringLiteral("ch_dev_term")),
             QStringLiteral("tmux kill-session -t 'ch_dev_term'"));

    // A quote-escape attempt: the injected quote is rewritten as '\'' so the
    // rm never leaves the quoted argument.
    QCOMPARE(TerminalFactory::tmuxKillSessionCommand(
                 QStringLiteral("ch_a'; rm -rf ~; '")),
             QStringLiteral("tmux kill-session -t 'ch_a'\\''; rm -rf ~; '\\'''"));

    // Substitution, backticks and a newline are inert inside single quotes and
    // must be passed through verbatim rather than stripped.
    const QString nasty = QStringLiteral("ch_$(id)`whoami`\nX");
    const QString command = TerminalFactory::tmuxKillSessionCommand(nasty);
    QCOMPARE(command, QStringLiteral("tmux kill-session -t '") + nasty + QLatin1Char('\''));
    QVERIFY(command.startsWith(QStringLiteral("tmux kill-session -t '")));
    QVERIFY(command.endsWith(QLatin1Char('\'')));
}

// The three frozen TerminalBridge slots the page calls, each landing on the
// controller: keystrokes on the transport, geometry recorded, visibility
// toggled.
void TstTerminalFactory::bridgeForwardsInputResizeAndVisibility()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);

    FakeTransport transport;
    controller->setTransport(&transport);

    // UTF-8 on the way in as well: a composed keystroke must reach the PTY as
    // the bytes the remote expects, not as a lossy latin1 squash.
    bridge->sendInput(QStringLiteral("ls\n"));
    bridge->sendInput(QString::fromUtf8("é"));
    QCOMPARE(transport.data(), QByteArrayLiteral("ls\n\xC3\xA9"));

    QSignalSpy geometry(bridge, &TerminalBridge::geometryChanged);
    bridge->resize(100, 30);
    QCOMPARE(bridge->columns(), 100);
    QCOMPARE(bridge->rows(), 30);
    QCOMPARE(controller->columns(), 100);
    QCOMPARE(controller->rows(), 30);
    QCOMPARE(geometry.count(), 1);

    // An unlaid-out renderer reports 0: the recorded geometry must survive it,
    // otherwise the PTY would be resized to nothing.
    bridge->resize(0, 0);
    QCOMPARE(bridge->columns(), 100);
    QCOMPARE(bridge->rows(), 30);
    QCOMPARE(geometry.count(), 1); // unchanged: no spurious notify

    // Same size again is not a change either.
    bridge->resize(100, 30);
    QCOMPARE(geometry.count(), 1);

    bridge->ready();
    QVERIFY(controller->viewVisible());
    bridge->notifyViewVisible(false);
    QVERIFY(!controller->viewVisible());
    bridge->notifyViewVisible(true);
    QVERIFY(controller->viewVisible());
}

// The page mounts asynchronously; output produced while it loads must reach the
// renderer once, in order, rather than being flushed into the void (SPEC 5.4).
void TstTerminalFactory::bridgeHoldsOutputUntilTheRendererIsReady()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    QSignalSpy writes(bridge, &TerminalBridge::write);

    controller->ingestOutput(QByteArrayLiteral("early "));
    QTRY_COMPARE(controller->hiddenBuffer(), QByteArrayLiteral("early "));
    controller->ingestOutput(QByteArrayLiteral("output"));
    QTRY_COMPARE(controller->hiddenBuffer(), QByteArrayLiteral("early output"));
    QCOMPARE(writes.count(), 0);

    bridge->ready();
    QVERIFY(bridge->rendererReady());
    // Exactly one replay batch, in arrival order.
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(0).toString(), QStringLiteral("early output"));

    // From here on output flows straight through.
    controller->ingestOutput(QByteArrayLiteral("live"));
    QTRY_COMPARE(writes.count(), 2);
    QCOMPARE(writes.at(1).at(0).toString(), QStringLiteral("live"));
}

// The controller flushes on byte thresholds (SPEC 5.5) and will split a
// multi-byte sequence across batches. Decoding per batch would turn the split
// glyph into replacement characters, so the decoder must carry state.
void TstTerminalFactory::bridgeDecodesUtf8SplitAcrossFlushes()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    bridge->ready();
    QSignalSpy writes(bridge, &TerminalBridge::write);

    // "✔" is E2 9C 94; cut it after the second byte. The flush is observed on
    // the controller so the assertion below is a fact, not a race.
    QSignalSpy flushes(controller, &TerminalController::flushReady);
    controller->ingestOutput(QByteArrayLiteral("\xE2\x9C"));
    QTRY_COMPARE(flushes.count(), 1);
    // The incomplete sequence is held back rather than emitted as U+FFFD — and
    // an empty decode is not pushed at the page at all.
    QCOMPARE(writes.count(), 0);

    controller->ingestOutput(QByteArrayLiteral("\x94 done"));
    QTRY_COMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(0).toString(), QString::fromUtf8("\xE2\x9C\x94 done"));
    QVERIFY(!writes.at(0).at(0).toString().contains(QChar(0xFFFD)));
}

// The page renders the SPEC 5.6 lifecycle; it receives it as the ch::TerminalState
// string, both as a property (for a late-loading page) and as a signal.
void TstTerminalFactory::bridgeReportsStateTransitionsAsStrings()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    QSignalSpy states(bridge, &TerminalBridge::connectionStateChanged);

    controller->setState(TerminalState::OpeningChannel);
    controller->setState(TerminalState::Ready);
    QCOMPARE(states.count(), 2);
    QCOMPARE(states.at(0).at(0).toString(), toString(TerminalState::OpeningChannel));
    QCOMPARE(states.at(1).at(0).toString(), toString(TerminalState::Ready));
    QCOMPARE(bridge->connectionState(), toString(TerminalState::Ready));

    // A page that mounts late gets the current state re-announced by the
    // handshake, so its status strip is never stuck on "unloaded".
    bridge->ready();
    QCOMPARE(states.count(), 3);
    QCOMPARE(states.at(2).at(0).toString(), toString(TerminalState::Ready));

    controller->setState(TerminalState::Disconnected);
    QCOMPARE(states.count(), 4);
    QCOMPARE(bridge->connectionState(), toString(TerminalState::Disconnected));
}

void TstTerminalFactory::bridgeClearIsAViewOnlyRequest()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);

    FakeTransport transport;
    controller->setTransport(&transport);
    QSignalSpy cleared(bridge, &TerminalBridge::clearRequested);

    bridge->requestClear();

    QCOMPARE(cleared.count(), 1);
    // Clearing the screen must never type anything at the remote shell.
    QVERIFY(transport.data().isEmpty());
}

QTEST_GUILESS_MAIN(TstTerminalFactory)
#include "tst_terminalfactory.moc"
