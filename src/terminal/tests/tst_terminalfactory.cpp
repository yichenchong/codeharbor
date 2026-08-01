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
#include <QHash>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QPointer>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QVector>

#include <limits>
#include <utility>

#include "Ids.h"
#include "SessionState.h"
#include "SshConnectionPool.h"
#include "TerminalBridge.h"
#include "TerminalController.h"
#include "TerminalFactory.h"
#include "WorkspaceDb.h"
#include "WorkspaceTypes.h"

using namespace ch;

namespace {

// A writable stand-in for the SSH PTY channel: the controller only requires a
// QIODevice, so input can be inspected with no session in sight.
//
// WriteOnly on purpose. A QBuffer opened ReadWrite hands back everything written
// into it as READABLE data, so the moment an event loop turned, a keystroke sent
// through the bridge would arrive again as terminal OUTPUT and every assertion
// about what the page received would really be measuring the test's own echo.
// Write-only keeps the controller's readyRead handler inert (it refuses a
// transport that is not readable) while sendInput() still works.
class FakeTransport : public QBuffer {
public:
    FakeTransport() { open(QIODevice::WriteOnly); }
};

} // namespace

class TstTerminalFactory : public QObject {
    Q_OBJECT

private slots:
    void createParentsTheControllerToThePane();
    void createBridgeWrapsTheControllerAndDiesWithThePane();
    void attachWithoutAConnectionFailsAndReportsWhy();
    void attachWithoutATargetIsRefused();
    void resolveTargetWithoutAServerRefuses();
    void paneKeysAddressOneSlotOfOneDevSession();
    void resolveAddressesAPaneByItsRowIdAndNeverByItsLabel();
    void detachAndKillWithoutAnAttachmentAreNoOps();
    void killCommandQuotesAdversarialTargets();
    void attachCommandQuotesAdversarialIdsAndWorkingDir();
    void bridgeClampsAbsurdGeometryFromThePage();
    void bridgeExposesNoRemoteTargetingSlots();
    void bridgeForwardsInputResizeAndVisibility();
    void bridgeHoldsOutputUntilTheRendererIsReady();
    void bridgeIgnoresAVisibleReportFromAPaneWithNoRenderer();
    void bridgeRetainsOutputForAHiddenPaneAndAcrossAPageReload();
    void bridgeIsInertOnceItsControllerIsDestroyed();
    void bridgeDecodesUtf8SplitAcrossFlushes();
    void bridgeCarriesByteWeightAndFeedsAcknowledgementsBack();
    void bridgeReportsStateTransitionsAsStrings();
    void bridgeClearIsAViewOnlyRequest();
    void attachStallIsReportedAsAPaneMessage();
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
    QVERIFY(!factory.attach(controller, QStringLiteral("ch_dev-1_term-1"),
                            QStringLiteral("/home/u"), 80, 24));

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
    QVERIFY(!poolless.attach(controller, QStringLiteral("ch_dev-1_term-1"), QString(), 80, 24));
    QCOMPARE(poollessErrors.count(), 1);
    // A null controller is refused without reporting an error against nobody.
    QVERIFY(!poolless.attach(nullptr, QStringLiteral("ch_dev-1_term-1"), QString(), 80, 24));
    QCOMPARE(poollessErrors.count(), 1);
}

// A pane that has not been resolved against the server yet has no target, and
// there is no longer anything the factory could put in its place: the identity
// is a server row. Refusing is the whole point — every locally plausible
// default ("ch_" + the layout pane id, say) is a name some OTHER pane may
// already be attached to, which is how two panes end up mirroring one shell.
void TstTerminalFactory::attachWithoutATargetIsRefused()
{
    SshConnectionPool pool;
    TerminalFactory factory(&pool);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    QObject pane;
    TerminalController* controller = factory.create(&pane);

    QVERIFY(!factory.attach(controller, QString(), QStringLiteral("/home/u"), 80, 24));
    QCOMPARE(errors.count(), 1);
    QVERIFY(factory.targetFor(controller).isEmpty());
    QVERIFY(controller->state() == TerminalState::Unloaded);
}

// resolveTarget() reaches the server for a row, so with no connection it has to
// refuse exactly as attach() does — and, above all, it must not answer with a
// locally composed target. A refusal returns false, so the pane knows no answer
// is coming and can offer Retry instead of waiting for ever.
void TstTerminalFactory::resolveTargetWithoutAServerRefuses()
{
    SshConnectionPool pool;
    TerminalFactory factory(&pool);
    QSignalSpy errors(&factory, &TerminalFactory::error);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);

    QObject pane;
    TerminalController* controller = factory.create(&pane);

    QVERIFY(!factory.connected());
    QVERIFY(!factory.resolveTarget(controller, QStringLiteral("dev-1"),
                                   QStringLiteral("terminal-1"), QStringLiteral("row-1"),
                                   QStringLiteral("/home/u")));
    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.at(0).at(0).value<TerminalController*>(), controller);
    QCOMPARE(resolved.count(), 0);

    // A pane with no Dev Session cannot be addressed at all, however it is
    // considered, and a null controller reports against nobody.
    QVERIFY(!factory.resolveTarget(controller, QString(), QStringLiteral("terminal-1"),
                                   QStringLiteral("row-1"), QString()));
    QCOMPARE(errors.count(), 2);
    QVERIFY(!factory.resolveTarget(nullptr, QStringLiteral("dev-1"),
                                   QStringLiteral("terminal-1"), QStringLiteral("row-1"),
                                   QString()));
    QCOMPARE(errors.count(), 2);

    // Nothing was answered: a refusal is not a resolution, and a pane must
    // never be handed a target that did not come from the server.
    QCOMPARE(resolved.count(), 0);
}

// The address a pane resolution is keyed by. It is a PAIR — Dev Session plus
// layout pane id — and both halves matter: layout pane ids are minted per Dev
// Session, so "terminal-1" exists in every one of them and naming a terminal by
// the id alone would hand one session's pane another session's shell. The
// separator must also not be forgeable out of the parts, or two different
// addresses could collapse onto one cache entry.
void TstTerminalFactory::paneKeysAddressOneSlotOfOneDevSession()
{
    QCOMPARE(TerminalFactory::paneKey(QStringLiteral("dev-a"), QStringLiteral("terminal-1")),
             QStringLiteral("dev-a/terminal-1"));
    QVERIFY(TerminalFactory::paneKey(QStringLiteral("dev-a"), QStringLiteral("terminal-1"))
            != TerminalFactory::paneKey(QStringLiteral("dev-b"), QStringLiteral("terminal-1")));
    QVERIFY(TerminalFactory::paneKey(QStringLiteral("dev-a"), QStringLiteral("terminal-1"))
            != TerminalFactory::paneKey(QStringLiteral("dev-a"), QStringLiteral("terminal-10")));
    // The join is unambiguous because of what the LEFT half is, not because of
    // any escaping: a Dev Session id is a server-minted UUID, so it cannot
    // contain the separator and the key can only be cut in one place. Pinned
    // here so a future key format keeps that property in mind.
    const QString devSessionId = QStringLiteral("2f1c9a30-4c1b-4e3e-8d0e-6a1f9b2c3d4e");
    QVERIFY(!devSessionId.contains(QLatin1Char('/')));
    QCOMPARE(TerminalFactory::paneKey(devSessionId, QStringLiteral("terminal-1"))
                 .left(devSessionId.size()),
             devSessionId);
}

// THE fix, stated as a contract. A layout leaf that carries a `terminal_panes`
// row id is addressed by that id and by nothing else — the slot label must not
// even reach the server, because the label recycles: closing a pane leaves its
// row and its remote shell alive, and the next split on any client can mint the
// same label for a brand new pane. A leaf with NO row id predates the field, so
// its label genuinely is its historical key and lookup-or-create by label is
// correct for it exactly once.
void TstTerminalFactory::resolveAddressesAPaneByItsRowIdAndNeverByItsLabel()
{
    const QString server = QStringLiteral("srv-1");
    const QString session = QStringLiteral("2f1c9a30-4c1b-4e3e-8d0e-6a1f9b2c3d4e");
    const QString row = QStringLiteral("8ad0b1c2-1111-4222-8333-944455556666");

    const ResolveTerminalPaneParams byRow = TerminalFactory::resolveParamsFor(
        server, session, QStringLiteral("terminal-2"), row, QStringLiteral("/home/u"));
    QCOMPARE(byRow.serverId.value, server);
    QCOMPARE(byRow.devSessionId.value, session);
    QCOMPARE(byRow.id, row);
    // Not merely unused — ABSENT. A row lookup cannot create, so a working
    // directory would be a promise the server never keeps, and the label must
    // not look like part of the question.
    QVERIFY(byRow.name.isEmpty());
    QVERIFY(!byRow.workingDirectory.has_value());

    const ResolveTerminalPaneParams byLabel = TerminalFactory::resolveParamsFor(
        server, session, QStringLiteral("terminal-2"), QString(), QStringLiteral("/home/u"));
    QVERIFY(byLabel.id.isEmpty());
    QCOMPARE(byLabel.name, QStringLiteral("terminal-2"));
    // Only this path can create a row, and only a created tmux session honours
    // `-c <dir>`.
    QVERIFY(byLabel.workingDirectory.has_value());
    QCOMPARE(*byLabel.workingDirectory, QStringLiteral("/home/u"));

    // An empty working directory is omitted rather than sent as "", so the
    // server applies its own default.
    QVERIFY(!TerminalFactory::resolveParamsFor(server, session,
                                               QStringLiteral("terminal-2"), QString(),
                                               QString())
                 .workingDirectory.has_value());

    // Two panes wearing the SAME label but owning different rows ask different
    // questions. Under the old label-keyed scheme they asked the same one, and
    // the second pane was handed the first pane's shell.
    const QString otherRow = QStringLiteral("cafe0000-2222-4333-8444-955566667777");
    QVERIFY(TerminalFactory::resolveParamsFor(server, session,
                                              QStringLiteral("terminal-2"), row, QString())
                .id
            != TerminalFactory::resolveParamsFor(server, session,
                                                 QStringLiteral("terminal-2"), otherRow,
                                                 QString())
                   .id);
    // And the cache keys those two produce cannot collide with each other, nor
    // with a legacy slot address: a row id is a UUID and carries no "/".
    QVERIFY(!row.contains(QLatin1Char('/')));
    QVERIFY(row != TerminalFactory::paneKey(session, QStringLiteral("terminal-2")));
}

void TstTerminalFactory::detachAndKillWithoutAnAttachmentAreNoOps()
{
    TerminalFactory factory(nullptr);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    controller->setState(TerminalState::Unloaded);

    factory.detach(controller);
    factory.detach(nullptr);
    factory.kill(controller);
    factory.kill(nullptr);

    // A pane that never attached is not "dropped": its state is left alone and
    // no phantom failure is reported.
    QVERIFY(controller->state() == TerminalState::Unloaded);
    QVERIFY(!controller->transport());
    QCOMPARE(errors.count(), 0);
}

// kill() hands a command to the remote user's shell, built out of ids that came
// from SERVER data. Two separate escapes have to be closed and each is checked
// on its own here, because passing one proves nothing about the other:
//
//   * the SHELL escape — an id carrying quotes/`$(...)`/backticks/newlines must
//     stay one inert argument (SPEC 5.2, same rule as the attach command), and
//   * the tmux TARGET escape — `-t <name>` is a target expression, and tmux
//     resolves it exact -> prefix -> fnmatch, so a perfectly shell-safe id can
//     still name somebody else's session.
void TstTerminalFactory::killCommandQuotesAdversarialTargets()
{
    // The `=` is tmux's exact-match sigil and lives INSIDE the quotes: it is
    // tmux syntax, not shell syntax.
    QCOMPARE(TerminalFactory::tmuxKillSessionCommand(QStringLiteral("ch_dev_term")),
             QStringLiteral("tmux kill-session -t '=ch_dev_term'"));

    // A quote-escape attempt: the injected quote is rewritten as '\'' so the
    // rm never leaves the quoted argument.
    QCOMPARE(TerminalFactory::tmuxKillSessionCommand(
                 QStringLiteral("ch_a'; rm -rf ~; '")),
             QStringLiteral("tmux kill-session -t '=ch_a'\\''; rm -rf ~; '\\'''"));

    // Substitution, backticks and a newline are inert inside single quotes and
    // must be passed through verbatim rather than stripped.
    const QString nasty = QStringLiteral("ch_$(id)`whoami`\nX");
    const QString command = TerminalFactory::tmuxKillSessionCommand(nasty);
    QCOMPARE(command, QStringLiteral("tmux kill-session -t '=") + nasty + QLatin1Char('\''));
    QVERIFY(command.startsWith(QStringLiteral("tmux kill-session -t '=")));
    QVERIFY(command.endsWith(QLatin1Char('\'')));

    // Every id that is shell-inert but still a tmux PATTERN. Verified against
    // tmux 3.6 by hand: with a live `ch_victim_t1`, `kill-session -t 'ch_*_t1'`
    // killed it and `kill-session -t 'ch_long'` killed `ch_longname_t1`, while
    // both refused with "can't find session" once the `=` was present.
    for (const QString& target : {QStringLiteral("ch_*_t1"),
                                  QStringLiteral("ch_?_t1"),
                                  QStringLiteral("ch_[abc]_t1"),
                                  QStringLiteral("ch_a_t1")}) {
        const QString pinned = TerminalFactory::tmuxKillSessionCommand(target);
        QVERIFY2(pinned == QStringLiteral("tmux kill-session -t '=") + target
                               + QLatin1Char('\''),
                 qPrintable(pinned));
    }

    // A target that already looks like an option cannot become one: the sigil is
    // the first character inside the quotes, so tmux's getopt never sees a `-`.
    QCOMPARE(TerminalFactory::tmuxKillSessionCommand(QStringLiteral("-a")),
             QStringLiteral("tmux kill-session -t '=-a'"));

    // A hostile target can no longer be MINTED — codeharbord validates what it
    // stores against tmux's own grammar — but a target still crosses a machine
    // boundary before it reaches a remote shell, so the escaping stays and
    // stays tested.
    QCOMPARE(TerminalFactory::tmuxKillSessionCommand(QStringLiteral("ch_*_t1'; id; '")),
             QStringLiteral("tmux kill-session -t '=ch_*_t1'\\''; id; '\\'''"));
}

// The attach command is the other half of the same rule, and it is built by
// TerminalController for the factory. It must be shell-inert AND unable to
// resolve to another session — but note the different mechanism: `-s` on
// new-session is a NAME, not a target, so tmux matches it exactly. Verified
// against tmux 3.6: with `ch_exact_t1` live, `new-session -A -s ch_exa` created
// a SECOND session rather than attaching to the first, so no `=` is needed (and
// tmux would take one as a literal character of the new name).
//
// The attach invocation carries a SECOND tmux command (`set-option ... mouse
// on`, which gives tmux the wheel), and that one takes a real target, so both
// mechanisms appear side by side in one string: the `-s` name without a sigil,
// and the `-t` target with one.
void TstTerminalFactory::attachCommandQuotesAdversarialIdsAndWorkingDir()
{
    QCOMPARE(TerminalController::tmuxNewSessionCommand(QStringLiteral("ch_dev_t1"),
                                                       QStringLiteral("/srv/repo")),
             QStringLiteral("tmux new-session -A -s 'ch_dev_t1' -c '/srv/repo'"
                            " \\; set-option -t '=ch_dev_t1:' mouse on"));

    // A working directory is the field most likely to carry a real quote, and
    // the one a user can type. Breaking out of it would run `id` on the host.
    QCOMPARE(TerminalController::tmuxNewSessionCommand(
                 QStringLiteral("ch_dev_t1"), QStringLiteral("/tmp/x'; id; echo '")),
             QStringLiteral("tmux new-session -A -s 'ch_dev_t1' "
                            "-c '/tmp/x'\\''; id; echo '\\'''"
                            " \\; set-option -t '=ch_dev_t1:' mouse on"));

    // The target arrives from the server: a quote in it must not escape. It
    // reaches the command TWICE — as the new session's name and as the target
    // of the mouse option — so both copies are checked.
    QCOMPARE(TerminalController::tmuxNewSessionCommand(
                 QStringLiteral("ch_d'; rm -rf ~; '_t`whoami`"), QStringLiteral("/w")),
             QStringLiteral("tmux new-session -A -s 'ch_d'\\''; rm -rf ~; '\\''_t`whoami`' "
                            "-c '/w'"
                            " \\; set-option -t "
                            "'=ch_d'\\''; rm -rf ~; '\\''_t`whoami`:' mouse on"));

    // A leading `-` in the working directory is consumed as the value of -c by
    // getopt, and a newline is inert inside the quotes: neither adds a word to
    // the command.
    const QString command = TerminalController::tmuxNewSessionCommand(
        QStringLiteral("ch_dev_t1"), QStringLiteral("-rf /\nrm -rf ~"));
    QCOMPARE(command,
             QStringLiteral("tmux new-session -A -s 'ch_dev_t1' -c '-rf /\nrm -rf ~'"
                            " \\; set-option -t '=ch_dev_t1:' mouse on"));
    // Exactly three quoted arguments — the session name, the working directory
    // and the mouse option's target — so nothing became a fourth word.
    QCOMPARE(command.count(QLatin1Char('\'')), 6);
    // The option's target is tmux-pinned with the `=` sigil INSIDE the quotes,
    // so it cannot fnmatch onto another session; the sigil is tmux syntax, not
    // shell syntax (the same rule as the kill command above).
    QVERIFY(command.contains(QStringLiteral("-t '=ch_dev_t1:'")));
}

// The bridge is reachable from the WebEngine page, so `cols`/`rows` are values
// an ATTACKER supplies once the renderer is compromised — and they do not stop
// at this process: resize() becomes an SSH window-change and sizes a grid on
// the remote host. An unbounded value is a remote allocation primitive.
void TstTerminalFactory::bridgeClampsAbsurdGeometryFromThePage()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);

    // A real renderer's size passes through untouched, whatever else happens.
    bridge->resize(120, 40);
    QCOMPARE(controller->columns(), 120);
    QCOMPARE(controller->rows(), 40);

    bridge->resize((std::numeric_limits<int>::max)(), (std::numeric_limits<int>::max)());
    QCOMPARE(controller->columns(), TerminalBridge::kMaxDimension);
    QCOMPARE(controller->rows(), TerminalBridge::kMaxDimension);

    // One dimension at a time, so a clamp applied to only one is caught.
    bridge->resize(90, 1'000'000);
    QCOMPARE(controller->columns(), 90);
    QCOMPARE(controller->rows(), TerminalBridge::kMaxDimension);
    bridge->resize(1'000'000, 30);
    QCOMPARE(controller->columns(), TerminalBridge::kMaxDimension);
    QCOMPARE(controller->rows(), 30);

    // The bound is an upper bound only: a renderer that has not been laid out
    // reports 0, and clamping that UP to 1 would resize a live PTY to one cell.
    bridge->resize(0, 0);
    QCOMPARE(controller->columns(), TerminalBridge::kMaxDimension);
    QCOMPARE(controller->rows(), 30);
    bridge->resize(-2'000'000'000, -1);
    QCOMPARE(controller->columns(), TerminalBridge::kMaxDimension);
    QCOMPARE(controller->rows(), 30);

    // Exactly at the bound is a legitimate size, not something to clamp away.
    bridge->resize(TerminalBridge::kMaxDimension, TerminalBridge::kMaxDimension);
    QCOMPARE(controller->columns(), TerminalBridge::kMaxDimension);
    QCOMPARE(controller->rows(), TerminalBridge::kMaxDimension);
}

// Everything the WebChannel publishes is callable by the page, so the shape of
// this object IS the security boundary. The page may drive the pane it is
// already rendering; it must not be able to name a DIFFERENT remote target.
// This asserts the published surface directly through the meta-object, which is
// exactly what qwebchannel.js enumerates.
void TstTerminalFactory::bridgeExposesNoRemoteTargetingSlots()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalBridge* bridge = factory.createBridge(factory.create(&pane), &pane);

    const QMetaObject* meta = bridge->metaObject();
    QStringList callable;
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        if (method.methodType() == QMetaMethod::Slot
            || method.methodType() == QMetaMethod::Method)
            callable << QString::fromLatin1(method.name());
    }
    callable.sort();

    // The contract, and nothing else. requestClear() is Q_INVOKABLE for the app
    // side and is a view-only operation; the rest is this pane's own input,
    // geometry, visibility, mount handshake and output-consumption report. No
    // attach, no kill, no detach, no tmux target, no working directory, no
    // session id.
    QCOMPARE(callable,
             QStringList({QStringLiteral("notifyOutputConsumed"),
                          QStringLiteral("notifyViewVisible"), QStringLiteral("ready"),
                          QStringLiteral("requestClear"), QStringLiteral("resize"),
                          QStringLiteral("sendInput")}));

    // Nor can the page read one back out and act on it: the properties are the
    // renderer's own view state.
    QStringList properties;
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i)
        properties << QString::fromLatin1(meta->property(i).name());
    properties.sort();
    QCOMPARE(properties,
             QStringList({QStringLiteral("columns"), QStringLiteral("connectionState"),
                          QStringLiteral("rows")}));
}

// The TerminalBridge slots the page calls, each landing on the controller:
// keystrokes on the transport, geometry recorded, visibility toggled. (The
// fourth, notifyOutputConsumed(), has its own test below.)
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

// "The controller may emit output" means "a renderer is listening to write()",
// and only the mount handshake proves that. The QML pane
// (src/qml/TerminalPaneView.qml onVisibleChanged) reports the ITEM's
// visibility, and it can speak long before Chromium has finished loading the
// bundle — a pane hidden by a Dev Session switch and shown again while the page
// is still loading reports "visible" with no renderer behind it. Taking that at
// face value emits the whole first screenful tmux drew at a page that has no
// handler attached yet, and those bytes are gone: they are not in the rolling
// buffer either, because the controller already handed them over. The user is
// left staring at a blank terminal until they press a key.
void TstTerminalFactory::bridgeIgnoresAVisibleReportFromAPaneWithNoRenderer()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    QSignalSpy writes(bridge, &TerminalBridge::write);

    QVERIFY(!bridge->rendererReady());
    bridge->notifyViewVisible(false);
    bridge->notifyViewVisible(true);
    QVERIFY(!controller->viewVisible());

    controller->ingestOutput(QByteArrayLiteral("first screenful"));
    QTRY_COMPARE(controller->hiddenBuffer(), QByteArrayLiteral("first screenful"));
    QCOMPARE(writes.count(), 0);

    // The handshake is the only thing that may release the buffer.
    bridge->ready();
    QVERIFY(controller->viewVisible());
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(0).toString(), QStringLiteral("first screenful"));
}

// The other side of the same rule, in the two shapes production produces:
// a pane the user hid, and a page that reloaded under a pane that never moved.
void TstTerminalFactory::bridgeRetainsOutputForAHiddenPaneAndAcrossAPageReload()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    bridge->ready();
    QVERIFY(controller->viewVisible());
    QSignalSpy writes(bridge, &TerminalBridge::write);

    // (1) A mounted renderer that is not on screen: retain, then replay once.
    bridge->notifyViewVisible(false);
    QVERIFY(!controller->viewVisible());
    controller->ingestOutput(QByteArrayLiteral("while hidden"));
    QTRY_COMPARE(controller->hiddenBuffer(), QByteArrayLiteral("while hidden"));
    QCOMPARE(writes.count(), 0);

    bridge->notifyViewVisible(true);
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(0).toString(), QStringLiteral("while hidden"));

    // (2) A reload. The outgoing document reports hidden on its way out
    // (TerminalHost.dispose() in src/web/terminal/src/index.ts) and the fresh
    // one announces itself with ready(). The handshake must OVERRIDE the
    // outgoing page's last word, or the pane would retain output forever behind
    // a renderer that is plainly on screen.
    bridge->notifyViewVisible(false);
    controller->ingestOutput(QByteArrayLiteral("across the reload"));
    QTRY_COMPARE(controller->hiddenBuffer(), QByteArrayLiteral("across the reload"));
    QCOMPARE(writes.count(), 1);

    bridge->ready();
    QVERIFY(controller->viewVisible());
    QCOMPARE(writes.count(), 2);
    QCOMPARE(writes.at(1).at(0).toString(), QStringLiteral("across the reload"));
}

// The bridge holds its controller weakly because the two can be destroyed in
// either order. Everything the PAGE can call is reachable for as long as the
// WebChannel object lives, so each entry point has to be inert rather than a
// crash once the controller is gone.
void TstTerminalFactory::bridgeIsInertOnceItsControllerIsDestroyed()
{
    TerminalFactory factory(nullptr);
    QObject pane;
    // A separate owner, so the controller can die while the bridge lives.
    auto* controllerOwner = new QObject;
    TerminalController* controller = factory.create(controllerOwner);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    QCOMPARE(bridge->controller(), controller);

    delete controllerOwner;
    QVERIFY(bridge->controller() == nullptr);

    QSignalSpy writes(bridge, &TerminalBridge::write);
    QSignalSpy states(bridge, &TerminalBridge::connectionStateChanged);
    QSignalSpy geometry(bridge, &TerminalBridge::geometryChanged);
    QSignalSpy cleared(bridge, &TerminalBridge::clearRequested);

    bridge->sendInput(QStringLiteral("ls\n"));
    bridge->resize(120, 40);
    bridge->notifyViewVisible(false);
    bridge->notifyViewVisible(true);
    bridge->ready();
    bridge->requestClear();

    QCOMPARE(bridge->columns(), 0);
    QCOMPARE(bridge->rows(), 0);
    QCOMPARE(bridge->connectionState(), toString(TerminalState::Unloaded));
    QCOMPARE(writes.count(), 0);
    // Nothing to report a transition for, and no geometry was recorded.
    QCOMPARE(states.count(), 0);
    QCOMPARE(geometry.count(), 0);
    // requestClear() is a pure view operation and needs no controller at all.
    QCOMPARE(cleared.count(), 1);
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
    // The two bytes of the batch that decoded to nothing are carried onto this
    // one, so the page can acknowledge all eight bytes the controller charged
    // against its flow-control window. Losing them would leak the controller's
    // credit away one truncated glyph at a time until the pane stopped
    // receiving output altogether.
    QCOMPARE(writes.at(0).at(1).toInt(), 8);
}

// The flow-control loop across the bridge: what write() advertises as the byte
// weight of a batch is what the page hands back, and handing it back is what
// releases the output the controller retained.
void TstTerminalFactory::bridgeCarriesByteWeightAndFeedsAcknowledgementsBack()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    bridge->ready();
    QSignalSpy writes(bridge, &TerminalBridge::write);

    // Run the renderer's credit down to zero without acknowledging anything.
    const QByteArray chunk(TerminalController::kFlushSizeBytes, 'A');
    const int batches =
        TerminalController::kMaxUnacknowledgedBytes / TerminalController::kFlushSizeBytes;
    for (int i = 0; i < batches; ++i)
        controller->ingestOutput(chunk);
    QCOMPARE(writes.count(), batches);
    // Every batch advertised its own byte weight, which is what the page echoes.
    QCOMPARE(writes.at(0).at(1).toInt(), TerminalController::kFlushSizeBytes);

    // Past the window the pane retains instead of emitting at a renderer that
    // is demonstrably not keeping up.
    controller->ingestOutput(QByteArrayLiteral("held back"));
    QTRY_COMPARE(controller->hiddenBuffer(), QByteArrayLiteral("held back"));
    QCOMPARE(writes.count(), batches);

    // The page reports it consumed one batch; the retained bytes are released.
    bridge->notifyOutputConsumed(TerminalController::kFlushSizeBytes);
    QCOMPARE(writes.count(), batches + 1);
    QCOMPARE(writes.at(batches).at(0).toString(), QStringLiteral("held back"));
    QCOMPARE(writes.at(batches).at(1).toInt(), 9);
    QVERIFY(controller->hiddenBuffer().isEmpty());

    // A renderer that DIES without reporting hidden — a crash, or a navigation
    // its pagehide handler did not survive — leaves the controller believing a
    // renderer is there and owing a full window. The replacement's handshake
    // must clear that, or the new page would sit blank forever behind a debt it
    // can never pay: it is not a visibility CHANGE, so nothing else would.
    for (int i = 0; i < batches; ++i)
        controller->ingestOutput(chunk);
    controller->ingestOutput(QByteArrayLiteral("stranded"));
    QTRY_VERIFY(controller->hiddenBuffer().endsWith(QByteArrayLiteral("stranded")));
    const QByteArray stranded = controller->hiddenBuffer();

    const int before = writes.count();
    bridge->ready(); // the replacement page mounts; no hidden report preceded it
    QCOMPARE(writes.count(), before + 1);
    QCOMPARE(writes.at(before).at(0).toString(), QString::fromUtf8(stranded));
    QVERIFY(controller->hiddenBuffer().isEmpty());
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

// A pane whose attach never produces a byte is bounded by the controller (see
// tst_terminalcontroller silentAttachIsBoundedAndReportedAsAnError), but "error"
// on its own tells the user nothing. The factory owns the only message channel
// the pane's chrome reads (src/qml/TerminalPaneView.qml binds factory.onError),
// so the reason has to come out of there, once per stall and against the right
// pane.
void TstTerminalFactory::attachStallIsReportedAsAPaneMessage()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    // A second pane on the same factory: a stall on one must not be reported on
    // the other.
    TerminalController* quiet = factory.create(&pane);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    controller->setAttachTimeoutMs(50);
    controller->setState(TerminalState::AttachingTmux);

    QVERIFY(errors.wait(2000));
    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.at(0).at(0).value<TerminalController*>(), controller);
    QVERIFY(!errors.at(0).at(1).toString().isEmpty());
    QVERIFY(controller->state() == TerminalState::Error);
    QVERIFY(quiet->state() == TerminalState::Unloaded);

    // Retrying the attach and stalling again reports exactly once more: the
    // handler is installed per controller, not per attach, so a pane that
    // reconnects repeatedly cannot end up shouting the same stall N times.
    controller->setState(TerminalState::AttachingTmux);
    QTRY_COMPARE(errors.count(), 2);
    QTest::qWait(200);
    QCOMPARE(errors.count(), 2);
}

QTEST_GUILESS_MAIN(TstTerminalFactory)
#include "tst_terminalfactory.moc"
