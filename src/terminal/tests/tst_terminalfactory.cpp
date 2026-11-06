// Unit gate for the per-pane terminal plumbing (SPEC 5.1-5.3): ch::TerminalFactory
// (controller/bridge minting, ownership, attach refusal, kill command) and
// ch::TerminalBridge (the WebChannel face the xterm.js page talks to).
//
// Almost nothing here needs a server: attaching a PTY is the live gate's job
// (tst_liveterminalfactory). What IS covered here is everything that must hold
// with no connection at all — including the refusal itself, which is the state
// a real user hits first — plus, at the end, the rules deciding which answer a
// pane may adopt as its agent-status identity, which run against a real
// workspace repository over a socket pair.

#include <QtTest/QtTest>

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <limits>
#include <optional>

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "Ids.h"
#include "RpcTypes.h"
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

class PrefixThenFailTransport : public QIODevice {
public:
    explicit PrefixThenFailTransport(qint64 failure) : m_failure(failure)
    {
        open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    }
    const QByteArray& written() const { return m_written; }

protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 size) override
    {
        constexpr qint64 prefixBytes = 3;
        if (m_written.size() >= prefixBytes)
            return m_failure;
        const qint64 accepted = qMin(size, prefixBytes - m_written.size());
        m_written.append(data, accepted);
        return accepted;
    }

private:
    QByteArray m_written;
    qint64 m_failure;
};

// resolveTarget() refuses unless the SSH pool reports Connected, and a pool
// cannot reach that state without a real handshake against a real server. That
// one gate is what kept every rule about which answer a pane may adopt as its
// identity out of reach of a unit test. This subclass opens exactly that door
// and changes nothing else: the resolution, the waiting lists, the caching and
// the identity adoption below are all the production code.
class OfflineFactory : public TerminalFactory {
public:
    using TerminalFactory::TerminalFactory;
    bool connected() const override { return true; }
};

// The SECOND environmental gate, opened for the same reason and no wider.
// attach() builds a real ch::SshChannelDevice, and startPty() cannot succeed
// without libssh and a live session, so everything the production attach path
// does AFTER a successful open was unreachable from a unit test — including the
// recreated-session diagnostic, which is precisely the code that decides whether
// a user is told their work is gone. The override answers "the channel came up"
// and changes nothing else; the pane simply never receives any bytes on it.
class AttachingFactory : public OfflineFactory {
public:
    using OfflineFactory::OfflineFactory;
    int openCount = 0;
    QString lastCommand;

protected:
    bool openPty(SshChannelDevice*, int, int, const QString& command) override
    {
        ++openCount;
        lastCommand = command;
        return true;
    }
};

QByteArray jsonLine(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

// A real ch::WorkspaceDb on a real ch::CodeharbordClient over a socket pair,
// the arrangement tst_workspacedb uses. No mock: the test plays the server, so
// it decides WHEN each answer arrives, which is the whole point — "retarget the
// pane while a lookup is in flight" needs a lookup that can be held open.
class RpcPair {
public:
    bool start()
    {
        static int seq = 0;
        const QString name = QStringLiteral("ch_tf_ag_%1_%2")
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(++seq);
        QLocalServer::removeServer(name);
        if (!m_server.listen(name))
            return false;
        m_clientSide.connectToServer(name);
        if (!m_clientSide.waitForConnected(2000))
            return false;
        if (!m_server.waitForNewConnection(2000))
            return false;
        m_serverSide = m_server.nextPendingConnection();
        if (!m_serverSide)
            return false;
        m_client.setTransport(&m_clientSide);
        return true;
    }

    WorkspaceDb* db() { return &m_db; }
    // The same peer the repository speaks over. ch::TerminalFactory's
    // recreated-session diagnostic is a `tmux.*` call, which is not the
    // workspace group, so it is asked over the client directly.
    CodeharbordClient* client() { return &m_client; }

    // The next request frame the client wrote, or an empty object if none
    // arrived. Frames are buffered because two lookups can be in flight and the
    // socket may hand both over in one read. `timeoutMs` is short for the tests
    // that assert NOTHING was asked.
    QJsonObject takeRequest(int timeoutMs = 2000)
    {
        m_clientSide.flush();
        QDeadlineTimer deadline(timeoutMs);
        while (!m_pending.contains('\n') && !deadline.hasExpired()) {
            if (m_serverSide->bytesAvailable() > 0 || m_serverSide->waitForReadyRead(50))
                m_pending += m_serverSide->readAll();
        }
        const qsizetype newline = m_pending.indexOf('\n');
        if (newline < 0)
            return {};
        const QByteArray line = m_pending.left(newline);
        m_pending.remove(0, newline + 1);
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        m_requestServers.insert(
            request.value(QStringLiteral("id")).toInt(),
            request.value(QStringLiteral("params")).toObject()
                .value(QStringLiteral("serverId")).toString());
        return request;
    }

    // Answer request `id` with one `terminal_panes` row, in the wire shape
    // remote/src/workspace.ts returns. Server ownership comes from the captured
    // request, not the factory's current peer; an explicit override still lets
    // a negative test construct a foreign or malformed reply.
    void answerWithRow(int id, const QString& rowId, const QString& devSessionId,
                       const QString& name, const QString& tmuxTarget,
                       const QString& harness, std::optional<QString> serverId = std::nullopt)
    {
        const QJsonObject row{
            {QStringLiteral("id"), rowId},
            {QStringLiteral("serverId"), serverId.value_or(m_requestServers.value(id))},
            {QStringLiteral("devSessionId"), devSessionId},
            {QStringLiteral("name"), name},
            {QStringLiteral("workingDirectory"), QStringLiteral("/repo")},
            {QStringLiteral("tmuxTarget"), tmuxTarget},
            {QStringLiteral("startupCommand"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("harness"),
             harness.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(harness)},
            {QStringLiteral("position"), 0},
        };
        write({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
               {QStringLiteral("id"), id},
               {QStringLiteral("result"), row}});
    }

    void answerWithError(int id, const QString& message)
    {
        write({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
               {QStringLiteral("id"), id},
               {QStringLiteral("error"),
                QJsonObject{{QStringLiteral("code"), -32000},
                            {QStringLiteral("message"), message}}}});
    }

    // Answer request `id` with a `tmux.listSessions` listing, in the wire shape
    // remote/src/tmux.ts returns: the TmuxSession array from
    // remote/src/rpc-types.ts, `created` being tmux's session_created as a UNIX
    // timestamp in SECONDS.
    void answerWithSessions(int id, const QJsonArray& sessions)
    {
        write({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
               {QStringLiteral("id"), id},
               {QStringLiteral("result"), sessions}});
    }

private:
    void write(const QJsonObject& frame)
    {
        m_serverSide->write(jsonLine(frame));
        m_serverSide->flush();
    }

    QLocalServer m_server;
    QLocalSocket m_clientSide;
    QLocalSocket* m_serverSide = nullptr;  // owned by m_server
    CodeharbordClient m_client;
    WorkspaceDb m_db{&m_client};
    QByteArray m_pending;
    QHash<int, QString> m_requestServers;
};

int asInt(AgentState state) { return static_cast<int>(state); }

// One entry of a `tmux.listSessions` answer (remote/src/rpc-types.ts
// TmuxSession). `created` is tmux's session_created: SECONDS since the epoch.
QJsonObject tmuxSession(const QString& name, qint64 created)
{
    return QJsonObject{{QStringLiteral("name"), name},
                       {QStringLiteral("windows"), 1},
                       {QStringLiteral("created"), created},
                       {QStringLiteral("attached"), true}};
}

// Bring `controller` up on a server-minted target the way production does:
// resolve the pane's row, let the factory adopt the answer, then attach. Returns
// false if any step did not happen, so a test cannot pass by never attaching.
bool resolveAndAttach(TerminalFactory& factory, RpcPair& rpc,
                      TerminalController* controller, const QString& target,
                      const QString& harness = QString())
{
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    if (!factory.resolveTarget(controller, QStringLiteral("s1"),
                               QStringLiteral("terminal-1"), QStringLiteral("row-A"),
                               QStringLiteral("/repo")))
        return false;
    const QJsonObject request = rpc.takeRequest();
    if (request.value(QStringLiteral("method")).toString()
        != QString::fromLatin1(rpc::kMethodWorkspaceResolveTerminalPane))
        return false;
    rpc.answerWithRow(request.value(QStringLiteral("id")).toInt(), QStringLiteral("row-A"),
                      QStringLiteral("s1"), QStringLiteral("terminal-1"), target, harness);
    if (!resolved.wait(2000) || resolved.constFirst().at(1).toString() != target)
        return false;
    return factory.attach(controller, target, QStringLiteral("/repo"), 80, 24);
}

// Answer the `tmux.listSessions` probe that every attach now asks, reporting
// `target` with tmux's `session_created` set to `created`. A NEGATIVE `created`
// answers a listing that does not mention `target` at all — what a host says
// when no such session exists, which is also what a channel that came up
// without ever getting a tmux session out of it leaves behind. Returns false if
// the request was not the probe, so a test cannot pass by never being asked.
bool answerSessionProbe(RpcPair& rpc, const QString& target, qint64 created)
{
    const QJsonObject probe = rpc.takeRequest();
    if (probe.value(QStringLiteral("method")).toString()
        != QString::fromLatin1(rpc::kMethodListSessions))
        return false;
    // Somebody else's session is always in the listing: the verdict must come
    // from the entry that NAMES this pane's target and from nothing else.
    QJsonArray listing{tmuxSession(QStringLiteral("ch_s1_somebody-else"), 1000)};
    if (created >= 0)
        listing.append(tmuxSession(target, created));
    rpc.answerWithSessions(probe.value(QStringLiteral("id")).toInt(), listing);
    return true;
}

} // namespace

class TstTerminalFactory : public QObject {
    Q_OBJECT

private slots:
    void createParentsTheControllerToThePane();
    void createOwnerlessControllerIsAdoptedByTheFactory();
    void createBridgeWrapsTheControllerAndDiesWithThePane();
    void attachWithoutAConnectionFailsAndReportsWhy();
    void attachRejectsTargetNotResolvedByServer();
    void attachWithoutATargetIsRefused();
    void attachRefusesATargetTmuxWouldReadAsAnIdRatherThanAName();
    void attachObserversCanCancelOrReplaceTheOpeningChannel_data();
    void attachObserversCanCancelOrReplaceTheOpeningChannel();
    void resolveTargetWithoutAServerRefuses();
    void workspaceIdentityMustBeAcceptedBeforeUsingAConnectedTransport();
    void unacceptedReconnectCannotCompleteThePreviousIdentityLookup();
    void anIdentityBoundaryDuringAttachCancelsThatAttempt();
    void newTmuxChildrenUseCurrentEndpointsWithoutInheritedCapabilities();
    void paneKeysAddressOneSlotOfOneDevSession();
    void resolveAddressesAPaneByItsRowIdAndNeverByItsLabel();
    void rowResolutionRetainsItsDevSessionScope_data();
    void rowResolutionRetainsItsDevSessionScope();
    void detachAndKillWithoutAnAttachmentAreNoOps();
    void killCommandQuotesAdversarialTargets();
    void attachCommandQuotesAdversarialIdsAndWorkingDir();
    void bridgeClampsAbsurdGeometryFromThePage();
    void bridgeExposesNoRemoteTargetingSlots();
    void bridgeForwardsInputResizeAndVisibility();
    // The byte-safe input slot: bytes the page could not send as text (an X10
    // mouse report) must reach the PTY unchanged, and a payload that is not
    // valid base64 must reach it not at all.
    void bridgeDecodesBinaryInputAndRefusesMalformedBase64();
    void bridgeReportsPartialInputWithoutDisconnectingOrReplaying_data();
    void bridgeReportsPartialInputWithoutDisconnectingOrReplaying();
    void queuedInputCannotCrossATransportReplacement_data();
    void queuedInputCannotCrossATransportReplacement();
    void bridgeHoldsOutputUntilTheRendererIsReady();
    void bridgeIgnoresAVisibleReportFromAPaneWithNoRenderer();
    void aHiddenReplacementMountNeverReleasesRetainedOutput_data();
    void aHiddenReplacementMountNeverReleasesRetainedOutput();
    void bridgePreservesHiddenReportBeforeReady();
    void bridgeRetainsOutputForAHiddenPaneAndAcrossAPageReload();
    void bridgeIsInertOnceItsControllerIsDestroyed();
    void bridgeDestructorHidesItsController();
    void bridgeDecodesUtf8SplitAcrossFlushes();
    void bridgeStartsAFreshDecodeForAReplacementRenderer();
    void bridgeCarriesByteWeightAndFeedsAcknowledgementsBack();
    void bridgeReportsStateTransitionsAsStrings();
    void bridgeClearIsAViewOnlyRequest();
    void attachStallIsReportedAsAPaneMessage();

    // AG-N1: which answer a pane may adopt as its agent-status identity.
    void aPaneReportsItsOutputUnderTheRowIdTheServerAnswered();
    void aCachedLegacyAnswerStillReportsItsRowSoTheBackfillCanBeRetried();
    void aPaneRetargetedMidLookupNeverReportsUnderTheSupersededPanesIdentity();
    void supersededResolutionErrorsDoNotReplaceTheCurrentAnswer();
    void detachCancelsPendingResolution_data();
    void detachCancelsPendingResolution();
    void cancellingOldServerFlightsDoesNotCancelReentrantNewRequests();
    void aFailedResolutionLeavesThePaneWithNoIdentity();
    void targetCannotCrossAWorkspaceServerSwitch();
    void oldResolutionsCannotCompleteANewServerRequest_data();
    void oldResolutionsCannotCompleteANewServerRequest();
    void aServerSwitchDuringResolutionDeliveryCancelsTheOtherWaiters();
    void aServerAnswerWithAnUnusableTmuxTargetFailsTheResolution();
    void changingTheWorkspaceServerForgetsRememberedRowIdentities();
    void aChangedHarnessSurvivesTheResolutionCache();
    void repeatingAnUnchangedHarnessDoesNotRestartTheClock();
    void outputDuringChannelOpenIsNotResetToStarting();
    void endedTerminalObservationAcceptsRemoteActivity_data();
    void endedTerminalObservationAcceptsRemoteActivity();

    // A pane whose remote session died and was silently replaced.
    void aFirstAttachStaysSilentEvenWhenItCreatedTheSession();
    void aReattachThatCreatedTheSessionSaysTheOldOneIsGone();
    void anOrdinaryReconnectToASessionThatWasStillThereStaysSilent();
    void aFailedOrEmptyListingReportsNothingAndRaisesNoError();
    void aRetargetWhileTheProbeIsInFlightReportsNothing();
    void anOlderAttachmentProbeCannotReportForANewerChannel();
    void aPaneThatNeverHadASessionStaysSilentWhenOneIsFinallyCreated();
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

// The owner parameter defaults, and a defaulted call must still produce an
// OWNED object. create() is a Q_INVOKABLE: an unparented QObject returned to
// QML is handed JavaScriptOwnership and collected whenever the engine feels
// like it (taking a live pane's flush timer, buffers and transport connections
// with it), and a C++ caller that took the default would simply leak it.
// Neither outcome is a decision this factory should leave to its caller, so it
// adopts the controller itself.
void TstTerminalFactory::createOwnerlessControllerIsAdoptedByTheFactory()
{
    QPointer<TerminalController> orphan;
    {
        TerminalFactory factory(nullptr);
        orphan = factory.create();

        QVERIFY(!orphan.isNull());
        QCOMPARE(orphan->parent(), &factory);
    }
    // ...and so it is destroyed, rather than left behind with nobody holding it.
    QVERIFY(orphan.isNull());
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

void TstTerminalFactory::attachObserversCanCancelOrReplaceTheOpeningChannel_data()
{
    QTest::addColumn<TerminalState>("phase");
    QTest::addColumn<bool>("replace");
    QTest::newRow("cancel-opening") << TerminalState::OpeningChannel << false;
    QTest::newRow("replace-opening") << TerminalState::OpeningChannel << true;
    QTest::newRow("cancel-attaching") << TerminalState::AttachingTmux << false;
    QTest::newRow("replace-attaching") << TerminalState::AttachingTmux << true;
}

void TstTerminalFactory::attachObserversCanCancelOrReplaceTheOpeningChannel()
{
    QFETCH(TerminalState, phase);
    QFETCH(bool, replace);
    AttachingFactory factory(nullptr);
    TerminalController* controller = factory.create();
    bool acted = false;
    bool replacementAttached = false;
    QPointer<QIODevice> openingTransport;
    connect(controller, &TerminalController::stateChanged, controller,
            [&](TerminalState state) {
                if (acted || state != phase)
                    return;
                acted = true;
                openingTransport = controller->transport();
                if (replace)
                    replacementAttached = factory.attach(
                        controller, QStringLiteral("ch_replacement"), QString(), 100, 30);
                else
                    factory.detach(controller);
            });
    QVERIFY(!factory.attach(controller, QStringLiteral("ch_original"), QString(), 80, 24));
    QVERIFY(acted);
    QVERIFY(openingTransport);
    const int originalOpens = phase == TerminalState::OpeningChannel ? 0 : 1;
    QCOMPARE(factory.openCount, originalOpens + (replace ? 1 : 0));
    if (replace) {
        QVERIFY(replacementAttached);
        QVERIFY(controller->transport());
        QVERIFY(controller->transport() != openingTransport);
        QCOMPARE(factory.targetFor(controller), QStringLiteral("ch_replacement"));
        // The retiring attach must not replace the attachment table's live
        // device with its old channel after its state notification returns.
        factory.detach(controller);
    }
    QVERIFY(!controller->transport());
}

// A target supplied by a QML caller is not trusted merely because it is
// non-empty. Once the workspace repository is configured, attach() accepts only
// the target returned by resolveTarget(), so a stale or compromised caller
// cannot attach this pane to another session.
void TstTerminalFactory::attachRejectsTargetNotResolvedByServer()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    QVERIFY(!factory.attach(controller, QStringLiteral("client-made-target"),
                            QStringLiteral("/repo"), 80, 24));
    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.at(0).at(0).value<TerminalController*>(), controller);
    QVERIFY(errors.at(0).at(1).toString().contains(QStringLiteral("server")));
    QVERIFY(factory.targetFor(controller).isEmpty());
    QVERIFY(controller->state() == TerminalState::Unloaded);
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

// A target is not just a string that gets quoted; it is TMUX GRAMMAR. tmux
// resolves an ID sigil before it looks a name up, and the `=` exact-match prefix
// every call site uses does not suppress that: with a session `victim` holding
// id `$0` and a second session literally named `$0`, `tmux kill-session -t '=$0'`
// destroys `victim` (verified on tmux 3.6). attach() puts the target in two such
// positions — `set-option -t '=<target>:'`, and `new-session -s <target>` where
// it is not even shielded — so a name tmux reads as something else has to be
// refused before any of that is built.
void TstTerminalFactory::attachRefusesATargetTmuxWouldReadAsAnIdRatherThanAName()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    // Every shape tmux would read as something other than this name.
    const QStringList unusable{
        QStringLiteral("$0"),          // session id
        QStringLiteral("@1"),          // window id
        QStringLiteral("%2"),          // pane id
        QStringLiteral("ch_*_t1"),     // fnmatch wildcard
        QStringLiteral("=ch_s1_row"),  // the exact-match prefix itself
        QStringLiteral("ch_s1:0"),     // session/window separator
        QStringLiteral("-d"),          // an option to `new-session -s`
    };
    int seen = 0;
    for (const QString& target : unusable) {
        QVERIFY2(!factory.attach(controller, target, QStringLiteral("/repo"), 80, 24),
                 qPrintable(target));
        QCOMPARE(errors.count(), ++seen);
        QCOMPARE(errors.at(seen - 1).at(0).value<TerminalController*>(), controller);
        // The pane says which name it refused, so the user is not left with a
        // silent dead terminal.
        QVERIFY2(errors.at(seen - 1).at(1).toString().contains(target), qPrintable(target));
        // And nothing was recorded: kill() reads targetFor(), so a refused
        // target must never become something this pane could later destroy.
        QVERIFY(factory.targetFor(controller).isEmpty());
        QVERIFY(controller->state() == TerminalState::Unloaded);
        QVERIFY(!controller->transport());
    }

    // The gate is about the GRAMMAR and nothing else: a well-formed target gets
    // past it and is refused one check later, for not having been resolved by
    // the server. Two different refusals, so this test cannot pass by accident.
    QVERIFY(!factory.attach(controller, QStringLiteral("ch_s1_row-A"),
                            QStringLiteral("/repo"), 80, 24));
    QCOMPARE(errors.count(), seen + 1);
    QVERIFY(errors.at(seen).at(1).toString().contains(QStringLiteral("resolved")));
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

void TstTerminalFactory::workspaceIdentityMustBeAcceptedBeforeUsingAConnectedTransport()
{
    RpcPair rpc;
    QVERIFY(rpc.start());
    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    TerminalController* controller = factory.create();
    QVERIFY(factory.connected());
    QVERIFY(!factory.workspaceReady());
    QVERIFY(!factory.resolveTarget(controller, QStringLiteral("s1"), QString(),
                                   QStringLiteral("row-A"), QString()));
    QVERIFY(!factory.attach(controller, QStringLiteral("ch_s1_row-A"), QString(), 80, 24));
    QCOMPARE(factory.openCount, 0);
    QVERIFY(rpc.takeRequest(50).isEmpty());

    factory.setWorkspaceReady(true);
    QVERIFY(resolveAndAttach(factory, rpc, controller, QStringLiteral("ch_s1_row-A")));
    controller->setViewVisible(false);
    controller->ingestOutput(QByteArrayLiteral("retained screen"));
    QTRY_COMPARE(controller->hiddenBuffer(), QByteArrayLiteral("retained screen"));
    factory.detach(controller);
    factory.setWorkspaceReady(false);
    QVERIFY(!factory.resolveTarget(controller, QStringLiteral("s1"), QString(),
                                   QStringLiteral("row-A"), QString()));
    QVERIFY(!factory.attach(controller, QStringLiteral("ch_s1_row-A"), QString(), 80, 24));
    factory.kill(controller);
    QCOMPARE(factory.targetFor(controller), QStringLiteral("ch_s1_row-A"));
    QCOMPARE(controller->hiddenBuffer(), QByteArrayLiteral("retained screen"));
    QCOMPARE(factory.openCount, 1);
    QVERIFY(rpc.takeRequest(50).isEmpty());

    // The id does not change on an ordinary reconnect. Only the explicit
    // accepted-identity handoff may reopen the cached attachment path.
    factory.setServerId(QStringLiteral("srv-1"));
    QVERIFY(!factory.workspaceReady());
    factory.setWorkspaceReady(true);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"), QString(),
                                  QStringLiteral("row-A"), QString()));
    QTRY_COMPARE(resolved.count(), 1);
    QCOMPARE(resolved.first().at(1).toString(), QStringLiteral("ch_s1_row-A"));
    QVERIFY(rpc.takeRequest(50).isEmpty());
    QVERIFY(factory.attach(controller, QStringLiteral("ch_s1_row-A"), QString(), 80, 24));
    QCOMPARE(factory.targetFor(controller), QStringLiteral("ch_s1_row-A"));
    QCOMPARE(controller->hiddenBuffer(), QByteArrayLiteral("retained screen"));
}

void TstTerminalFactory::unacceptedReconnectCannotCompleteThePreviousIdentityLookup()
{
    RpcPair rpc;
    QVERIFY(rpc.start());
    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    TerminalController* controller = factory.create();
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"), QString(),
                                  QStringLiteral("row-A"), QString()));
    const QJsonObject oldRequest = rpc.takeRequest();
    factory.setWorkspaceReady(false);
    rpc.answerWithRow(oldRequest.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("row-A"), QStringLiteral("s1"), QString(),
                      QStringLiteral("ch_old_answer"), QString());
    QTRY_COMPARE(rpc.client()->pendingCount(), 0);
    QCOMPARE(resolved.count(), 0);
    factory.setWorkspaceReady(true);
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"), QString(),
                                  QStringLiteral("row-A"), QString()));
    const QJsonObject currentRequest = rpc.takeRequest();
    QVERIFY(!currentRequest.isEmpty());
    rpc.answerWithRow(currentRequest.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("row-A"), QStringLiteral("s1"), QString(),
                      QStringLiteral("ch_current_answer"), QString());
    QTRY_COMPARE(resolved.count(), 1);
    QCOMPARE(resolved.first().at(1).toString(), QStringLiteral("ch_current_answer"));
}

void TstTerminalFactory::anIdentityBoundaryDuringAttachCancelsThatAttempt()
{
    RpcPair rpc;
    QVERIFY(rpc.start());
    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    TerminalController* controller = factory.create();
    QVERIFY(resolveAndAttach(factory, rpc, controller, QStringLiteral("ch_s1_row-A")));
    bool changedIdentity = false;
    connect(controller, &TerminalController::stateChanged, controller,
            [&](TerminalState state) {
                if (changedIdentity || state != TerminalState::OpeningChannel)
                    return;
                changedIdentity = true;
                factory.setWorkspaceReady(false);
                factory.setWorkspaceReady(true);
            });
    QVERIFY(!factory.attach(controller, QStringLiteral("ch_s1_row-A"), QString(), 80, 24));
    QVERIFY(changedIdentity);
    QVERIFY(!controller->transport());
    QCOMPARE(factory.openCount, 1);
    QVERIFY(factory.attach(controller, QStringLiteral("ch_s1_row-A"), QString(), 80, 24));
    QCOMPARE(factory.openCount, 2);
}

void TstTerminalFactory::newTmuxChildrenUseCurrentEndpointsWithoutInheritedCapabilities()
{
#if !defined(Q_OS_LINUX)
    QSKIP("the isolated real-tmux PTY fixture requires Linux script(1)");
#else
    const QString tmux = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    const QString script = QStandardPaths::findExecutable(QStringLiteral("script"));
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (tmux.isEmpty() || script.isEmpty() || node.isEmpty())
        QSKIP("tmux, script and node are required for the private endpoint regression");
    QTemporaryDir fixture(QDir::temp().filePath(QStringLiteral("ntc9-XXXXXX")));
    QVERIFY(fixture.isValid());
    const QString privateTmux = fixture.filePath(QStringLiteral("tmux"));
    const QString masterRuntime = fixture.filePath(QStringLiteral("master-runtime"));
    const QString clientRuntime = fixture.filePath(QStringLiteral("client-runtime"));
    const QString masterHome = fixture.filePath(QStringLiteral("master-home"));
    const QString firstWorkingDirectory = fixture.filePath(QStringLiteral("cwd;"));
    const QString secondWorkingDirectory = fixture.filePath(QStringLiteral("cwd\\;"));
    QVERIFY(QDir().mkpath(firstWorkingDirectory));
    QVERIFY(QDir().mkpath(secondWorkingDirectory));
    QVERIFY(QDir().mkpath(fixture.filePath(QStringLiteral("cwd"))));
    const QString clientHome = fixture.filePath(QStringLiteral("client-home"));
    for (const QString& directory : {privateTmux, masterRuntime, clientRuntime,
                                    masterHome, clientHome})
        QVERIFY(QDir().mkpath(directory));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("TMUX"));
    environment.remove(QStringLiteral("TMUX_PANE"));
    environment.remove(QStringLiteral("NODE_OPTIONS"));
    environment.remove(QStringLiteral("BASH_ENV"));
    environment.remove(QStringLiteral("ENV"));
    environment.insert(QStringLiteral("TMUX_TMPDIR"), privateTmux);
    environment.insert(QStringLiteral("HOME"), masterHome);
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), masterRuntime);
    environment.insert(QStringLiteral("CODEHARBOR_AGENT_SOCKET"),
                       fixture.filePath(QStringLiteral("stale-master.sock")));
    environment.insert(QStringLiteral("CODEHARBOR_CONTROL_SOCKET"),
                       fixture.filePath(QStringLiteral("stale-window.sock")));
    environment.insert(QStringLiteral("OMP_DEV_SESSION_ID"), QStringLiteral("wrong-dev"));
    environment.insert(QStringLiteral("OMP_TERMINAL_ID"), QStringLiteral("wrong-terminal"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    QString diagnostic;
    QProcess attachment;
    attachment.setProcessChannelMode(QProcess::MergedChannels);
    const auto runTmux = [&](const QStringList& arguments) {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(tmux, arguments);
        if (!process.waitForStarted(5000) || !process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished(5000);
            diagnostic = QString::fromUtf8(process.readAll());
            return false;
        }
        diagnostic = QString::fromUtf8(process.readAll());
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };
    const auto cleanup = qScopeGuard([&]() {
        runTmux({QStringLiteral("kill-server")});
        if (attachment.state() != QProcess::NotRunning
            && !attachment.waitForFinished(5000)) {
            attachment.kill();
            attachment.waitForFinished(5000);
        }
    });
    QVERIFY2(runTmux({QStringLiteral("-f"), QStringLiteral("/dev/null"),
                      QStringLiteral("new-session"), QStringLiteral("-d"),
                      QStringLiteral("-s"), QStringLiteral("bootstrap"),
                      QStringLiteral("/bin/sleep"), QStringLiteral("60")}),
             qPrintable(diagnostic));
    QVERIFY(runTmux({QStringLiteral("set-option"), QStringLiteral("-g"),
                     QStringLiteral("update-environment"), QString()}));
    QVERIFY(runTmux({QStringLiteral("set-option"), QStringLiteral("-g"),
                     QStringLiteral("default-shell"), QStringLiteral("/bin/sh")}));
    QVERIFY(runTmux({QStringLiteral("set-window-option"), QStringLiteral("-g"),
                     QStringLiteral("remain-on-exit"), QStringLiteral("on")}));
    const QString helperPath = fixture.filePath(QStringLiteral("child.mjs"));
    const QString clearedPath = fixture.filePath(QStringLiteral("cleared"));
    QFile helper(helperPath);
    QVERIFY(helper.open(QIODevice::WriteOnly));
    const QByteArray helperSource = QByteArrayLiteral(R"JS(
import net from "node:net";
import { writeFileSync, renameSync } from "node:fs";
const inherited = {
    dev: process.env.OMP_DEV_SESSION_ID ?? null,
    terminal: process.env.OMP_TERMINAL_ID ?? null,
    agent: process.env.CODEHARBOR_AGENT_SOCKET ?? null,
    control: process.env.CODEHARBOR_CONTROL_SOCKET ?? null,
    runtime: process.env.XDG_RUNTIME_DIR,
    cwd: process.cwd()
};
if (!inherited.agent && !inherited.control) {
    writeFileSync(process.argv[2] + ".tmp", JSON.stringify(inherited));
    renameSync(process.argv[2] + ".tmp", process.argv[2]);
} else {
    for (const key of ["CODEHARBOR_AGENT_SOCKET", "CODEHARBOR_CONTROL_SOCKET"]) {
        if (!process.env[key]) continue;
        const socket = net.createConnection({ path: process.env[key] });
        socket.once("connect", () => socket.write(JSON.stringify({ ...inherited, key }) + "\n"));
        socket.once("data", () => socket.end());
        socket.once("error", () => { process.exitCode = 1; });
    }
}
)JS");
    QCOMPARE(helper.write(helperSource), qint64(helperSource.size()));
    helper.close();
    const auto quoted = [](QString value) {
        value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QLatin1Char('\'') + value + QLatin1Char('\'');
    };
    QVERIFY(runTmux({QStringLiteral("set-option"), QStringLiteral("-g"),
                     QStringLiteral("default-command"),
                     quoted(node) + QLatin1Char(' ') + quoted(helperPath)
                         + QLatin1Char(' ') + quoted(clearedPath)}));
    // The SSH-side client has a different HOME/XDG from the already-running
    // master. Neither is rewritten by the production command under test.
    environment.insert(QStringLiteral("HOME"), clientHome);
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), clientRuntime);
    QLocalServer firstEndpoint;
    QLocalServer secondEndpoint;
    QLocalServer firstControl;
    QLocalServer secondControl;
    QLocalServer staleAgent;
    QLocalServer staleWindow;
    QLocalServer truncatedFirstAgent;
    QLocalServer truncatedSecondAgent;
    QLocalServer truncatedFirstControl;
    QLocalServer truncatedSecondControl;
    const QString firstControlPath = fixture.filePath(QStringLiteral("window's endpoint\\;"));
    const QString secondControlPath = fixture.filePath(QStringLiteral("new-window;"));
    const QString firstPath = fixture.filePath(QStringLiteral("agent's endpoint;"));
    const QString secondPath = fixture.filePath(QStringLiteral("replacement\\;"));
    QVERIFY(firstEndpoint.listen(firstPath));
    QVERIFY(secondEndpoint.listen(secondPath));
    QVERIFY(firstControl.listen(firstControlPath));
    QVERIFY(secondControl.listen(secondControlPath));
    QVERIFY(staleAgent.listen(fixture.filePath(QStringLiteral("stale-master.sock"))));
    QVERIFY(staleWindow.listen(fixture.filePath(QStringLiteral("stale-window.sock"))));
    // These are the actual wrong endpoints produced by tmux's unescaped
    // trailing-semicolon grammar, including removal of one literal backslash.
    QVERIFY(truncatedFirstAgent.listen(firstPath.chopped(1)));
    QVERIFY(truncatedSecondAgent.listen(secondPath.left(secondPath.size() - 2) + QLatin1Char(';')));
    QVERIFY(truncatedFirstControl.listen(
        firstControlPath.left(firstControlPath.size() - 2) + QLatin1Char(';')));
    QVERIFY(truncatedSecondControl.listen(secondControlPath.chopped(1)));
    RpcPair rpc;
    QVERIFY(rpc.start());
    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    TerminalController* controller = factory.create();
    const QString target = QStringLiteral("ch_s1_row-A");
    const QString advertisedDev = QStringLiteral("s1;");
    const QString advertisedTerminal = QStringLiteral("row-A\\;");
    const auto attachPty = [&](const QString& command, const QString& barrier) {
        attachment.setProcessEnvironment(environment);
        attachment.start(script,
                         {QStringLiteral("-q"), QStringLiteral("-c"),
                          command + QStringLiteral(" \\; wait-for -S ") + barrier,
                          QStringLiteral("/dev/null")});
        return attachment.waitForStarted(5000)
            && runTmux({QStringLiteral("wait-for"), barrier});
    };
    for (int stage = 0; stage < 2; ++stage) {
        factory.setAgentSocket(stage == 0 ? firstPath : secondPath);
        factory.setControlSocket(stage == 0 ? firstControlPath : secondControlPath);
        if (stage == 0) {
            QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
            QVERIFY(factory.resolveTarget(controller, advertisedDev, QString(),
                                          advertisedTerminal, fixture.path()));
            const QJsonObject request = rpc.takeRequest();
            QVERIFY(!request.isEmpty());
            rpc.answerWithRow(request.value(QStringLiteral("id")).toInt(),
                              advertisedTerminal, advertisedDev, QString(),
                              target, QString());
            QTRY_COMPARE(resolved.count(), 1);
        }
        QVERIFY(factory.attach(controller, target,
                               stage == 0 ? firstWorkingDirectory : secondWorkingDirectory,
                               80, 24));
        QVERIFY2(attachPty(factory.lastCommand,
                           stage == 0 ? QStringLiteral("created") : QStringLiteral("refreshed")),
                 qPrintable(diagnostic + QString::fromUtf8(attachment.readAll())));
        if (stage != 0)
            QVERIFY(runTmux({QStringLiteral("new-window"), QStringLiteral("-t"),
                             QLatin1Char('=') + target + QLatin1Char(':')}));
        QLocalServer& endpoint = stage == 0 ? firstEndpoint : secondEndpoint;
        QLocalServer& control = stage == 0 ? firstControl : secondControl;
        for (QLocalServer* destination : {&endpoint, &control}) {
            QVERIFY2(destination->waitForNewConnection(5000),
                     qPrintable(QString::fromUtf8(attachment.readAll())));
            QLocalSocket* peer = destination->nextPendingConnection();
            QVERIFY(peer);
            QTRY_VERIFY_WITH_TIMEOUT(peer->canReadLine(), 5000);
            const QJsonObject event = QJsonDocument::fromJson(peer->readLine()).object();
            QCOMPARE(event.value(QStringLiteral("runtime")).toString(), masterRuntime);
            QCOMPARE(event.value(QStringLiteral("dev")).toString(), advertisedDev);
            QCOMPARE(event.value(QStringLiteral("terminal")).toString(), advertisedTerminal);
            QCOMPARE(event.value(QStringLiteral("agent")).toString(),
                     stage == 0 ? firstPath : secondPath);
            QCOMPARE(event.value(QStringLiteral("control")).toString(),
                     stage == 0 ? firstControlPath : secondControlPath);
            if (stage == 0)
                QCOMPARE(event.value(QStringLiteral("cwd")).toString(),
                         QDir(firstWorkingDirectory).canonicalPath());
            QCOMPARE(event.value(QStringLiteral("key")).toString(),
                     destination == &endpoint ? QStringLiteral("CODEHARBOR_AGENT_SOCKET")
                                              : QStringLiteral("CODEHARBOR_CONTROL_SOCKET"));
            peer->write("done");
            if (peer->bytesToWrite() > 0)
                QVERIFY(peer->waitForBytesWritten(5000));
        }
        QVERIFY(runTmux({QStringLiteral("detach-client"), QStringLiteral("-s"),
                         QLatin1Char('=') + target}));
        QVERIFY(attachment.waitForFinished(5000));
    }
    for (QLocalServer* wrongEndpoint : {&truncatedFirstAgent, &truncatedSecondAgent,
                                       &truncatedFirstControl, &truncatedSecondControl})
        QVERIFY(!wrongEndpoint->hasPendingConnections());
    // A missing endpoint must suppress even the stale global value installed
    // when the private master started, not expose it again through -u.
    factory.setAgentSocket(QString());
    factory.setControlSocket(QString());
    QVERIFY(factory.attach(controller, target, fixture.path(), 80, 24));
    QVERIFY(attachPty(factory.lastCommand, QStringLiteral("cleared")));
    QVERIFY(runTmux({QStringLiteral("new-window"), QStringLiteral("-t"),
                     QLatin1Char('=') + target + QLatin1Char(':')}));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(clearedPath), 5000);
    QVERIFY(!firstEndpoint.hasPendingConnections());
    QVERIFY(!secondEndpoint.hasPendingConnections());
    QVERIFY(!firstControl.hasPendingConnections());
    QVERIFY(!secondControl.hasPendingConnections());
    QVERIFY(!staleAgent.hasPendingConnections());
    QVERIFY(!staleWindow.hasPendingConnections());
    QFile cleared(clearedPath);
    QVERIFY(cleared.open(QIODevice::ReadOnly));
    const QJsonObject removed = QJsonDocument::fromJson(cleared.readAll()).object();
    QVERIFY(removed.value(QStringLiteral("agent")).isNull());
    QVERIFY(removed.value(QStringLiteral("control")).isNull());
    QCOMPARE(removed.value(QStringLiteral("dev")).toString(), advertisedDev);
    QCOMPARE(removed.value(QStringLiteral("terminal")).toString(), advertisedTerminal);
    cleared.close();
    QVERIFY(runTmux({QStringLiteral("detach-client"), QStringLiteral("-s"),
                     QLatin1Char('=') + target}));
    QVERIFY(attachment.waitForFinished(5000));
    struct UnknownIdentity {
        QString dev;
        QString terminal;
    };
    const UnknownIdentity unknownIdentities[]{
        {},
        {QStringLiteral("known-dev"), QString()},
        {QString(), QStringLiteral("known-terminal")},
    };
    for (int index = 0; index < 3; ++index) {
        QVERIFY(QFile::remove(clearedPath));
        const QString anonymousTarget = QStringLiteral("ch_unidentified_%1").arg(index);
        const QString literalDirectory = index == 1 ? secondWorkingDirectory : firstWorkingDirectory;
        const QString command = TerminalController::tmuxNewSessionCommand(
            anonymousTarget, literalDirectory, unknownIdentities[index].dev,
            unknownIdentities[index].terminal, firstControlPath, firstPath);
        QVERIFY(attachPty(command, QStringLiteral("anonymous_%1").arg(index)));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(clearedPath), 5000);
        QVERIFY(cleared.open(QIODevice::ReadOnly));
        const QJsonObject firstChild = QJsonDocument::fromJson(cleared.readAll()).object();
        cleared.close();
        QCOMPARE(firstChild.value(QStringLiteral("cwd")).toString(),
                 QDir(literalDirectory).canonicalPath());
        for (const QString& key : {QStringLiteral("dev"), QStringLiteral("terminal"),
                                   QStringLiteral("agent"), QStringLiteral("control")}) {
            QVERIFY(firstChild.value(key).isString());
            QVERIFY(firstChild.value(key).toString().isEmpty());
        }
        QVERIFY(QFile::remove(clearedPath));
        QVERIFY(runTmux({QStringLiteral("new-window"), QStringLiteral("-t"),
                         QLatin1Char('=') + anonymousTarget + QLatin1Char(':')}));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(clearedPath), 5000);
        QVERIFY(cleared.open(QIODevice::ReadOnly));
        const QJsonObject futureChild = QJsonDocument::fromJson(cleared.readAll()).object();
        cleared.close();
        for (const QString& key : {QStringLiteral("dev"), QStringLiteral("terminal"),
                                   QStringLiteral("agent"), QStringLiteral("control")})
            QVERIFY(futureChild.value(key).isNull());
        QVERIFY(!firstEndpoint.hasPendingConnections());
        QVERIFY(!firstControl.hasPendingConnections());
        QVERIFY(!staleAgent.hasPendingConnections());
        QVERIFY(!staleWindow.hasPendingConnections());
        QVERIFY(runTmux({QStringLiteral("detach-client"), QStringLiteral("-s"),
                         QLatin1Char('=') + anonymousTarget}));
        QVERIFY(attachment.waitForFinished(5000));
    }
#endif
}

// The address a pane resolution is keyed by. It is a PAIR — Dev Session plus
// layout pane id — and both halves matter: layout pane ids are minted per Dev
// Session, so "terminal-1" exists in every one of them and naming a terminal by
// the id alone would hand one session's pane another session's shell. The
// separator must also not be forgeable out of the parts, or two different
// addresses could collapse onto one cache entry.
void TstTerminalFactory::paneKeysAddressOneSlotOfOneDevSession()
{
    QVERIFY(TerminalFactory::paneKey(QStringLiteral("dev-a"), QStringLiteral("terminal-1"))
            != TerminalFactory::paneKey(QStringLiteral("dev-b"), QStringLiteral("terminal-1")));
    QVERIFY(TerminalFactory::paneKey(QStringLiteral("dev-a"), QStringLiteral("terminal-1"))
            != TerminalFactory::paneKey(QStringLiteral("dev-a"), QStringLiteral("terminal-10")));
    QVERIFY(TerminalFactory::paneKey(QStringLiteral("dev/a"), QStringLiteral("b"))
            != TerminalFactory::paneKey(QStringLiteral("dev"), QStringLiteral("a/b")));
    QVERIFY(TerminalFactory::paneKey(QStringLiteral("dev:4"), QStringLiteral(":slot"))
            != TerminalFactory::paneKey(QStringLiteral("dev"), QStringLiteral(":4:slot")));
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
    QCOMPARE(byRow.id.value, row);
    // Not merely unused — ABSENT. A row lookup cannot create, so a working
    // directory would be a promise the server never keeps, and the label must
    // not look like part of the question.
    QVERIFY(byRow.name.isEmpty());
    QVERIFY(!byRow.workingDirectory.has_value());

    const ResolveTerminalPaneParams byRowWithoutLabel = TerminalFactory::resolveParamsFor(
        server, session, QString(), row, QStringLiteral("/home/u"));
    QCOMPARE(byRowWithoutLabel.id.value, row);
    QVERIFY(byRowWithoutLabel.name.isEmpty());
    QVERIFY(!byRowWithoutLabel.workingDirectory.has_value());

    const ResolveTerminalPaneParams byLabel = TerminalFactory::resolveParamsFor(
        server, session, QStringLiteral("terminal-2"), QString(), QStringLiteral("/home/u"));
    QVERIFY(byLabel.id.value.isEmpty());
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
    // Keep the shell-quoting/security assertions independent of the complete
    // command template; managed environment entries may be cleared explicitly.

    // A working directory is the field most likely to carry a real quote, and
    // the one a user can type. Breaking out of it would run `id` on the host.
    const QString quotedDirectory = TerminalController::tmuxNewSessionCommand(
        QStringLiteral("ch_dev_t1"), QStringLiteral("/tmp/x'; id; echo '"),
        QString(), QString(), QString(), QString());
    QVERIFY(quotedDirectory.contains(QStringLiteral("-c '/tmp/x'\\''; id; echo '\\'''")));

    // Server-provided names and all session-option targets must remain shell-inert.
    const QString quotedTarget = TerminalController::tmuxNewSessionCommand(
        QStringLiteral("ch_d'; rm -rf ~; '_t`whoami`"), QStringLiteral("/w"),
        QString(), QString(), QString(), QString());
    QVERIFY(quotedTarget.contains(
        QStringLiteral("-s 'ch_d'\\''; rm -rf ~; '\\''_t`whoami`'")));
    QVERIFY(quotedTarget.contains(
        QStringLiteral("-t '=ch_d'\\''; rm -rf ~; '\\''_t`whoami`:' mouse on")));
    QVERIFY(quotedTarget.contains(
        QStringLiteral("-t '=ch_d'\\''; rm -rf ~; '\\''_t`whoami`:' destroy-unattached off")));

    // A leading `-` in the working directory is consumed as the value of -c by
    // getopt, and a newline is inert inside the quotes: neither adds a word to
    // the command.
    const QString command = TerminalController::tmuxNewSessionCommand(
        QStringLiteral("ch_dev_t1"), QStringLiteral("-rf /\nrm -rf ~"), QString(),
        QString(), QString(), QString());
    QVERIFY(command.contains(QStringLiteral("-c '-rf /\nrm -rf ~'")));
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
    // side and is a view-only operation; the rest is this pane's own input (in
    // two shapes, text and raw bytes, both landing on this pane's own PTY),
    // geometry, visibility, mount handshake and output-consumption report. No
    // attach, no kill, no detach, no tmux target, no working directory, no
    // session id.
    QCOMPARE(callable,
             QStringList({QStringLiteral("notifyOutputConsumed"),
                          QStringLiteral("notifyViewVisible"), QStringLiteral("ready"),
                          QStringLiteral("requestClear"), QStringLiteral("resize"),
                          QStringLiteral("sendBinaryInput"), QStringLiteral("sendInput")}));

    // Nor can the page read one back out and act on it: the properties are the
    // renderer's own view state.
    QStringList properties;
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i)
        properties << QString::fromLatin1(meta->property(i).name());
    properties.sort();
    QCOMPARE(properties,
             QStringList({QStringLiteral("columns"), QStringLiteral("connectionState"),
                          QStringLiteral("inputGeneration"), QStringLiteral("rows")}));
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
    QVERIFY(bridge->sendInput(QStringLiteral("ls\n"), bridge->inputGeneration()));
    QVERIFY(bridge->sendInput(QString::fromUtf8("é"), bridge->inputGeneration()));
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

    bridge->ready(true);
    QVERIFY(controller->viewVisible());
    bridge->notifyViewVisible(false);
    QVERIFY(!controller->viewVisible());
    bridge->notifyViewVisible(true);
    QVERIFY(controller->viewVisible());
}

void TstTerminalFactory::bridgeDecodesBinaryInputAndRefusesMalformedBase64()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);

    FakeTransport transport;
    controller->setTransport(&transport);

    // An X10 mouse report for a click past column 95: CSI M, button byte 0x20,
    // column 0xAB, row 0xFF. These are the bytes that made xterm.js emit the
    // report as a binary event in the first place, and they are exactly what
    // sendInput() would destroy — its QString is encoded as UTF-8, so 0xAB would
    // arrive as the two bytes 0xC2 0xAB.
    const QByteArray report = QByteArrayLiteral("\x1b[M\x20\xAB\xFF");
    QVERIFY(bridge->sendBinaryInput(QString::fromLatin1(report.toBase64()),
                                    bridge->inputGeneration()));
    QCOMPARE(transport.data(), report);

    // Malformed payloads are REFUSED, not partially written: a lenient decode
    // would type bytes the user never produced into their shell. One payload per
    // way of being malformed — a character outside the alphabet, a length that
    // cannot be a whole number of bytes, and a non-Latin-1 string that only a
    // page confused about the encoding would send.
    QVERIFY(!bridge->sendBinaryInput(QStringLiteral("****"), bridge->inputGeneration()));
    QVERIFY(!bridge->sendBinaryInput(QStringLiteral("QUJD*g=="), bridge->inputGeneration()));
    QVERIFY(!bridge->sendBinaryInput(QStringLiteral("A"), bridge->inputGeneration()));
    QVERIFY(!bridge->sendBinaryInput(QStringLiteral("===="), bridge->inputGeneration()));
    QVERIFY(!bridge->sendBinaryInput(QStringLiteral("A==="), bridge->inputGeneration()));
    QVERIFY(!bridge->sendBinaryInput(QStringLiteral("AA=A"), bridge->inputGeneration()));
    QVERIFY(!bridge->sendBinaryInput(QString::fromUtf8("héllo!!"), bridge->inputGeneration()));
    QCOMPARE(transport.data(), report);

    // Nothing to send is not an error, and it must not disturb the stream.
    QVERIFY(bridge->sendBinaryInput(QString(), bridge->inputGeneration()));
    QCOMPARE(transport.data(), report);

    // Still the same pane afterwards: a refused payload leaves the input path
    // working rather than wedged.
    QVERIFY(bridge->sendInput(QStringLiteral("ls\n"), bridge->inputGeneration()));
    QCOMPARE(transport.data(), report + QByteArrayLiteral("ls\n"));
    // Valid one-/two-byte tails carry padding; a full byte alphabet must also
    // survive unchanged, including NUL and values that are not UTF-8 text.
    QByteArray alphabet;
    for (int value = 0; value <= 255; ++value)
        alphabet.append(static_cast<char>(value));
    QByteArray expected = transport.data();
    for (const int size : {1, 2, 3, 256}) {
        const QByteArray bytes = alphabet.left(size);
        QVERIFY(bridge->sendBinaryInput(QString::fromLatin1(bytes.toBase64()),
                                        bridge->inputGeneration()));
        expected.append(bytes);
        QCOMPARE(transport.data(), expected);
    }
}

void TstTerminalFactory::bridgeReportsPartialInputWithoutDisconnectingOrReplaying_data()
{
    QTest::addColumn<bool>("binary");
    QTest::addColumn<qint64>("failure");
    QTest::newRow("text-no-progress") << false << qint64{0};
    QTest::newRow("text-write-error") << false << qint64{-1};
    QTest::newRow("binary-no-progress") << true << qint64{0};
    QTest::newRow("binary-write-error") << true << qint64{-1};
}

void TstTerminalFactory::bridgeReportsPartialInputWithoutDisconnectingOrReplaying()
{
    QFETCH(bool, binary);
    QFETCH(qint64, failure);
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create();
    TerminalBridge* bridge = factory.createBridge(controller);
    PrefixThenFailTransport transport(failure);
    controller->setTransport(&transport);
    controller->setState(TerminalState::Ready);
    QSignalSpy rejected(bridge, &TerminalBridge::inputRejected);
    QSignalSpy errors(&factory, &TerminalFactory::error);
    const QByteArray input = binary ? QByteArray::fromHex("1b5b4d20abff")
                                   : QByteArrayLiteral("echo command\n");
    if (binary)
        QVERIFY(!bridge->sendBinaryInput(QString::fromLatin1(input.toBase64()),
                                         bridge->inputGeneration()));
    else
        QVERIFY(!bridge->sendInput(QString::fromUtf8(input), bridge->inputGeneration()));
    QCOMPARE(transport.written(), input.left(3));
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.first().at(0).value<TerminalController*>(), controller);
    const QString message = errors.first().at(1).toString();
    QVERIFY(message.contains(QStringLiteral("Part of it may have reached")));
    QVERIFY(message.contains(QStringLiteral("Inspect the terminal before retrying")));
    QVERIFY(controller->state() == TerminalState::Ready);
    QVERIFY(transport.isWritable());

    FakeTransport replacement;
    controller->setTransport(&replacement);
    QCoreApplication::processEvents();
    QVERIFY(replacement.data().isEmpty());
    QVERIFY(bridge->sendInput(QStringLiteral("fresh"), bridge->inputGeneration()));
    QCOMPARE(replacement.data(), QByteArrayLiteral("fresh"));
    QCOMPARE(errors.count(), 1);
}

void TstTerminalFactory::queuedInputCannotCrossATransportReplacement_data()
{
    QTest::addColumn<bool>("binary");
    QTest::addColumn<bool>("reusePointer");
    QTest::newRow("text") << false << false;
    QTest::newRow("binary") << true << false;
    QTest::newRow("text-reused-pointer") << false << true;
    QTest::newRow("binary-reused-pointer") << true << true;
}

void TstTerminalFactory::queuedInputCannotCrossATransportReplacement()
{
    QFETCH(bool, binary);
    QFETCH(bool, reusePointer);
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create();
    TerminalBridge* bridge = factory.createBridge(controller);
    FakeTransport oldTransport;
    FakeTransport replacement;
    controller->setTransport(&oldTransport);
    const quint64 oldGeneration = bridge->inputGeneration();
    QVERIFY(bridge->sendInput(QStringLiteral("prefix"), oldGeneration));
    controller->setTransport(nullptr);
    controller->setTransport(reusePointer ? &oldTransport : &replacement);
    QSignalSpy rejected(bridge, &TerminalBridge::inputRejected);
    const QByteArray suffix = QByteArrayLiteral("old paste suffix\n");
    const bool accepted = binary
        ? bridge->sendBinaryInput(QString::fromLatin1(suffix.toBase64()), oldGeneration)
        : bridge->sendInput(QString::fromLatin1(suffix), oldGeneration);
    QVERIFY(!accepted);
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(oldTransport.data(), QByteArrayLiteral("prefix"));
    QVERIFY(replacement.data().isEmpty());
    QVERIFY(bridge->sendInput(QStringLiteral("fresh input"), bridge->inputGeneration()));
    if (reusePointer)
        QCOMPARE(oldTransport.data(), QByteArrayLiteral("prefixfresh input"));
    else
        QCOMPARE(replacement.data(), QByteArrayLiteral("fresh input"));
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

    bridge->ready(true);
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
    bridge->ready(true);
    QVERIFY(controller->viewVisible());
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(0).toString(), QStringLiteral("first screenful"));
}

void TstTerminalFactory::bridgePreservesHiddenReportBeforeReady()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    QSignalSpy writes(bridge, &TerminalBridge::write);

    bridge->notifyViewVisible(false);
    controller->ingestOutput(QByteArrayLiteral("hidden while loading"));
    QTRY_COMPARE(controller->hiddenBuffer(), QByteArrayLiteral("hidden while loading"));
    bridge->ready(false);

    QVERIFY(!controller->viewVisible());
    QCOMPARE(writes.count(), 0);
    bridge->notifyViewVisible(true);
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(0).toString(), QStringLiteral("hidden while loading"));
}

void TstTerminalFactory::aHiddenReplacementMountNeverReleasesRetainedOutput_data()
{
    QTest::addColumn<bool>("outgoingReportedHidden");
    QTest::newRow("ordinary-reload") << true;
    QTest::newRow("crashed-renderer") << false;
}

void TstTerminalFactory::aHiddenReplacementMountNeverReleasesRetainedOutput()
{
    QFETCH(bool, outgoingReportedHidden);
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create();
    TerminalBridge* bridge = factory.createBridge(controller);
    bridge->ready(true);
    QSignalSpy writes(bridge, &TerminalBridge::write);
    controller->ingestOutput(
        QByteArray(TerminalController::kMaxUnacknowledgedBytes, 'x'));
    QCOMPARE(controller->unacknowledgedBytes(),
             qint64(TerminalController::kMaxUnacknowledgedBytes));
    controller->ingestOutput(QByteArrayLiteral("retained for the replacement"));
    QTRY_COMPARE(controller->hiddenBuffer(),
                 QByteArrayLiteral("retained for the replacement"));
    if (outgoingReportedHidden)
        bridge->notifyViewVisible(false);
    const int before = writes.count();
    bridge->ready(false);
    QVERIFY(!controller->viewVisible());
    QCOMPARE(controller->unacknowledgedBytes(), qint64(0));
    QCOMPARE(writes.count(), before);
    QCOMPARE(controller->hiddenBuffer(), QByteArrayLiteral("retained for the replacement"));
    bridge->notifyViewVisible(true);
    QCOMPARE(writes.count(), before + 1);
    QCOMPARE(writes.last().first().toString(), QStringLiteral("retained for the replacement"));
    QVERIFY(controller->hiddenBuffer().isEmpty());
}

// The other side of the same rule, in the two shapes production produces:
// a pane the user hid, and a page that reloaded under a pane that never moved.
void TstTerminalFactory::bridgeRetainsOutputForAHiddenPaneAndAcrossAPageReload()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    bridge->ready(true);
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

    bridge->ready(true);
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

    QVERIFY(!bridge->sendInput(QStringLiteral("ls\n"), bridge->inputGeneration()));
    bridge->resize(120, 40);
    bridge->notifyViewVisible(false);
    bridge->notifyViewVisible(true);
    bridge->ready(true);
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

void TstTerminalFactory::bridgeDestructorHidesItsController()
{
    TerminalFactory factory(nullptr);
    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QPointer<TerminalBridge> bridge = factory.createBridge(controller, &pane);

    bridge->ready(true);
    QVERIFY(controller->viewVisible());
    delete bridge;
    QVERIFY(bridge.isNull());
    QVERIFY(!controller->viewVisible());
}

// Two halves of one contract.
//
// The CONTROLLER no longer splits a multi-byte sequence across batches at all:
// flush() holds an incomplete trailing character back, because a half-character
// that has already been emitted (and charged) can be orphaned for good if an
// eviction takes its continuation bytes before they are ever flushed.
//
// The BRIDGE still decodes statefully and still carries the byte weight of a
// text-less batch forward, and that is not redundant: flushReady is a public
// seam and the two halves of the flow-control accounting live in different
// languages, so the bridge must be correct on its own terms.
void TstTerminalFactory::bridgeDecodesUtf8SplitAcrossFlushes()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    bridge->ready(true);
    QSignalSpy writes(bridge, &TerminalBridge::write);

    // "✔" is E2 9C 94, fed in two pieces cut after the second byte.
    QSignalSpy flushes(controller, &TerminalController::flushReady);
    controller->ingestOutput(QByteArrayLiteral("\xE2\x9C"));
    // The flush window passes and NOTHING is emitted: the controller will not
    // charge the renderer for two thirds of a character.
    QTest::qWait(5 * TerminalController::kFlushIntervalMs);
    QCOMPARE(flushes.count(), 0);
    QCOMPARE(writes.count(), 0);

    controller->ingestOutput(QByteArrayLiteral("\x94 done"));
    QTRY_COMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(0).toString(), QString::fromUtf8("\xE2\x9C\x94 done"));
    QVERIFY(!writes.at(0).at(0).toString().contains(QChar(0xFFFD)));
    // All eight bytes the controller charged against its flow-control window
    // are advertised to the page, so the page can hand every one of them back.
    QCOMPARE(writes.at(0).at(1).toInt(), 8);

    // The bridge's own half, driven at its actual seam. A batch that decodes to
    // nothing must not become an empty chunk at the page, and its byte weight
    // must survive onto the next batch that does decode — otherwise the
    // controller's credit would leak away one truncated glyph at a time until
    // the pane stopped receiving output altogether.
    emit controller->flushReady(QByteArrayLiteral("\xE2\x9C"));
    QCOMPARE(writes.count(), 1);
    emit controller->flushReady(QByteArrayLiteral("\x94"));
    QCOMPARE(writes.count(), 2);
    QCOMPARE(writes.at(1).at(0).toString(), QString::fromUtf8("\xE2\x9C\x94"));
    QCOMPARE(writes.at(1).at(1).toInt(), 3);
}

// A page reload leaves the bridge's stateful UTF-8 decoder holding the lead
// bytes of a character whose tail was handed to the page that is going away.
// Those bytes were EMITTED, so the controller is not retaining them and they
// are never coming back. Left in the decoder they would be completed from the
// first bytes the replacement renderer is sent, painting one wrong glyph at the
// top of a screen the user is looking at. The mount handshake starts the decode
// over, exactly as it starts the flow-control account over.
void TstTerminalFactory::bridgeStartsAFreshDecodeForAReplacementRenderer()
{
    QObject pane;
    TerminalFactory factory(nullptr);
    TerminalController* controller = factory.create(&pane);
    TerminalBridge* bridge = factory.createBridge(controller, &pane);
    bridge->ready(true);
    QSignalSpy writes(bridge, &TerminalBridge::write);

    // Two thirds of "✔" (E2 9C 94) reach the page's decoder and stay there.
    emit controller->flushReady(QByteArrayLiteral("\xE2\x9C"));
    QCOMPARE(writes.count(), 0);

    // The page reloads and announces itself. Its first batch is plain ASCII.
    bridge->ready(true);
    emit controller->flushReady(QByteArrayLiteral("fresh"));

    QCOMPARE(writes.count(), 1);
    // Exactly the new batch: no replacement character in front of it from the
    // half-character the previous renderer took with it.
    QCOMPARE(writes.at(0).at(0).toString(), QStringLiteral("fresh"));
    QVERIFY(!writes.at(0).at(0).toString().contains(QChar(0xFFFD)));
    // And only the new batch's bytes are charged, so the page can hand back
    // exactly what it was told.
    QCOMPARE(writes.at(0).at(1).toInt(), 5);
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
    bridge->ready(true);
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
    bridge->ready(true); // the replacement page mounts; no hidden report preceded it
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
    bridge->ready(true);
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

// ---- AG-N1: the pane's agent-status identity -------------------------------
//
// SPEC 6.6's "generic" harness publishes no lifecycle events, so the only thing
// that says whether it is working is its terminal output. The output is on this
// side of the wire, in ch::TerminalController; the identity it must be reported
// under — the Dev Session and the `terminal_panes` row — is known only here, in
// the factory. These four cases cover the join, and specifically the rule that
// decides WHICH answer a pane is allowed to take its identity from.
//
// What THESE cases do not cover: attach() itself. It constructs a real
// ch::SshChannelDevice and startPty() refuses without a live session, so the
// noteTerminalAttached() call the production attach path makes is reachable
// only from tst_liveterminalfactory. Where a test needs a pane to be in the
// attached state it calls noteTerminalAttached() itself — the same call, made
// by hand — and says so. (The recreated-session cases at the end of this file
// do drive the real attach path, through AttachingFactory's one override of the
// channel open; nothing was changed here to use it, because a stand-in for a
// call these tests make directly would only add a layer to read through.)

// The normal path, end to end: ask by the legacy slot label, take the row id,
// the owning Dev Session and the harness off the server's ANSWER, and report
// the pane's output under that row id.
void TstTerminalFactory::aPaneReportsItsOutputUnderTheRowIdTheServerAnswered()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AgentStatusMonitor monitor;
    // Far above anything this test waits out: a generic pane goes quiet after
    // this window, and nothing here is measuring the quiet window.
    monitor.setFallbackIdleThresholdMs(60000);

    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    factory.setAgentMonitor(&monitor);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);

    // No row id on the leaf: the legacy slot-label path, where the row id
    // exists only in the answer.
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QString(),
                                  QStringLiteral("/repo")));
    // Repeating the same request before its answer joins the existing flight;
    // it must not produce a duplicate targetResolved() signal.
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QString(),
                                  QStringLiteral("/repo")));
    const QJsonObject request = rpc.takeRequest();
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(rpc::kMethodWorkspaceResolveTerminalPane));
    rpc.answerWithRow(request.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("row-A"), QStringLiteral("s1"),
                      QStringLiteral("terminal-1"), QStringLiteral("ch_s1_row-A"),
                      QStringLiteral("generic"));
    QTRY_COMPARE(resolved.count(), 1);
    QCOMPARE(resolved.at(0).at(1).toString(), QStringLiteral("ch_s1_row-A"));

    // The harness travelled with the answer, so the monitor knows this pane
    // derives its state from output before a byte of it exists. Registration on
    // its own says nothing: the pane has produced no observation yet.
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Unknown));

    // Stand-in for the attach (see the note above).
    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-A"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Starting));

    controller->ingestOutput(QByteArrayLiteral("tmux drew the pane"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));

    // Under the SERVER-MINTED row and nothing else. The layout slot label is
    // recycled by closed panes, which is why it is never an identity.
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("terminal-1")),
             asInt(AgentState::Unknown));
}

void TstTerminalFactory::rowResolutionRetainsItsDevSessionScope_data()
{
    QTest::addColumn<bool>("cached");
    QTest::addColumn<QString>("firstSession");
    QTest::addColumn<QString>("firstRow");
    QTest::addColumn<QString>("secondSession");
    QTest::addColumn<QString>("secondRow");
    QTest::newRow("in-flight") << false << QStringLiteral("s1") << QStringLiteral("row-A")
                              << QStringLiteral("s2") << QStringLiteral("row-A");
    QTest::newRow("cached") << true << QStringLiteral("s1") << QStringLiteral("row-A")
                           << QStringLiteral("s2") << QStringLiteral("row-A");
    QTest::newRow("opaque-in-flight") << false << QStringLiteral("s1/segment")
                                     << QStringLiteral("row-A") << QStringLiteral("s1")
                                     << QStringLiteral("segment/row-A");
    QTest::newRow("opaque-cached") << true << QStringLiteral("s1/segment")
                                  << QStringLiteral("row-A") << QStringLiteral("s1")
                                  << QStringLiteral("segment/row-A");
}

void TstTerminalFactory::rowResolutionRetainsItsDevSessionScope()
{
    QFETCH(bool, cached);
    QFETCH(QString, firstSession);
    QFETCH(QString, firstRow);
    QFETCH(QString, secondSession);
    QFETCH(QString, secondRow);
    RpcPair rpc;
    QVERIFY(rpc.start());
    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    auto* first = factory.create();
    auto* second = factory.create();
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);

    QVERIFY(factory.resolveTarget(first, firstSession, QStringLiteral("terminal-1"),
                                  firstRow, QStringLiteral("/repo")));
    const QJsonObject firstRequest = rpc.takeRequest();
    QVERIFY(!firstRequest.isEmpty());
    const auto answerFirst = [&]() {
        rpc.answerWithRow(firstRequest.value(QStringLiteral("id")).toInt(),
                          firstRow, firstSession,
                          QStringLiteral("terminal-1"), QStringLiteral("ch_s1_row-A"), QString());
    };
    if (cached) {
        answerFirst();
        QTRY_COMPARE(resolved.count(), 1);
    }

    // A stale layout can carry the same row id after switching Dev Sessions.
    // The server must decide whether that row belongs to this new context,
    // even when the previous context already has a cached or in-flight answer.
    QVERIFY(factory.resolveTarget(second, secondSession, QStringLiteral("terminal-1"),
                                  secondRow, QStringLiteral("/repo")));
    const QJsonObject secondRequest = rpc.takeRequest();
    QVERIFY2(!secondRequest.isEmpty(), "the old context bypassed the server ownership check");
    QCOMPARE(secondRequest.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("devSessionId")).toString(), secondSession);
    if (!cached) {
        answerFirst();
        QTRY_COMPARE(resolved.count(), 1);
    }
    QCOMPARE(qvariant_cast<TerminalController*>(resolved.at(0).at(0)), first);
    QCOMPARE(resolved.at(0).at(1).toString(), QStringLiteral("ch_s1_row-A"));
    rpc.answerWithError(secondRequest.value(QStringLiteral("id")).toInt(),
                        QStringLiteral("terminal does not belong to this Dev Session"));
    QTRY_COMPARE(resolved.count(), 2);
    QCOMPARE(qvariant_cast<TerminalController*>(resolved.at(1).at(0)), second);
    QVERIFY(resolved.at(1).at(1).toString().isEmpty());
    QVERIFY(!factory.attach(second, QStringLiteral("ch_s1_row-A"),
                            QStringLiteral("/repo"), 80, 24));
}

// The BACKFILL half of the same answer, and specifically what happens the
// second time a legacy slot label is resolved.
//
// A layout leaf stored before layouts carried a row id is addressed by its slot
// label, which is recycled across panes and clients — the unsafe key the row id
// exists to retire. paneRowResolved() is what lets ch::SessionLayouts write the
// row id into that leaf so it never asks by label again, and this factory
// cannot see whether that write landed: the pane may have been closed while the
// answer travelled, the layout may have been mid-load, the write may have been
// refused. So the report must be repeatable. It used to be emitted ONLY from
// the live round trip, while every later resolution of the same label is
// answered from this factory's cache and said nothing about the row at all — so
// one missed report meant the leaf kept using the recyclable label for the rest
// of the process, with nothing able to correct it.
void TstTerminalFactory::aCachedLegacyAnswerStillReportsItsRowSoTheBackfillCanBeRetried()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QSignalSpy rows(&factory, &TerminalFactory::paneRowResolved);

    // The legacy path: no row id on the leaf, so the row exists only in the
    // server's answer.
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QString(),
                                  QStringLiteral("/repo")));
    const QJsonObject request = rpc.takeRequest();
    rpc.answerWithRow(request.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("row-A"), QStringLiteral("s1"),
                      QStringLiteral("terminal-1"), QStringLiteral("ch_s1_row-A"),
                      QStringLiteral("generic"));
    QTRY_COMPARE(resolved.count(), 1);
    QCOMPARE(rows.count(), 1);
    QCOMPARE(rows.at(0).at(0).toString(), QStringLiteral("s1"));
    QCOMPARE(rows.at(0).at(1).toString(), QStringLiteral("terminal-1"));
    QCOMPARE(rows.at(0).at(2).toString(), QStringLiteral("row-A"));

    // The SAME label again — a reconnect, or simply a leaf whose backfill did
    // not land the first time. No second round trip happens: nothing answers
    // one, so reaching a second targetResolved() at all proves the answer came
    // from the cache. And it carries the row, exactly as the first one did.
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QString(),
                                  QStringLiteral("/repo")));
    QTRY_COMPARE(resolved.count(), 2);
    QCOMPARE(resolved.at(1).at(1).toString(), QStringLiteral("ch_s1_row-A"));
    QTRY_COMPARE(rows.count(), 2);
    QCOMPARE(rows.at(1).at(0).toString(), QStringLiteral("s1"));
    QCOMPARE(rows.at(1).at(1).toString(), QStringLiteral("terminal-1"));
    QCOMPARE(rows.at(1).at(2).toString(), QStringLiteral("row-A"));

    // The guard is unchanged, on the cached path as on the live one: a pane
    // addressed BY its row id already knows the answer, so there is nothing to
    // backfill and nothing is reported. (This resolution is also served from the
    // cache — the live answer above was remembered under the row id too.)
    TerminalController* byRow = factory.create(&pane);
    QVERIFY(factory.resolveTarget(byRow, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"),
                                  QStringLiteral("row-A"), QStringLiteral("/repo")));
    QTRY_COMPARE(resolved.count(), 3);
    QCOMPARE(resolved.at(2).at(1).toString(), QStringLiteral("ch_s1_row-A"));
    QCOMPARE(rows.count(), 2);
}

// The trap. A lookup is a round trip, and QML can point a pane at a different
// terminal while one is in flight — the user clicks another Dev Session in the
// sidebar. The pane's OLD channel is still open and still printing at that
// moment. Neither the terminal it has left nor the one it has not been given
// yet may be credited with those bytes.
void TstTerminalFactory::aPaneRetargetedMidLookupNeverReportsUnderTheSupersededPanesIdentity()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AgentStatusMonitor monitor;
    monitor.setFallbackIdleThresholdMs(60000);

    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    factory.setAgentMonitor(&monitor);

    // BOTH candidate terminals are registered and attached, so a byte reported
    // under either one would show up as a transition out of Starting. Without
    // this the test could pass by the monitor simply not knowing the ids.
    for (const char* row : {"row-A", "row-B"}) {
        monitor.setTerminalHarness(QStringLiteral("s1"), QString::fromLatin1(row),
                                   QStringLiteral("generic"));
        monitor.noteTerminalAttached(QStringLiteral("s1"), QString::fromLatin1(row));
    }
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Starting));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-B")),
             asInt(AgentState::Starting));

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);

    // The pane asks about row-A...
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QStringLiteral("row-A"),
                                  QStringLiteral("/repo")));
    const QJsonObject firstRequest = rpc.takeRequest();
    // ...and is retargeted at row-B before that answer comes back.
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-2"), QStringLiteral("row-B"),
                                  QStringLiteral("/repo")));
    const QJsonObject secondRequest = rpc.takeRequest();
    QVERIFY(firstRequest.value(QStringLiteral("id")).toInt()
            != secondRequest.value(QStringLiteral("id")).toInt());

    // The superseded answer lands first.
    rpc.answerWithRow(firstRequest.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("row-A"), QStringLiteral("s1"),
                      QStringLiteral("terminal-1"), QStringLiteral("ch_s1_row-A"),
                      QStringLiteral("generic"));
    QTRY_COMPARE(rpc.client()->pendingCount(), 1);
    QCOMPARE(resolved.count(), 0);

    // The pane prints on the channel it opened for row-A.
    controller->ingestOutput(QByteArrayLiteral("output from the pane we left"));
    QTest::qWait(50);
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Starting));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-B")),
             asInt(AgentState::Starting));

    // The answer the pane is actually waiting for. From here it is row-B, and
    // its output is row-B's.
    rpc.answerWithRow(secondRequest.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("row-B"), QStringLiteral("s1"),
                      QStringLiteral("terminal-2"), QStringLiteral("ch_s1_row-B"),
                      QStringLiteral("generic"));
    QTRY_COMPARE(resolved.count(), 1);
    controller->ingestOutput(QByteArrayLiteral("output from the pane we are on"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-B")),
             asInt(AgentState::Running));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Starting));
}

void TstTerminalFactory::supersededResolutionErrorsDoNotReplaceTheCurrentAnswer()
{
    RpcPair rpc;
    QVERIFY(rpc.start());
    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    TerminalController* controller = factory.create();
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QSignalSpy errors(&factory, &TerminalFactory::error);
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"), QString(),
                                  QStringLiteral("row-A"), QString()));
    const QJsonObject oldRequest = rpc.takeRequest();
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"), QString(),
                                  QStringLiteral("row-B"), QString()));
    const QJsonObject currentRequest = rpc.takeRequest();
    rpc.answerWithRow(currentRequest.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("row-B"), QStringLiteral("s1"), QString(),
                      QStringLiteral("ch_s1_row-B"), QString());
    QTRY_COMPARE(resolved.count(), 1);
    rpc.answerWithError(oldRequest.value(QStringLiteral("id")).toInt(),
                        QStringLiteral("the old row was deleted"));
    QTRY_COMPARE(rpc.client()->pendingCount(), 0);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(errors.count(), 0);
    QVERIFY(factory.attach(controller, resolved.first().at(1).toString(), QString(), 80, 24));
    QCOMPARE(factory.targetFor(controller), QStringLiteral("ch_s1_row-B"));
}

void TstTerminalFactory::detachCancelsPendingResolution_data()
{
    QTest::addColumn<bool>("cached");
    QTest::newRow("rpc-answer") << false;
    QTest::newRow("queued-cache-answer") << true;
}

void TstTerminalFactory::detachCancelsPendingResolution()
{
    QFETCH(bool, cached);
    RpcPair rpc;
    QVERIFY(rpc.start());
    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    TerminalController* controller = factory.create();
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QSignalSpy errors(&factory, &TerminalFactory::error);
    const auto resolve = [&]() {
        return factory.resolveTarget(controller, QStringLiteral("s1"), QString(),
                                     QStringLiteral("row-A"), QString());
    };
    QVERIFY(resolve());
    const QJsonObject request = rpc.takeRequest();
    if (cached) {
        rpc.answerWithRow(request.value(QStringLiteral("id")).toInt(),
                          QStringLiteral("row-A"), QStringLiteral("s1"), QString(),
                          QStringLiteral("ch_s1_row-A"), QString());
        QTRY_COMPARE(resolved.count(), 1);
        resolved.clear();
        QVERIFY(resolve());
    }
    factory.detach(controller);
    if (!cached) {
        rpc.answerWithRow(request.value(QStringLiteral("id")).toInt(),
                          QStringLiteral("row-A"), QStringLiteral("s1"), QString(),
                          QStringLiteral("ch_s1_row-A"), QString());
        QTRY_COMPARE(rpc.client()->pendingCount(), 0);
    }
    QCoreApplication::processEvents();
    QCOMPARE(resolved.count(), 0);
    QCOMPARE(errors.count(), 0);
    QVERIFY(!controller->transport());
    // Cancellation is per waiter, not eviction of a server-authoritative
    // answer. A later request for this same terminal still resolves normally.
    QVERIFY(resolve());
    QTRY_COMPARE(resolved.count(), 1);
    QCOMPARE(resolved.first().at(1).toString(), QStringLiteral("ch_s1_row-A"));
}

void TstTerminalFactory::cancellingOldServerFlightsDoesNotCancelReentrantNewRequests()
{
    RpcPair rpc;
    QVERIFY(rpc.start());
    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    TerminalController* first = factory.create();
    TerminalController* second = factory.create();
    const auto resolve = [&](TerminalController* controller, const QString& row) {
        return factory.resolveTarget(controller, QStringLiteral("s1"), QString(), row, QString());
    };
    QVERIFY(resolve(first, QStringLiteral("row-A")));
    const QJsonObject oldFirst = rpc.takeRequest();
    QVERIFY(resolve(second, QStringLiteral("row-B")));
    const QJsonObject oldSecond = rpc.takeRequest();
    bool renewed = false;
    bool renewalAccepted = false;
    connect(&factory, &TerminalFactory::error, &factory,
            [&](TerminalController*, const QString&) {
                if (renewed)
                    return;
                renewed = true;
                factory.detach(first);
                factory.detach(second);
                renewalAccepted = resolve(first, QStringLiteral("row-A"))
                    && resolve(second, QStringLiteral("row-B"));
            });
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    factory.setServerId(QStringLiteral("srv-2"));
    QVERIFY(renewed);
    QVERIFY(renewalAccepted);
    QCOMPARE(resolved.count(), 0);
    const QJsonObject newFirst = rpc.takeRequest();
    const QJsonObject newSecond = rpc.takeRequest();
    QVERIFY(!newFirst.isEmpty());
    QVERIFY(!newSecond.isEmpty());
    for (const QJsonObject& request : {oldFirst, oldSecond})
        rpc.answerWithError(request.value(QStringLiteral("id")).toInt(),
                            QStringLiteral("old server request"));
    for (const QJsonObject& request : {newFirst, newSecond}) {
        const QString row = request.value(QStringLiteral("params")).toObject()
                                .value(QStringLiteral("id")).toString();
        rpc.answerWithRow(request.value(QStringLiteral("id")).toInt(), row,
                          QStringLiteral("s1"), QString(), QStringLiteral("ch_") + row, QString());
    }
    QTRY_COMPARE(resolved.count(), 2);
    for (const QList<QVariant>& answer : resolved)
        QVERIFY(!answer.at(1).toString().isEmpty());
}

// A pane pointed somewhere new whose lookup FAILS has no identity at all. It
// may not keep the one the previous answer gave it: those bytes are not that
// terminal's any more, and a stale identity that merely takes longer to be
// noticed is the same defect.
void TstTerminalFactory::aFailedResolutionLeavesThePaneWithNoIdentity()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AgentStatusMonitor monitor;

    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    factory.setAgentMonitor(&monitor);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QStringLiteral("row-A"),
                                  QStringLiteral("/repo")));
    const QJsonObject first = rpc.takeRequest();
    rpc.answerWithRow(first.value(QStringLiteral("id")).toInt(), QStringLiteral("row-A"),
                      QStringLiteral("s1"), QStringLiteral("terminal-1"),
                      QStringLiteral("ch_s1_row-A"), QStringLiteral("generic"));
    QTRY_COMPARE(resolved.count(), 1);

    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-A"));
    controller->ingestOutput(QByteArrayLiteral("working"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));

    // Retargeted at a legacy slot the factory has never resolved, so there is
    // nothing it could pre-emptively bind even if it wanted to — and the lookup
    // then fails.
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-legacy"), QString(),
                                  QStringLiteral("/repo")));
    const QJsonObject second = rpc.takeRequest();
    rpc.answerWithError(second.value(QStringLiteral("id")).toInt(),
                        QStringLiteral("no such Dev Session"));
    QTRY_COMPARE(errors.count(), 1);
    QTRY_COMPARE(resolved.count(), 2);
    QVERIFY(resolved.at(1).at(1).toString().isEmpty());

    // Identity removal ended local observation, so server observations can
    // speak for row-A again.
    monitor.noteRemoteActivity(QStringLiteral("s1"), QStringLiteral("row-A"), 0);
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));
    // Make the old row locally observable again, as a separate pane could.
    // Otherwise a stale forwarding connection could be hidden by the monitor's
    // detached guard and this identity test would pass for the wrong reason.
    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-A"));
    const int previous = monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A"));
    QVERIFY(previous != asInt(AgentState::Running));
    QSignalSpy transitions(&monitor, &AgentStatusMonitor::agentStateChanged);
    controller->ingestOutput(QByteArrayLiteral("still printing"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")), previous);
    QCOMPARE(transitions.count(), 0);
}

// A target recorded while looking at one workspace server must not be reused
// to kill a same-named session after the factory switches profiles.
void TstTerminalFactory::targetCannotCrossAWorkspaceServerSwitch()
{
    OfflineFactory factory(nullptr);
    factory.setServerId(QStringLiteral("srv-1"));

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    // The offline subclass opens the lower-level attach seam, which records a
    // target before the expected no-pool failure. That lets this test exercise
    // the target's server binding without a live SSH session.
    QVERIFY(!factory.attach(controller, QStringLiteral("srv-1-target"),
                            QStringLiteral("/repo"), 80, 24));
    QCOMPARE(factory.targetFor(controller), QStringLiteral("srv-1-target"));

    factory.setServerId(QStringLiteral("srv-2"));
    QVERIFY(factory.targetFor(controller).isEmpty());
    QVERIFY(errors.count() >= 1);
}

void TstTerminalFactory::oldResolutionsCannotCompleteANewServerRequest_data()
{
    QTest::addColumn<bool>("cached");
    QTest::addColumn<bool>("returnToOriginalServer");
    QTest::newRow("queued-cache-other-server") << true << false;
    QTest::newRow("queued-cache-returned-server") << true << true;
    QTest::newRow("rpc-returned-server") << false << true;
}

void TstTerminalFactory::oldResolutionsCannotCompleteANewServerRequest()
{
    QFETCH(bool, cached);
    QFETCH(bool, returnToOriginalServer);
    RpcPair rpc;
    QVERIFY(rpc.start());
    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QSignalSpy rows(&factory, &TerminalFactory::paneRowResolved);
    const auto resolve = [&]() {
        return factory.resolveTarget(controller, QStringLiteral("s1"),
                                     QStringLiteral("terminal-1"), QString(),
                                     QStringLiteral("/repo"));
    };

    QVERIFY(resolve());
    const QJsonObject oldRequest = rpc.takeRequest();
    QVERIFY(!oldRequest.isEmpty());
    if (cached) {
        rpc.answerWithRow(oldRequest.value(QStringLiteral("id")).toInt(),
                          QStringLiteral("old-row"), QStringLiteral("s1"),
                          QStringLiteral("terminal-1"), QStringLiteral("old-target"),
                          QStringLiteral("generic"));
        QTRY_COMPARE(resolved.count(), 1);
        // Leave the cache completion queued while profiles change.
        QVERIFY(resolve());
    }
    factory.setServerId(QStringLiteral("srv-2"));
    if (returnToOriginalServer)
        factory.setServerId(QStringLiteral("srv-1"));
    resolved.clear();
    rows.clear();

    // The same address is now owned by a different resolution flight.
    QVERIFY(resolve());
    const QJsonObject newRequest = rpc.takeRequest();
    QVERIFY(!newRequest.isEmpty());
    if (!cached) {
        rpc.answerWithRow(oldRequest.value(QStringLiteral("id")).toInt(),
                          QStringLiteral("old-row"), QStringLiteral("s1"),
                          QStringLiteral("terminal-1"), QStringLiteral("old-target"),
                          QStringLiteral("generic"));
    }
    rpc.answerWithRow(newRequest.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("new-row"), QStringLiteral("s1"),
                      QStringLiteral("terminal-1"), QStringLiteral("new-target"),
                      QStringLiteral("generic"));
    QTRY_VERIFY(!resolved.isEmpty()
                && resolved.last().at(1).toString() == QStringLiteral("new-target"));
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(rows.count(), 1);
    QCOMPARE(rows.first().at(2).toString(), QStringLiteral("new-row"));

    // A reconnect must also reuse the new answer, not the retired flight.
    QVERIFY(resolve());
    QTRY_COMPARE(resolved.count(), 2);
    QCOMPARE(resolved.last().at(1).toString(), QStringLiteral("new-target"));
}

void TstTerminalFactory::aServerSwitchDuringResolutionDeliveryCancelsTheOtherWaiters()
{
    RpcPair rpc;
    QVERIFY(rpc.start());
    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    QObject pane;
    TerminalController* first = factory.create(&pane);
    TerminalController* second = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QSignalSpy rows(&factory, &TerminalFactory::paneRowResolved);
    connect(&factory, &TerminalFactory::targetResolved, &factory,
            [&](TerminalController* controller, const QString& target) {
                if (controller == first && !target.isEmpty())
                    factory.setServerId(QStringLiteral("srv-2"));
            });
    for (TerminalController* controller : {first, second}) {
        QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                      QStringLiteral("terminal-1"), QString(),
                                      QStringLiteral("/repo")));
    }
    const QJsonObject request = rpc.takeRequest();
    QVERIFY(!request.isEmpty());
    rpc.answerWithRow(request.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("old-row"), QStringLiteral("s1"),
                      QStringLiteral("terminal-1"), QStringLiteral("old-target"),
                      QStringLiteral("generic"));
    QTRY_COMPARE(resolved.count(), 2);
    QCOMPARE(resolved.first().at(0).value<TerminalController*>(), first);
    QCOMPARE(resolved.first().at(1).toString(), QStringLiteral("old-target"));
    QCOMPARE(resolved.last().at(0).value<TerminalController*>(), second);
    QVERIFY(resolved.last().at(1).toString().isEmpty());
    QCOMPARE(rows.count(), 0);
}

// The other end of the same rule. A tmux target arrives as DATA, from a server,
// and it is then used as tmux GRAMMAR. codeharbord validates what it mints and
// repairs stored rows, so what is left is a row written by an older daemon or a
// server that is not the one we think it is — and a target such as `$0` selects
// whichever session holds that id, which is a session belonging to somebody
// else. The answer must fail the resolution rather than be adopted: no target,
// no identity, and nothing cached, so a retry actually asks again.
void TstTerminalFactory::aServerAnswerWithAnUnusableTmuxTargetFailsTheResolution()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AgentStatusMonitor monitor;
    monitor.setFallbackIdleThresholdMs(60000);

    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    factory.setAgentMonitor(&monitor);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QSignalSpy errors(&factory, &TerminalFactory::error);
    QSignalSpy rows(&factory, &TerminalFactory::paneRowResolved);

    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QString(),
                                  QStringLiteral("/repo")));
    const QJsonObject request = rpc.takeRequest();
    rpc.answerWithRow(request.value(QStringLiteral("id")).toInt(), QStringLiteral("row-A"),
                      QStringLiteral("s1"), QStringLiteral("terminal-1"),
                      QStringLiteral("$0"), QStringLiteral("generic"));

    // Reported as a failure: an empty target, with the reason naming the value.
    QTRY_COMPARE(resolved.count(), 1);
    QVERIFY(resolved.at(0).at(1).toString().isEmpty());
    QCOMPARE(errors.count(), 1);
    QVERIFY(errors.at(0).at(1).toString().contains(QStringLiteral("$0")));
    // Nothing to backfill into the layout leaf either: the row was not adopted.
    QCOMPARE(rows.count(), 0);

    // No identity, so the pane's output is attributed to nobody. row-A is
    // registered and attached BY HAND here, so the monitor demonstrably knows
    // the id and would move it to Running the instant a report arrived — without
    // this the assertion could pass simply because the monitor had never heard
    // of row-A. The factory refused to register it, which is itself part of the
    // outcome: the resolution never produced an identity to register.
    monitor.setTerminalHarness(QStringLiteral("s1"), QStringLiteral("row-A"),
                               QStringLiteral("generic"));
    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-A"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Starting));
    controller->ingestOutput(QByteArrayLiteral("printing under no identity"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Starting));

    // And nothing was cached: a retry reaches the server again rather than being
    // answered from a remembered bad target. This time the answer is usable.
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QString(),
                                  QStringLiteral("/repo")));
    const QJsonObject retry = rpc.takeRequest();
    QCOMPARE(retry.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(rpc::kMethodWorkspaceResolveTerminalPane));
    rpc.answerWithRow(retry.value(QStringLiteral("id")).toInt(), QStringLiteral("row-A"),
                      QStringLiteral("s1"), QStringLiteral("terminal-1"),
                      QStringLiteral("ch_s1_row-A"), QStringLiteral("generic"));
    QTRY_COMPARE(resolved.count(), 2);
    QCOMPARE(resolved.at(1).at(1).toString(), QStringLiteral("ch_s1_row-A"));
    QCOMPARE(rows.count(), 1);
}

// A remembered answer names rows on ONE server. Both halves of it — the tmux
// target and the row id the pane reports its agent state under — are dropped
// when the workspace server changes, or a legacy slot label reused on the next
// server inherits the previous server's terminal identity.
void TstTerminalFactory::changingTheWorkspaceServerForgetsRememberedRowIdentities()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AgentStatusMonitor monitor;

    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    factory.setAgentMonitor(&monitor);

    QObject pane;
    TerminalController* first = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);

    // A legacy slot label resolved on srv-1, so the factory remembers that
    // "s1/terminal-1" is row-A.
    QVERIFY(factory.resolveTarget(first, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QString(),
                                  QStringLiteral("/repo")));
    const QJsonObject firstRequest = rpc.takeRequest();
    rpc.answerWithRow(firstRequest.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("row-A"), QStringLiteral("s1"),
                      QStringLiteral("terminal-1"), QStringLiteral("ch_s1_row-A"),
                      QStringLiteral("generic"));
    QTRY_COMPARE(resolved.count(), 1);
    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-A"));
    first->ingestOutput(QByteArrayLiteral("srv-1 output"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));

    factory.setServerId(QStringLiteral("srv-2"));
    monitor.noteRemoteActivity(QStringLiteral("s1"), QStringLiteral("row-A"), 0);
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));
    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-A"));
    const int oldState = monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A"));
    QVERIFY(oldState != asInt(AgentState::Running));

    // A pane on the new server asks about the SAME slot label.
    TerminalController* second = factory.create(&pane);
    QVERIFY(factory.resolveTarget(second, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QString(),
                                  QStringLiteral("/repo")));
    const QJsonObject secondRequest = rpc.takeRequest();
    // The remembered target went with the server, so this is a real round trip
    // and it is addressed to the new server.
    QCOMPARE(secondRequest.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-2"));

    // And while it is in flight the pane has no identity: srv-1's row-A is not
    // available to be inherited. Checked with no wait, for the same reason as
    // the failed-resolution case above — a report would land synchronously.
    QSignalSpy transitions(&monitor, &AgentStatusMonitor::agentStateChanged);
    second->ingestOutput(QByteArrayLiteral("srv-2 output, before the answer"));
    first->ingestOutput(QByteArrayLiteral("retiring srv-1 pane output"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")), oldState);
    QCOMPARE(transitions.count(), 0);

    // The new server's own row is what it ends up reporting under.
    rpc.answerWithRow(secondRequest.value(QStringLiteral("id")).toInt(),
                      QStringLiteral("row-Z"), QStringLiteral("s1"),
                      QStringLiteral("terminal-1"), QStringLiteral("ch_s1_row-Z"),
                      QStringLiteral("generic"));
    QTRY_COMPARE(resolved.count(), 2);
    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-Z"));
    second->ingestOutput(QByteArrayLiteral("srv-2 output"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-Z")),
             asInt(AgentState::Running));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")), oldState);
}

// The resolution cache outlives a disconnect on purpose, and a remembered
// answer carries the harness the pane resolved with. So a harness the user
// changes has to reach that memory as well: left alone, the next rebind would
// re-apply the old value and put the pane back to reporting exactly as it did
// before, with nothing on screen to explain why.
void TstTerminalFactory::aChangedHarnessSurvivesTheResolutionCache()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AgentStatusMonitor monitor;
    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    factory.setAgentMonitor(&monitor);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);

    // Resolved as a plain shell: no clock, so output says nothing about it.
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QStringLiteral("row-A"),
                                  QStringLiteral("/repo")));
    const QJsonObject first = rpc.takeRequest();
    rpc.answerWithRow(first.value(QStringLiteral("id")).toInt(), QStringLiteral("row-A"),
                      QStringLiteral("s1"), QStringLiteral("terminal-1"),
                      QStringLiteral("ch_s1_row-A"), QString());
    QTRY_COMPARE(resolved.count(), 1);

    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-A"));
    controller->ingestOutput(QByteArrayLiteral("printing"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Unknown));

    // The user asks for the activity clock. In production the factory also
    // re-reports the live channel here, which is what makes the change take
    // effect at once; this harness has no PTY to attach, so the attach is made
    // by hand exactly as the note at the top of these monitor tests describes.
    factory.noteHarnessChanged(QStringLiteral("row-A"), QStringLiteral("generic"));
    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-A"));
    controller->ingestOutput(QByteArrayLiteral("printing again"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));

    // Now the part the cache used to undo: the pane is resolved again, which
    // after a reconnect is answered from memory rather than from the server.
    // The answer it hands the monitor must be the harness the user chose.
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QStringLiteral("row-A"),
                                  QStringLiteral("/repo")));
    QTRY_COMPARE(resolved.count(), 2);
    QVERIFY2(rpc.takeRequest().isEmpty(),
             "the second resolution went to the server instead of being answered from "
             "memory, so it proves nothing about the cache");

    controller->ingestOutput(QByteArrayLiteral("still printing"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));
}


// The workspace refresh republishes every pane's harness, and a refresh follows
// every mutation. So "the harness did not change" has to cost nothing at all:
// the work a real change does includes re-announcing the pane's attach, and an
// attach restarts SPEC 6.6's clock at "attached and silent", which would drag a
// working pane back to Starting several times a minute.
//
// What this can check is that a repeat says nothing and changes nothing, and
// that a real change is still heard. The attach half needs a pane holding a
// live PTY channel, which no unit harness here can build (see the note above
// aPaneReportsItsOutputUnderTheRowIdTheServerAnswered), so that half rests on
// the guard being read, not on this test.
void TstTerminalFactory::repeatingAnUnchangedHarnessDoesNotRestartTheClock()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AgentStatusMonitor monitor;
    OfflineFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    factory.setAgentMonitor(&monitor);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);

    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-1"), QStringLiteral("row-A"),
                                  QStringLiteral("/repo")));
    const QJsonObject first = rpc.takeRequest();
    rpc.answerWithRow(first.value(QStringLiteral("id")).toInt(), QStringLiteral("row-A"),
                      QStringLiteral("s1"), QStringLiteral("terminal-1"),
                      QStringLiteral("ch_s1_row-A"), QStringLiteral("generic"));
    QTRY_COMPARE(resolved.count(), 1);

    monitor.noteTerminalAttached(QStringLiteral("s1"), QStringLiteral("row-A"));
    controller->ingestOutput(QByteArrayLiteral("working"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));

    // What a refresh does, several times over. The pane is already generic, so
    // every one of these has to be a complete no-op: not one word to the
    // monitor, because the words it would say are "this harness again" and
    // "this pane attached again", and the second of those restarts the clock.
    QSignalSpy states(&monitor, &AgentStatusMonitor::agentStateChanged);
    for (int i = 0; i < 5; ++i)
        factory.noteHarnessChanged(QStringLiteral("row-A"), QStringLiteral("generic"));
    QCOMPARE(states.count(), 0);
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));

    // ...and the silence above is not the spy being deaf: a harness that really
    // does change is heard immediately.
    factory.noteHarnessChanged(QStringLiteral("row-A"), QStringLiteral("oh-my-pi"));
    QCOMPARE(states.count(), 1);
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Unknown));
}

// ---- a pane whose remote session died and was silently replaced ------------
//
// `tmux new-session -A` attaches to the session if it exists and CREATES it if
// it does not, and it does not say which it did. So a pane whose remote session
// had died came back as a brand new empty shell: the user's agent, its work and
// its scrollback gone, with nothing on screen even hinting that anything had
// happened. That silence is what made the underlying bug undiagnosable.
//
// The check is a `tmux.listSessions` round trip made AFTER every attach, out of
// band exactly like the kill exec. It reports only on POSITIVE EVIDENCE, and the
// evidence is tmux's own session_created for the pane's target: a session of
// that name was seen alive with one creation time, and the session of that name
// alive now was created LATER. Since tmux never changes a session's creation
// time, a later one is a different session wearing the same name.
//
// What that rules out is the class of lie these cases exist to prevent: a pane
// telling the user their work was destroyed when the client only ever inferred
// that a session had existed.

// A pane coming up for the FIRST time creates its session legitimately — that
// is what a new terminal is — and has lost nothing. It is asked (the answer is
// what records the session it now has) and must not report.
void TstTerminalFactory::aFirstAttachStaysSilentEvenWhenItCreatedTheSession()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setRpcClient(rpc.client());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    const QString target = QStringLiteral("ch_s1_row-A");
    QSignalSpy recreated(&factory, &TerminalFactory::sessionRecreated);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    QVERIFY(resolveAndAttach(factory, rpc, controller, target));

    // Created by this very attach, one second in the future for good measure:
    // there is still nothing to report, because nothing this pane ever observed
    // says it had a session for this one to have replaced.
    QVERIFY(answerSessionProbe(rpc, target,
                               QDateTime::currentSecsSinceEpoch() + 1));
    QTest::qWait(150);
    QCOMPARE(recreated.count(), 0);
    QCOMPARE(errors.count(), 0);
}

// THE BUG. The pane attached once and its session was seen alive; the session
// then died while the client was away, and the re-attach silently made a new
// one — which the listing gives away, because the session now wearing the name
// was created later than the one that was observed.
void TstTerminalFactory::aReattachThatCreatedTheSessionSaysTheOldOneIsGone()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setRpcClient(rpc.client());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    const QString target = QStringLiteral("ch_s1_row-A");
    const qint64 born = QDateTime::currentSecsSinceEpoch() - 3600;
    QVERIFY(resolveAndAttach(factory, rpc, controller, target));
    // The session the pane is working in, observed alive: this is the evidence
    // that there is something to lose.
    QVERIFY(answerSessionProbe(rpc, target, born));
    QTRY_COMPARE(rpc.client()->pendingCount(), 0);

    QSignalSpy recreated(&factory, &TerminalFactory::sessionRecreated);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    // A DIFFERENT session under the same name: the pane's shell, and everything
    // that was running in it, ended.
    QVERIFY(answerSessionProbe(rpc, target, born + 60));

    QTRY_COMPARE(recreated.count(), 1);
    QCOMPARE(recreated.constFirst().at(0).value<TerminalController*>(), controller);
    QCOMPARE(recreated.constFirst().at(1).toString(), target);
    // A diagnostic, not a fault: the pane itself is working.
    QCOMPARE(errors.count(), 0);
}

// The ORDINARY reconnect, which is most of them: the session outlived the
// disconnect and the attach found it. Nothing was lost, so nothing may be said —
// a notice here would appear after every reconnect and teach the user to ignore
// the one that matters.
void TstTerminalFactory::anOrdinaryReconnectToASessionThatWasStillThereStaysSilent()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setRpcClient(rpc.client());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    const QString target = QStringLiteral("ch_s1_row-A");
    QSignalSpy recreated(&factory, &TerminalFactory::sessionRecreated);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    // Deliberately a creation time in the FUTURE by this client's clock, which
    // is what a host whose clock runs ahead reports. The verdict compares tmux's
    // answer with tmux's earlier answer and never with our own wall time, so
    // skew — and tmux's one-second granularity, which no clock race can survive
    // either — cannot manufacture a report of lost work.
    const qint64 born = QDateTime::currentSecsSinceEpoch() + 86400;
    QVERIFY(resolveAndAttach(factory, rpc, controller, target));
    QVERIFY(answerSessionProbe(rpc, target, born));
    QTRY_COMPARE(rpc.client()->pendingCount(), 0);

    // Two more reconnects to the very same session.
    for (int i = 0; i < 2; ++i) {
        QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
        QVERIFY(answerSessionProbe(rpc, target, born));
        QTest::qWait(50);
    }
    QCOMPARE(recreated.count(), 0);
    QCOMPARE(errors.count(), 0);
}

// THE LIE THIS REPLACED, and the regression that keeps it gone. An attach that
// opens a PTY channel is NOT evidence that a tmux session exists: the command
// runs on the far side of a transport that can die between the exec request and
// tmux actually running, and `tmux new-session` can also fail on its own (no
// tmux on the host, an unusable working directory). The predecessor recorded
// "this pane attached once" and treated the next attach that created the
// session as proof that a session had been destroyed — so a pane that had never
// had a session at all announced that the user's work was gone.
//
// The pane here comes up, gets a channel, and the host says there is no such
// session. The next attach really does create it, for the first time. Nothing
// was lost and nothing may be claimed.
void TstTerminalFactory::aPaneThatNeverHadASessionStaysSilentWhenOneIsFinallyCreated()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setRpcClient(rpc.client());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    const QString target = QStringLiteral("ch_s1_row-A");
    QSignalSpy recreated(&factory, &TerminalFactory::sessionRecreated);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    // The channel came up; the session did not. A negative `created` is a
    // listing with no entry for this target, which is exactly what the host
    // reports when the command never made one.
    QVERIFY(resolveAndAttach(factory, rpc, controller, target));
    QVERIFY(answerSessionProbe(rpc, target, -1));
    QTest::qWait(50);

    // The retry works, and this is the session's real birth.
    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    QVERIFY(answerSessionProbe(rpc, target,
                               QDateTime::currentSecsSinceEpoch()));
    QTest::qWait(150);
    QVERIFY2(recreated.count() == 0,
             "a pane that was never observed to have a session told the user their work "
             "had been destroyed");
    QCOMPARE(errors.count(), 0);

    // And the silence above is not a diagnostic that has been switched off: the
    // session just recorded, replaced, is reported.
    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    QVERIFY(answerSessionProbe(rpc, target,
                               QDateTime::currentSecsSinceEpoch() + 300));
    QTRY_COMPARE(recreated.count(), 1);
    QCOMPARE(recreated.constFirst().at(1).toString(), target);
    QCOMPARE(errors.count(), 0);
}

// Every way the diagnostic can fail to produce a verdict, and the one rule that
// covers all of them: say nothing. A failed diagnostic must never become a
// user-facing error, must never stand in the way of a pane that is working, and
// must not throw away what the pane already knows — the report that matters
// comes after the failures, not instead of them.
void TstTerminalFactory::aFailedOrEmptyListingReportsNothingAndRaisesNoError()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setRpcClient(rpc.client());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    const QString target = QStringLiteral("ch_s1_row-A");
    const qint64 born = QDateTime::currentSecsSinceEpoch() - 3600;
    QVERIFY(resolveAndAttach(factory, rpc, controller, target));
    QVERIFY(answerSessionProbe(rpc, target, born));
    QTRY_COMPARE(rpc.client()->pendingCount(), 0);

    QSignalSpy recreated(&factory, &TerminalFactory::sessionRecreated);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    // 1. The host refuses the listing outright.
    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    QJsonObject probe = rpc.takeRequest();
    QCOMPARE(probe.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(rpc::kMethodListSessions));
    rpc.answerWithError(probe.value(QStringLiteral("id")).toInt(),
                        QStringLiteral("tmux: command not found"));
    QTest::qWait(150);
    QCOMPARE(recreated.count(), 0);
    QCOMPARE(errors.count(), 0);

    // 2. The listing answers, but has no entry for this pane's session — a host
    // with no tmux server running answers an empty array, and the session may
    // also have died again since the attach. Either way there is nothing to
    // compare and therefore nothing to say.
    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    QVERIFY(answerSessionProbe(rpc, target, -1));
    QTest::qWait(150);
    QCOMPARE(recreated.count(), 0);
    QCOMPARE(errors.count(), 0);

    // 3. The session is back in the listing, unchanged: the same session all
    // along, and the two silences above did not lose the observation that says
    // so.
    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    QVERIFY(answerSessionProbe(rpc, target, born));
    QTest::qWait(150);
    QCOMPARE(recreated.count(), 0);

    // 4. ...and the observation really did survive them, because a genuinely
    // newer session is still reported afterwards. Without this the three
    // silences above could be a diagnostic that had simply given up.
    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    QVERIFY(answerSessionProbe(rpc, target, born + 60));
    QTRY_COMPARE(recreated.count(), 1);
    QCOMPARE(errors.count(), 0);
}

// The probe is asked on one channel and answered later, so the pane can move
// underneath it. A user who retargets a pane at another terminal in that window
// must not then be told that the terminal they just LEFT is gone: the notice
// would name a session they never lost, in a pane now showing something else.
// The answer is matched against the target the pane is on NOW, not the one the
// question was asked about.
void TstTerminalFactory::aRetargetWhileTheProbeIsInFlightReportsNothing()
{
    RpcPair rpc;
    QVERIFY(rpc.start());

    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setRpcClient(rpc.client());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);

    QObject pane;
    TerminalController* controller = factory.create(&pane);
    const QString first = QStringLiteral("ch_s1_row-A");
    const qint64 born = QDateTime::currentSecsSinceEpoch() - 3600;
    QVERIFY(resolveAndAttach(factory, rpc, controller, first));
    QVERIFY(answerSessionProbe(rpc, first, born));
    QTRY_COMPARE(rpc.client()->pendingCount(), 0);

    QSignalSpy recreated(&factory, &TerminalFactory::sessionRecreated);
    QSignalSpy errors(&factory, &TerminalFactory::error);

    // A re-attach on a pane that HAS an observed session, so this probe would
    // otherwise report.
    QVERIFY(factory.attach(controller, first, QStringLiteral("/repo"), 80, 24));
    const QJsonObject probe = rpc.takeRequest();
    QCOMPARE(probe.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(rpc::kMethodListSessions));

    // The pane moves to another terminal before the listing comes back. Resolved
    // by hand rather than through resolveAndAttach, which hard-codes row-A: this
    // needs a DIFFERENT row, and attach() refuses a target the pane has not
    // resolved.
    const QString second = QStringLiteral("ch_s1_row-B");
    QSignalSpy resolved(&factory, &TerminalFactory::targetResolved);
    QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                  QStringLiteral("terminal-2"), QStringLiteral("row-B"),
                                  QStringLiteral("/repo")));
    const QJsonObject lookup = rpc.takeRequest();
    QCOMPARE(lookup.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(rpc::kMethodWorkspaceResolveTerminalPane));
    rpc.answerWithRow(lookup.value(QStringLiteral("id")).toInt(), QStringLiteral("row-B"),
                      QStringLiteral("s1"), QStringLiteral("terminal-2"), second,
                      QString());
    QVERIFY(resolved.wait(2000));
    QVERIFY(factory.attach(controller, second, QStringLiteral("/repo"), 80, 24));
    // That attach asks its own question, and its answer says the new session was
    // created just now — which reports nothing either, because rememberTarget()
    // forgot what this pane had observed the moment the target moved: nothing is
    // expected to be waiting at a session the pane has not used before.
    QVERIFY(answerSessionProbe(rpc, second,
                               QDateTime::currentSecsSinceEpoch()));

    // The late answer says the FIRST session was replaced, which on a pane still
    // sitting on it is exactly what the notice reports.
    rpc.answerWithSessions(probe.value(QStringLiteral("id")).toInt(),
                           QJsonArray{tmuxSession(first, born + 60)});

    QTest::qWait(150);
    QCOMPARE(recreated.count(), 0);
    QCOMPARE(errors.count(), 0);
}

void TstTerminalFactory::anOlderAttachmentProbeCannotReportForANewerChannel()
{
    RpcPair rpc;
    QVERIFY(rpc.start());
    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setRpcClient(rpc.client());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    QObject pane;
    TerminalController* controller = factory.create(&pane);
    const QString target = QStringLiteral("ch_s1_row-A");
    const qint64 born = QDateTime::currentSecsSinceEpoch() - 3600;
    QVERIFY(resolveAndAttach(factory, rpc, controller, target));
    QVERIFY(answerSessionProbe(rpc, target, born));
    QTRY_COMPARE(rpc.client()->pendingCount(), 0);
    // A real replacement establishes that the first observation was consumed.
    QSignalSpy recreated(&factory, &TerminalFactory::sessionRecreated);
    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    QVERIFY(answerSessionProbe(rpc, target, born + 60));
    QTRY_COMPARE(recreated.count(), 1);
    recreated.clear();

    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    const QJsonObject oldProbe = rpc.takeRequest();
    QVERIFY(factory.attach(controller, target, QStringLiteral("/repo"), 80, 24));
    const QJsonObject currentProbe = rpc.takeRequest();
    QVERIFY(!oldProbe.isEmpty());
    QVERIFY(!currentProbe.isEmpty());
    rpc.answerWithSessions(oldProbe.value(QStringLiteral("id")).toInt(),
                           QJsonArray{tmuxSession(target, born + 120)});
    // Observe a whole response turn: a retired channel must not raise a
    // notice before the current attachment's own probe has answered.
    QTest::qWait(50);
    QCOMPARE(recreated.count(), 0);
    rpc.answerWithSessions(currentProbe.value(QStringLiteral("id")).toInt(),
                           QJsonArray{tmuxSession(target, born + 120)});
    QTRY_COMPARE(recreated.count(), 1);
}


void TstTerminalFactory::outputDuringChannelOpenIsNotResetToStarting()
{
    RpcPair rpc;
    QVERIFY(rpc.start());
    AgentStatusMonitor monitor;
    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    factory.setAgentMonitor(&monitor);
    TerminalController* controller = factory.create();
    connect(controller, &TerminalController::stateChanged, controller,
            [controller](TerminalState state) {
                if (state == TerminalState::OpeningChannel)
                    controller->ingestOutput(QByteArrayLiteral("first screen"));
            });
    QVERIFY(resolveAndAttach(factory, rpc, controller, QStringLiteral("ch_s1_row-A"),
                             QStringLiteral("generic")));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));
}

void TstTerminalFactory::endedTerminalObservationAcceptsRemoteActivity_data()
{
    QTest::addColumn<QString>("ending");
    QTest::newRow("remote-eof") << QStringLiteral("eof");
    QTest::newRow("attach-error") << QStringLiteral("error");
    QTest::newRow("destroyed-pane") << QStringLiteral("destroy");
    QTest::newRow("retargeted-pane") << QStringLiteral("retarget");
    QTest::newRow("server-switch") << QStringLiteral("server");
}

void TstTerminalFactory::endedTerminalObservationAcceptsRemoteActivity()
{
    QFETCH(QString, ending);
    RpcPair rpc;
    QVERIFY(rpc.start());
    AgentStatusMonitor monitor;
    AttachingFactory factory(nullptr);
    factory.setWorkspace(rpc.db());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setWorkspaceReady(true);
    factory.setAgentMonitor(&monitor);
    TerminalController* controller = factory.create();
    QVERIFY(resolveAndAttach(factory, rpc, controller, QStringLiteral("ch_s1_row-A"),
                             QStringLiteral("generic")));
    controller->ingestOutput(QByteArrayLiteral("working"));
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));
    QSignalSpy states(&factory, &TerminalFactory::terminalStateChanged);
    if (ending == QLatin1String("eof")) {
        QVERIFY(QMetaObject::invokeMethod(controller->transport(), "readChannelFinished",
                                         Qt::DirectConnection));
        QVERIFY(controller->state() == TerminalState::Disconnected);
    } else if (ending == QLatin1String("error")) {
        controller->setState(TerminalState::Error);
    } else if (ending == QLatin1String("destroy")) {
        delete controller;
        QVERIFY(!states.isEmpty());
        QVERIFY(states.last().at(3).value<TerminalState>() == TerminalState::Unloaded);
    } else if (ending == QLatin1String("retarget")) {
        QVERIFY(factory.resolveTarget(controller, QStringLiteral("s1"),
                                      QStringLiteral("terminal-2"), QStringLiteral("row-B"),
                                      QStringLiteral("/repo")));
    } else {
        factory.setServerId(QStringLiteral("srv-2"));
    }
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Unknown));
    monitor.noteRemoteActivity(QStringLiteral("s1"), QStringLiteral("row-A"), 0);
    QCOMPARE(monitor.stateFor(QStringLiteral("s1"), QStringLiteral("row-A")),
             asInt(AgentState::Running));
}
QTEST_GUILESS_MAIN(TstTerminalFactory)
#include "tst_terminalfactory.moc"
