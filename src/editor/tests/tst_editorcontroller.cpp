#include "CodeharbordClient.h"
#include "EditorController.h"
#include "EditorFactory.h"
#include "RpcTypes.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QString>
#include <QtTest/QtTest>

#include <optional>

using ch::CodeharbordClient;
using ch::EditorController;
using ch::EditorFactory;

namespace {

QByteArray jsonLine(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

QString method(const QJsonObject& req)
{
    return req.value(QStringLiteral("method")).toString();
}

int reqId(const QJsonObject& req)
{
    return req.value(QStringLiteral("id")).toInt();
}

QString reqPath(const QJsonObject& req)
{
    return req.value(QStringLiteral("params")).toObject().value(QStringLiteral("path")).toString();
}

QString reqExpectedRevision(const QJsonObject& req)
{
    return req.value(QStringLiteral("params"))
        .toObject()
        .value(QStringLiteral("expectedRevision"))
        .toString();
}

QString reqContent(const QJsonObject& req)
{
    return req.value(QStringLiteral("params")).toObject().value(QStringLiteral("content")).toString();
}

QString reqSubscriptionId(const QJsonObject& req)
{
    return req.value(QStringLiteral("params"))
        .toObject()
        .value(QStringLiteral("subscriptionId"))
        .toString();
}

qint64 reqOffset(const QJsonObject& req)
{
    return req.value(QStringLiteral("params"))
        .toObject()
        .value(QStringLiteral("offset"))
        .toInteger(-1);
}

qint64 reqLength(const QJsonObject& req)
{
    return req.value(QStringLiteral("params"))
        .toObject()
        .value(QStringLiteral("length"))
        .toInteger(-1);
}

// The paneId the test controller is keyed by, and the server-reported recovery
// base pushed into it via setRecoveryDir(): its snapshot lives at
// recoveryFilePath() — one file per pane under that base (SPEC 11.3).
const QString kPaneId = QStringLiteral("viewer-1");
const QString kRecoveryBase = QStringLiteral("/srv/data/codeharbor/recovery");

QString recoveryFilePath()
{
    return kRecoveryBase + QLatin1Char('/') + kPaneId;
}

int reqMode(const QJsonObject& req)
{
    return req.value(QStringLiteral("params"))
        .toObject()
        .value(QStringLiteral("mode"))
        .toInt(-1);
}

// The snapshot is a JSON envelope carrying the file path alongside the buffer
// (EditorController::serializeRecovery), so a pane can tell whose file a reused
// per-pane snapshot slot holds. These build and read that envelope.
QString snapshotEnvelope(const QString& path, const QString& content)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                  {QStringLiteral("content"), content}})
            .toJson(QJsonDocument::Compact));
}

QString snapshotContentOf(const QJsonObject& writeReq)
{
    const QString raw = reqContent(writeReq);
    if (raw.isEmpty())
        return QString();
    return QJsonDocument::fromJson(raw.toUtf8())
        .object()
        .value(QStringLiteral("content"))
        .toString();
}

QString snapshotPathOf(const QJsonObject& writeReq)
{
    const QString raw = reqContent(writeReq);
    if (raw.isEmpty())
        return QString();
    return QJsonDocument::fromJson(raw.toUtf8())
        .object()
        .value(QStringLiteral("path"))
        .toString();
}

const auto kReadFile = QString::fromLatin1(ch::rpc::kMethodReadFile);
const auto kWriteFile = QString::fromLatin1(ch::rpc::kMethodWriteFile);
const auto kWatch = QString::fromLatin1(ch::rpc::kMethodWatch);
const auto kUnwatch = QString::fromLatin1(ch::rpc::kMethodUnwatch);
const auto kStat = QString::fromLatin1(ch::rpc::kMethodStat);
const auto kWatchEvent = QString::fromLatin1(ch::rpc::kWatchEventNotification);
const auto kWatchEventsLost =
    QString::fromLatin1(ch::rpc::kWatchEventsLostNotification);

} // namespace

// Drives EditorController against a QLocalSocket pair where the TEST plays the
// codeharbord server (same pattern as tst_rpcclient::makePair): the controller
// issues RPC requests on its transport socket; the test reads those framed
// requests and writes canned responses/notifications on the other socket.
class TstEditorController : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void openLoadsCleanAndContent();
    void readWithoutRevisionIsRejected();
    void saveSuccessAdoptsRevisionAndCleans();
    void saveStaleRevisionConflictsNoOverwrite();
    void saveConflictFallsBackToStat();
    void saveSuccessWithoutRevisionIsAnError();
    void saveConflictStatIsDroppedAfterANewSave();
    void externalChangeWhileCleanReloads();
    void externalChangeWhileDirtyDoesNotReload();

    // SPEC 8.7 / file.watchEventsLost: the daemon dropped watch notifications
    // for this subscription because our end of the channel stalled, so the
    // buffer is known-stale and must be re-read — under exactly the rules an
    // ordinary change obeys, and only for OUR subscription id.
    void lostWatchEventsForThisSubscriptionReload();
    void lostWatchEventsWhileDirtyOnlyFlagTheBuffer();
    void lostWatchEventsForAnotherSubscriptionAreIgnored();
    void malformedWatchLossPayloadsAreNoOps();
    void lostWatchEventsAfterUnwatchingAreIgnored();
    void recoverySnapshotWrittenAndOfferedOnReopen();
    void unwatchIssuedOnDestruction();
    void reopenUnwatchesPreviousSubscription();
    void contentBufferedUntilPageReportsReady();
    void recoveryDirArrivingAfterOpenStillOffersSnapshot();
    void successfulSaveClearsRecoverySnapshot();

    // SPEC 8.2 read-only. Nothing in the tree used to DERIVE read-only-ness, so
    // a file the user cannot write opened freely editable and only failed at
    // save time. These pin the derivation, its two inputs, and the fact that it
    // is re-asked rather than latched.
    void unwritableFileOpensReadOnlyAndRefusesSave();
    void binaryFileOpensReadOnly();
    void readOnlyIsRederivedOnReloadAndNotLatched();
    void reconnectRederivesReadOnlyFromTheReconcileStat();
    void readOnlyReachesAPageThatConnectsLate();
    // X12: the server's `writable` flag (C2) is authoritative over `mode`.
    void writableFlagOverridesUnwritableMode();
    void unwritableFlagForcesReadOnlyOverWritableMode();

    // SPEC 5.6 reconnect: the transport is swapped for a channel onto a BRAND
    // NEW codeharbord whose file.watch registry has never heard of us.
    void detachingTransportNeverResubscribes();
    void reconnectResubscribesAndReconcilesCleanBuffer();
    void reconnectLeavesADirtyBufferAloneAndFlagsIt();
    void reconnectOverAnUnansweredWatchSubscribesExactlyOnce();
    void reconcileLosesToASaveThatSettledFirst();
    void transportDropParksTheFileStateUntilRebind();

    // The buffer's dirty flag is what stops an external change from being
    // auto-reloaded over unsaved work (SPEC 8.7), so every path that can leave
    // bytes only in the page has to raise it.
    void failedSaveLeavesTheBufferDirty();
    void saveWhileLoadingIsRefused();
    void editsDuringASaveSurviveTheSaveReply();
    void revertingDuringASaveStaysDirty();
    void editsTypedDuringASystemReloadAreNotClobbered();
    void aSaveInFlightIsNotClobberedByASystemReload();
    void aSaveReplyIsDroppedAfterAnExplicitReloadTookOverTheBuffer();
    void aSaveConflictIsDroppedAfterASamePathReopenTookOverTheBuffer();
    void aSecondSaveWhileRecoveryClearIsInFlightDoesNotDuplicateIt();
    void recoveryClearIntentSurvivesOverlappingSnapshot();
    void aSuccessfulSaveStillClearsASnapshotThatWasStillInFlight();
    void aReportAfterASaveCancelsTheTruncateThatSaveDeferred();
    void aDeferredTruncateSurvivesTheSnapshotRetryChain();
    void aStaleSnapshotReplyDoesNotDisturbTheNextFilesBookkeeping();
    void aSecondSaveOfTheSameBufferIsNotASecondWriteAndNotAConflict();
    void aSecondSaveOfChangedBytesIsRewrittenAgainstTheRevisionItNowNeeds();
    void aQueuedSaveIsDroppedWhenTheWriteItWaitedOnIsRefused();
    void aSaveOnANewFileDoesNotJoinThePreviousFilesSaveChain();
    void anUnownedControllerIsKeptAliveByTheFactory();
    void aBase64ReadIsDecodedBeforeItReachesThePage();
    void anUndecodableBase64ReadIsAReadFailure();
    void anExplicitReloadDiscardsTheRecoverySnapshotItThrewAway();
    void aPageReloadWithUnsavedWorkIsOfferedItsRecoverySnapshot();
    void reportContentDuringALoadIsIgnored();
    void reportIdenticalContentDoesNotStayDirty();
    void aSaveIssuedAfterAnExplicitReloadIsNotSwallowedByTheOldWrite();
    void aPageReloadDuringAnOutageIsServedWhenTheTransportReturns();
    void aSaveWithNothingToWriteToIsRefusedWithAReason();
    void recoveryIdArrivingAfterOpenStillOffersSnapshot();
    void aSnapshotFromAPreviousFileIsNotOfferedAsThisOnes();
    void aBase64SnapshotIsDecodedBeforeItIsOffered();

    // Out-of-order / superseded replies must never land on the file now open.
    void staleLoadReplyNeverWins();
    void saveReplyAfterAFileSwitchIsDropped();

    void truncatedReadIsNotWritableBack();
    void conflictKeepsItsStateWhenTheWatchEventLands();
    void lateReadyReplaysTheFileState();

    // SPEC 8.3 read ceiling: every file.readFile carries a byte window, and a
    // file too big to fit it is not opened for editing at all.
    void readsAskForABoundedWindow();
    void aTruncatedFileIsNotOpenedForEditing();

    // SPEC 8.7: a file deleted while the session was down produces no watch
    // event anywhere, so only the reconnect's file.stat can discover it.
    void deletedWhileDisconnectedStopsClaimingClean();
    void deletedWhileDisconnectedWithUnsavedWorkIsOnlyFlagged();

private:
    void makePair();
    QJsonObject nextRequest(int timeoutMs = 3000);
    void respondResult(int id, const QJsonObject& result);
    void respondError(int id, int code, const QString& message,
                      const QJsonValue& data = QJsonValue());
    void sendNotification(const QString& m, const QJsonObject& params);
    // Drain the watch subscription + (absent) recovery stat that follow a load.
    void serveWatchThenNoRecovery();
    // Answer the file.stat every load issues to derive read-only-ness (SPEC
    // 8.2). Defaults to a plainly writable regular file.
    void servePermissionStat(int mode = 0644);
    // ready() (the page is connected, so content flows straight through) then
    // open(path) -> readFile(content,rev) -> Clean, plus watch + empty recovery.
    void openClean(const QString& path, const QString& content, const QString& rev);
    // Swap the transport the way SessionBootstrap does on a SPEC 5.6 reconnect:
    // EOF on the dying channel while the consumer is STILL attached, then
    // detach, then bind a fresh pair standing in for the replacement server.
    void reconnectTransport();

    QLocalServer* m_server = nullptr;
    QLocalSocket* m_clientSide = nullptr;
    QLocalSocket* m_serverSide = nullptr;
    QByteArray m_serverBuf;
    CodeharbordClient* m_client = nullptr;
    EditorController* m_controller = nullptr;
    static int s_seq;
};

int TstEditorController::s_seq = 0;

void TstEditorController::init()
{
    m_client = new CodeharbordClient;
    m_controller = new EditorController(m_client, kPaneId);
    // Server-reported recovery base (server.info.recoveryDir) that EditorFactory
    // would push in; set directly here so recoveryPath() is non-empty and the
    // recovery round trips below actually fire.
    m_controller->setRecoveryDir(kRecoveryBase);
}

void TstEditorController::cleanup()
{
    delete m_controller;
    m_controller = nullptr;
    delete m_client;
    m_client = nullptr;
    delete m_serverSide;
    m_serverSide = nullptr;
    delete m_clientSide;
    m_clientSide = nullptr;
    delete m_server;
    m_server = nullptr;
    m_serverBuf.clear();
}

void TstEditorController::makePair()
{
    const QString name = QStringLiteral("ch_editor_test_%1_%2")
                             .arg(QCoreApplication::applicationPid())
                             .arg(++s_seq);
    QLocalServer::removeServer(name);

    m_server = new QLocalServer;
    QVERIFY(m_server->listen(name));

    m_clientSide = new QLocalSocket;
    m_clientSide->connectToServer(name);
    QVERIFY(m_clientSide->waitForConnected(2000));
    QVERIFY(m_server->waitForNewConnection(2000));
    m_serverSide = m_server->nextPendingConnection();
    QVERIFY(m_serverSide != nullptr);

    m_client->setTransport(m_clientSide);
}

QJsonObject TstEditorController::nextRequest(int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    forever {
        // Pump the event loop FIRST, every iteration. This lets the client both
        // flush its outgoing requests AND process any responses the server has
        // already written since the last call (e.g. the file.watch reply that
        // records the subscription id). Returning buffered requests without a
        // pump would strand those responses unprocessed, so a subscribe issued
        // just before a teardown/reopen would never be recorded (SPEC 8.7).
        QTest::qWait(5);
        if (m_serverSide)
            m_serverBuf += m_serverSide->readAll();
        const int nl = m_serverBuf.indexOf('\n');
        if (nl >= 0) {
            const QByteArray raw = m_serverBuf.left(nl);
            m_serverBuf.remove(0, nl + 1);
            return QJsonDocument::fromJson(raw).object();
        }
        if (deadline.hasExpired())
            break;
    }
    return {};
}

void TstEditorController::respondResult(int id, const QJsonObject& result)
{
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    m_serverSide->flush();
}

void TstEditorController::respondError(int id, int code, const QString& message,
                                       const QJsonValue& data)
{
    QJsonObject err{{"code", code}, {"message", message}};
    if (!data.isNull())
        err.insert(QStringLiteral("data"), data);
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"error", err}}));
    m_serverSide->flush();
}

void TstEditorController::sendNotification(const QString& m, const QJsonObject& params)
{
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"method", m}, {"params", params}}));
    m_serverSide->flush();
}

void TstEditorController::servePermissionStat(int mode)
{
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QVERIFY2(!reqPath(stat).contains(QStringLiteral("/recovery/")),
             "the permission stat must target the file itself, not the recovery snapshot");
    respondResult(reqId(stat), {{"path", reqPath(stat)},
                                {"kind", "file"},
                                {"size", 0},
                                {"mtimeMs", 0},
                                {"mode", mode},
                                {"revision", "rperm"}});
}

void TstEditorController::serveWatchThenNoRecovery()
{
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});

    // Every load re-derives read-only from the server's view of the file
    // (SPEC 8.2); the recovery probe is chained BEHIND it.
    servePermissionStat();

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), recoveryFilePath());
    respondError(reqId(stat), -32002, QStringLiteral("ENOENT")); // no snapshot
}

void TstEditorController::openClean(const QString& path, const QString& content,
                                    const QString& rev)
{
    // Stand in for the WebChannel editor page finishing its handshake. Without
    // it contentLoaded is HELD (see contentBufferedUntilPageReportsReady), which
    // is exactly the first-buffer loss the handshake exists to prevent.
    m_controller->ready();
    m_controller->open(path);
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), path);
    respondResult(reqId(read), {{"path", path},
                                {"encoding", "utf-8"},
                                {"content", content},
                                {"revision", rev},
                                {"truncated", false}});
    serveWatchThenNoRecovery();
}

void TstEditorController::openLoadsCleanAndContent()
{
    makePair();

    QSignalSpy stateSpy(m_controller, &EditorController::fileStateChanged);
    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QTRY_COMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(), QStringLiteral("hello"));
    QCOMPARE(contentSpy.at(0).at(1).toString(), QStringLiteral("r1"));

    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));

    // Loading -> Clean was observed.
    QVERIFY(stateSpy.count() >= 2);
    QCOMPARE(stateSpy.first().at(0).toString(), QStringLiteral("loading"));
    QCOMPARE(stateSpy.last().at(0).toString(), QStringLiteral("clean"));
}
void TstEditorController::readWithoutRevisionIsRejected()
{
    makePair();
    m_controller->ready();

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    m_controller->open(QStringLiteral("/foo/f.txt"));
    const QJsonObject read = nextRequest();
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "hello"},
                                {"truncated", false}});

    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("error"));
    QCOMPARE(contentSpy.count(), 0);
    QVERIFY2(nextRequest(300).isEmpty(),
             "a read without a revision token continued as an editable load");
}


void TstEditorController::saveSuccessAdoptsRevisionAndCleans()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy stateSpy(m_controller, &EditorController::fileStateChanged);

    m_controller->save(QStringLiteral("world"), QStringLiteral("r1"));

    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqPath(write), QStringLiteral("/foo/f.txt"));
    QCOMPARE(reqExpectedRevision(write), QStringLiteral("r1"));
    QCOMPARE(reqContent(write), QStringLiteral("world"));
    respondResult(reqId(write),
                  {{"path", "/foo/f.txt"}, {"revision", "r2"}});

    QTRY_COMPARE(savedSpy.count(), 1);
    QCOMPARE(savedSpy.at(0).at(0).toString(), QStringLiteral("r2"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r2"));
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // Saving -> Saved -> Clean transitions were emitted.
    QStringList seen;
    for (const auto& args : stateSpy)
        seen << args.at(0).toString();
    QVERIFY(seen.contains(QStringLiteral("saving")));
    QVERIFY(seen.contains(QStringLiteral("saved")));
    QVERIFY(seen.contains(QStringLiteral("clean")));
}
void TstEditorController::saveSuccessWithoutRevisionIsAnError()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy errorSpy(m_controller, &EditorController::saveError);

    m_controller->save(QStringLiteral("world"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    respondResult(reqId(write), {{"path", "/foo/f.txt"}});

    QTRY_COMPARE(errorSpy.count(), 1);
    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("error"));
}


void TstEditorController::saveStaleRevisionConflictsNoOverwrite()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    QSignalSpy savedSpy(m_controller, &EditorController::saved);

    m_controller->save(QStringLiteral("stale write"), QStringLiteral("r1"));

    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    // Server rejects: the file moved to r9 since load (SPEC 8.6).
    respondError(reqId(write), ch::rpc::kRevisionMismatch,
                 QStringLiteral("stale revision"),
                 QJsonObject{{"currentRevision", "r9"}});

    QTRY_COMPARE(conflictSpy.count(), 1);
    QCOMPARE(conflictSpy.at(0).at(0).toString(), QStringLiteral("r9"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("conflict"));
    // NEVER silently overwritten: revision baseline unchanged, no saved signal.
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
    QCOMPARE(savedSpy.count(), 0);
}

void TstEditorController::saveConflictFallsBackToStat()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);

    m_controller->save(QStringLiteral("stale"), QStringLiteral("r1"));

    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    // Conflict WITHOUT a currentRevision in the error data -> controller stats.
    respondError(reqId(write), ch::rpc::kRevisionMismatch,
                 QStringLiteral("stale revision"));

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(stat), {{"path", "/foo/f.txt"}, {"revision", "r9"}});

    QTRY_COMPARE(conflictSpy.count(), 1);
    QCOMPARE(conflictSpy.at(0).at(0).toString(), QStringLiteral("r9"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("conflict"));
}
// A conflict fallback stats the file on a second asynchronous hop. If the user
// starts a new save before that stat answers, the old stat must not turn the new
// save into a conflict.
void TstEditorController::saveConflictStatIsDroppedAfterANewSave()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    QSignalSpy savedSpy(m_controller, &EditorController::saved);

    m_controller->save(QStringLiteral("first"), QStringLiteral("r1"));
    const QJsonObject first = nextRequest();
    QCOMPARE(method(first), kWriteFile);
    respondError(reqId(first), ch::rpc::kRevisionMismatch,
                 QStringLiteral("stale revision"));

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), QStringLiteral("/foo/f.txt"));

    // A fresh save is a new generation, even though it uses the same loaded
    // revision. Its success must own the pane's state.
    m_controller->save(QStringLiteral("second"), QStringLiteral("r1"));
    const QJsonObject second = nextRequest();
    QCOMPARE(method(second), kWriteFile);
    QCOMPARE(reqContent(second), QStringLiteral("second"));
    respondResult(reqId(second), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTRY_COMPARE(savedSpy.count(), 1);
    QCOMPARE(m_controller->revision(), QStringLiteral("r2"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // The first save's late stat describes the pre-second-save world and must
    // be ignored.
    respondResult(reqId(stat), {{"path", "/foo/f.txt"}, {"revision", "r9"}});
    QTest::qWait(100);
    QCOMPARE(conflictSpy.count(), 0);
    QCOMPARE(savedSpy.count(), 1);
    QCOMPARE(m_controller->revision(), QStringLiteral("r2"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
}

void TstEditorController::externalChangeWhileCleanReloads()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    QSignalSpy stateSpy(m_controller, &EditorController::fileStateChanged);

    // Watch fires: file changed externally to r5 while our buffer is clean.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r5"}});

    // Controller auto-reloads: expect a fresh readFile.
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "externally edited"},
                                {"revision", "r5"},
                                {"truncated", false}});
    servePermissionStat();

    QTRY_COMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(), QStringLiteral("externally edited"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r5"));
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    QStringList seen;
    for (const auto& args : stateSpy)
        seen << args.at(0).toString();
    QVERIFY(seen.contains(QStringLiteral("externally_modified")));
    QVERIFY(seen.contains(QStringLiteral("clean")));
}

void TstEditorController::externalChangeWhileDirtyDoesNotReload()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    // Local unsaved edit -> recovery snapshot write (create-only).
    m_controller->reportContent(QStringLiteral("local edits"));
    const QJsonObject rec = nextRequest();
    QCOMPARE(method(rec), kWriteFile);
    QCOMPARE(reqPath(rec), recoveryFilePath());
    QCOMPARE(reqExpectedRevision(rec), QString()); // create-only
    respondResult(reqId(rec), {{"path", reqPath(rec)}, {"revision", "rec1"}});
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    // Watch fires while dirty: must NOT reload (would clobber local edits).
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r5"}});

    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("externally_modified"));
    // No reload request, no content reload, baseline revision unchanged.
    const QJsonObject none = nextRequest(300);
    QVERIFY(none.isEmpty());
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
}

// The daemon dropped watch events for OUR subscription. No path, no revision
// and no event kind survive that, so the only honest reaction is to re-read the
// watched file — the same re-read an ordinary change triggers.
void TstEditorController::lostWatchEventsForThisSubscriptionReload()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    QSignalSpy stateSpy(m_controller, &EditorController::fileStateChanged);

    sendNotification(kWatchEventsLost,
                     {{"subscriptionIds", QJsonArray{QStringLiteral("sub1")}}});

    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "the change we never heard about"},
                                {"revision", "r7"},
                                {"truncated", false}});
    servePermissionStat();

    QTRY_COMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(),
             QStringLiteral("the change we never heard about"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r7"));
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    QStringList seen;
    for (const auto& args : stateSpy)
        seen << args.at(0).toString();
    QVERIFY(seen.contains(QStringLiteral("externally_modified")));
    QVERIFY(seen.contains(QStringLiteral("clean")));
}

// A lost-events notification must never cost the user unsaved work: against a
// dirty buffer it takes the same route an ordinary change takes — flag the pane
// externally modified and let the user resolve it — and issues no read at all.
void TstEditorController::lostWatchEventsWhileDirtyOnlyFlagTheBuffer()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("local edits"));
    const QJsonObject rec = nextRequest();
    QCOMPARE(method(rec), kWriteFile);
    QCOMPARE(reqPath(rec), recoveryFilePath());
    respondResult(reqId(rec), {{"path", reqPath(rec)}, {"revision", "rec1"}});
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    sendNotification(kWatchEventsLost,
                     {{"subscriptionIds", QJsonArray{QStringLiteral("sub1")}}});

    // Exactly what externalChangeWhileDirtyDoesNotReload asserts for the
    // ordinary change path.
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("externally_modified"));
    const QJsonObject none = nextRequest(300);
    QVERIFY(none.isEmpty());
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
}

// One daemon serves every pane, so the notification names subscriptions this
// controller does not own. Those are somebody else's problem: no read, no state
// change here.
void TstEditorController::lostWatchEventsForAnotherSubscriptionAreIgnored()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    sendNotification(kWatchEventsLost,
                     {{"subscriptionIds", QJsonArray{QStringLiteral("sub-someone-else"),
                                                     QStringLiteral("sub99")}}});

    const QJsonObject none = nextRequest(300);
    QVERIFY(none.isEmpty());
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
}

// Untrusted bytes off a socket. Every broken shape is inert, and the controller
// is still listening afterwards — the final valid notification proves the
// silence above was rejection, not a wedged handler.
void TstEditorController::malformedWatchLossPayloadsAreNoOps()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    sendNotification(kWatchEventsLost, {});                         // field missing
    sendNotification(kWatchEventsLost, {{"subscriptionIds", "sub1"}}); // not an array
    sendNotification(kWatchEventsLost,
                     {{"subscriptionIds", QJsonObject{{"0", "sub1"}}}}); // object
    sendNotification(kWatchEventsLost, {{"subscriptionIds", QJsonArray{}}}); // empty
    sendNotification(kWatchEventsLost,
                     {{"subscriptionIds", QJsonArray{7, QJsonValue()}}}); // non-strings

    const QJsonObject none = nextRequest(300);
    QVERIFY(none.isEmpty());
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));

    sendNotification(kWatchEventsLost,
                     {{"subscriptionIds", QJsonArray{QStringLiteral("sub1")}}});
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
}

// The subscription this pane held is released when it switches files, so the
// id the daemon reports as lost is no longer ours. Acting on it would re-read a
// file this pane no longer shows.
void TstEditorController::lostWatchEventsAfterUnwatchingAreIgnored()
{
    makePair();
    openClean(QStringLiteral("/foo/a.txt"), QStringLiteral("A"),
              QStringLiteral("r1")); // subscribes "sub1"

    m_controller->open(QStringLiteral("/foo/b.txt"));

    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    QCOMPARE(reqSubscriptionId(unwatch), QStringLiteral("sub1"));

    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/b.txt"},
                                {"encoding", "utf-8"},
                                {"content", "B"},
                                {"revision", "r2"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});
    servePermissionStat();
    const QJsonObject recStat = nextRequest();
    QCOMPARE(method(recStat), kStat);
    QCOMPARE(reqPath(recStat), recoveryFilePath());
    respondError(reqId(recStat), -32002, QStringLiteral("ENOENT"));
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    // The released id: inert.
    sendNotification(kWatchEventsLost,
                     {{"subscriptionIds", QJsonArray{QStringLiteral("sub1")}}});
    const QJsonObject none = nextRequest(300);
    QVERIFY(none.isEmpty());
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // The live one still works, so the pane is not simply deaf.
    sendNotification(kWatchEventsLost,
                     {{"subscriptionIds", QJsonArray{QStringLiteral("sub2")}}});
    const QJsonObject reread = nextRequest();
    QCOMPARE(method(reread), kReadFile);
    QCOMPARE(reqPath(reread), QStringLiteral("/foo/b.txt"));
}

void TstEditorController::recoverySnapshotWrittenAndOfferedOnReopen()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    // Edit -> snapshot the unsaved buffer to the recovery path (SPEC 11.3).
    m_controller->reportContent(QStringLiteral("recovered edits"));
    const QJsonObject rec = nextRequest();
    QCOMPARE(method(rec), kWriteFile);
    const QString recoveryPath = reqPath(rec);
    QCOMPARE(recoveryPath, recoveryFilePath());
    QCOMPARE(snapshotContentOf(rec), QStringLiteral("recovered edits"));
    QCOMPARE(snapshotPathOf(rec), QStringLiteral("/foo/f.txt"));
    QCOMPARE(reqMode(rec), 0600);
    QCOMPARE(reqExpectedRevision(rec), QString()); // first write is create-only
    respondResult(reqId(rec), {{"path", recoveryPath}, {"revision", "rec1"}});

    // Re-open the same file: the on-disk file is unchanged, but a differing
    // recovery snapshot exists -> recoveryAvailable is offered.
    QSignalSpy recoverySpy(m_controller, &EditorController::recoveryAvailable);

    m_controller->open(QStringLiteral("/foo/f.txt"));
    // Re-open unwatches the previous file's subscription first (SPEC 8.7): no
    // leaked/duplicated watcher across the switch.
    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    QCOMPARE(reqSubscriptionId(unwatch), QStringLiteral("sub1"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "orig"},
                                {"revision", "r1"},
                                {"truncated", false}});

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    // Writable, so the recovery probe behind it goes ahead.
    servePermissionStat();

    // Recovery stat: snapshot present this time.
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), recoveryPath);
    respondResult(reqId(stat), {{"path", recoveryPath}, {"revision", "rec1"}});

    // Controller reads the snapshot to compare against the loaded file.
    const QJsonObject recRead = nextRequest();
    QCOMPARE(method(recRead), kReadFile);
    QCOMPARE(reqPath(recRead), recoveryPath);
    respondResult(reqId(recRead), {{"path", recoveryPath},
                                   {"encoding", "utf-8"},
                                   {"content", snapshotEnvelope(QStringLiteral("/foo/f.txt"), QStringLiteral("recovered edits"))},
                                   {"revision", "rec1"},
                                   {"truncated", false}});

    QTRY_COMPARE(recoverySpy.count(), 1);
    QCOMPARE(recoverySpy.at(0).at(0).toString(), QStringLiteral("recovered edits"));
}

// The server identity can arrive after a pane has already loaded its file. Once
// the recovery directory becomes known, an existing snapshot must be probed
// without requiring the user to reopen the pane.
void TstEditorController::recoveryDirArrivingAfterOpenStillOffersSnapshot()
{
    makePair();
    m_controller->setRecoveryDir(QString());
    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/f.txt"));

    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "orig"},
                                {"revision", "r1"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});
    servePermissionStat();
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    QSignalSpy recoverySpy(m_controller, &EditorController::recoveryAvailable);
    m_controller->setRecoveryDir(kRecoveryBase);

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), recoveryFilePath());
    respondResult(reqId(stat), {{"path", recoveryFilePath()}, {"revision", "rec1"}});

    const QJsonObject recRead = nextRequest();
    QCOMPARE(method(recRead), kReadFile);
    respondResult(reqId(recRead), {{"path", recoveryFilePath()},
                                   {"encoding", "utf-8"},
                                   {"content", snapshotEnvelope(
                                                    QStringLiteral("/foo/f.txt"),
                                                    QStringLiteral("recovered"))},
                                   {"revision", "rec1"},
                                   {"truncated", false}});

    QTRY_COMPARE(recoverySpy.count(), 1);
    QCOMPARE(recoverySpy.at(0).at(0).toString(), QStringLiteral("recovered"));
}

void TstEditorController::unwatchIssuedOnDestruction()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));
    // The load subscribed a watcher ("sub1"). Destroying the controller (as a
    // pane close would) MUST release it — otherwise the server-side watch leaks.
    delete m_controller;
    m_controller = nullptr;

    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    QCOMPARE(reqSubscriptionId(unwatch), QStringLiteral("sub1"));
}

void TstEditorController::reopenUnwatchesPreviousSubscription()
{
    makePair();
    openClean(QStringLiteral("/foo/a.txt"), QStringLiteral("A"),
              QStringLiteral("r1")); // subscribes "sub1"

    // Switch to a different file: the previous file's watcher is released first
    // (before the new read) so subscriptions are never leaked or duplicated.
    m_controller->open(QStringLiteral("/foo/b.txt"));

    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    QCOMPARE(reqSubscriptionId(unwatch), QStringLiteral("sub1"));

    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/b.txt"));
    respondResult(reqId(read), {{"path", "/foo/b.txt"},
                                {"encoding", "utf-8"},
                                {"content", "B"},
                                {"revision", "r2"},
                                {"truncated", false}});

    // The new file subscribes its own watcher ("sub2").
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    QCOMPARE(reqPath(watch), QStringLiteral("/foo/b.txt"));
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    servePermissionStat();

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), recoveryFilePath());
    respondError(reqId(stat), -32002, QStringLiteral("ENOENT")); // no snapshot

    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(m_controller->path(), QStringLiteral("/foo/b.txt"));

    // Destroying now releases the NEW subscription ("sub2"), proving the switch
    // adopted it rather than clinging to the stale "sub1".
    delete m_controller;
    m_controller = nullptr;
    const QJsonObject finalUnwatch = nextRequest();
    QCOMPARE(method(finalUnwatch), kUnwatch);
    QCOMPARE(reqSubscriptionId(finalUnwatch), QStringLiteral("sub2"));
}

// A load that completes BEFORE the WebChannel page connects must not be lost:
// EditorController holds it and replays it exactly once from ready().
void TstEditorController::contentBufferedUntilPageReportsReady()
{
    makePair(); // deliberately NO ready(): the page has not connected yet

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    m_controller->open(QStringLiteral("/foo/f.txt"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "loaded before the page"},
                                {"revision", "r1"},
                                {"truncated", false}});
    serveWatchThenNoRecovery();

    // The load completed — state and baseline revision advanced — but the page
    // is not listening, so nothing was pushed to it.
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
    QCOMPARE(contentSpy.count(), 0);

    // Page connects: the held buffer is delivered, intact and exactly once.
    m_controller->ready();
    QTRY_COMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(),
             QStringLiteral("loaded before the page"));
    QCOMPARE(contentSpy.at(0).at(1).toString(), QStringLiteral("r1"));

    // A second ready() is a page RELOAD. It must NOT replay the consumed
    // buffer (that would be a duplicate); it re-fetches instead.
    m_controller->ready();
    const QJsonObject refetch = nextRequest();
    QCOMPARE(method(refetch), kReadFile);
    QCOMPARE(reqPath(refetch), QStringLiteral("/foo/f.txt"));
    QCOMPARE(contentSpy.count(), 1);

    // ...and the re-fetched content lands once, not twice.
    respondResult(reqId(refetch), {{"path", "/foo/f.txt"},
                                   {"encoding", "utf-8"},
                                   {"content", "loaded before the page"},
                                   {"revision", "r1"},
                                   {"truncated", false}});
    QTRY_COMPARE(contentSpy.count(), 2);
    servePermissionStat();
    QTest::qWait(100);
    QCOMPARE(contentSpy.count(), 2);
}

// SPEC 11.3: once a save succeeds the recovery snapshot is obsolete. It MUST be
// cleared, or reopening the file offers a stale "unsaved changes" buffer that
// the user already committed.
void TstEditorController::successfulSaveClearsRecoverySnapshot()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    // Unsaved edit -> snapshot written (create-only, adopts revision "rec1").
    m_controller->reportContent(QStringLiteral("edited"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    const QString recoveryPath = reqPath(snapshot);
    QCOMPARE(recoveryPath, recoveryFilePath());
    QCOMPARE(snapshotContentOf(snapshot), QStringLiteral("edited"));
    QCOMPARE(reqMode(snapshot), 0600);
    respondResult(reqId(snapshot), {{"path", recoveryPath}, {"revision", "rec1"}});

    // Save the buffer for real.
    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    m_controller->save(QStringLiteral("edited"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqPath(write), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(write), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTRY_COMPARE(savedSpy.count(), 1);

    // The snapshot is truncated: a zero-length, revision-GUARDED write (the C1
    // catalog has no delete). Guarding on "rec1" means a snapshot another
    // session replaced would be refused rather than destroyed.
    const QJsonObject clear = nextRequest();
    QCOMPARE(method(clear), kWriteFile);
    QCOMPARE(reqPath(clear), recoveryPath);
    QCOMPARE(reqContent(clear), QString());
    QCOMPARE(reqExpectedRevision(clear), QStringLiteral("rec1"));
    QCOMPARE(reqMode(clear), 0600);
    respondResult(reqId(clear), {{"path", recoveryPath}, {"revision", "rec2"}});

    // Reopen: the emptied snapshot still EXISTS on the server, so stat+read
    // both succeed — but zero length means "nothing to recover".
    QSignalSpy recoverySpy(m_controller, &EditorController::recoveryAvailable);

    m_controller->open(QStringLiteral("/foo/f.txt"));
    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "edited"},
                                {"revision", "r2"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    servePermissionStat();

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), recoveryPath);
    respondResult(reqId(stat), {{"path", recoveryPath}, {"revision", "rec2"}});

    const QJsonObject recRead = nextRequest();
    QCOMPARE(method(recRead), kReadFile);
    QCOMPARE(reqPath(recRead), recoveryPath);
    respondResult(reqId(recRead), {{"path", recoveryPath},
                                   {"encoding", "utf-8"},
                                   {"content", ""},
                                   {"revision", "rec2"},
                                   {"truncated", false}});

    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QTest::qWait(100);
    QCOMPARE(recoverySpy.count(), 0);

    // A second save must not burn another round trip truncating an already
    // empty snapshot: the only request is the file write itself.
    m_controller->save(QStringLiteral("edited again"), QStringLiteral("r2"));
    const QJsonObject write2 = nextRequest();
    QCOMPARE(method(write2), kWriteFile);
    QCOMPARE(reqPath(write2), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(write2), {{"path", "/foo/f.txt"}, {"revision", "r3"}});
    QVERIFY(nextRequest(300).isEmpty());
}

// ---------------------------------------------------------------------------
// A recovery clear is asynchronous too. Two saves completed before the first
// truncate answers must not issue a duplicate truncate with the same revision.
void TstEditorController::aSecondSaveWhileRecoveryClearIsInFlightDoesNotDuplicateIt()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("edited"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    respondResult(reqId(snapshot),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    m_controller->save(QStringLiteral("edited"), QStringLiteral("r1"));
    const QJsonObject firstSave = nextRequest();
    QCOMPARE(reqPath(firstSave), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(firstSave), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTRY_COMPARE(savedSpy.count(), 1);

    const QJsonObject clear = nextRequest();
    QCOMPARE(method(clear), kWriteFile);
    QCOMPARE(reqPath(clear), recoveryFilePath());
    QCOMPARE(reqContent(clear), QString());
    QCOMPARE(reqExpectedRevision(clear), QStringLiteral("rec1"));

    // The second save completes while the first truncate is still in flight.
    m_controller->save(QStringLiteral("edited again"), QStringLiteral("r2"));
    const QJsonObject secondSave = nextRequest();
    QCOMPARE(reqPath(secondSave), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(secondSave),
                  {{"path", "/foo/f.txt"}, {"revision", "r3"}});
    QTRY_COMPARE(savedSpy.count(), 2);
    QVERIFY2(nextRequest(300).isEmpty(),
             "a second save duplicated the recovery truncate");

    respondResult(reqId(clear),
                  {{"path", recoveryFilePath()}, {"revision", "rec2"}});
    QVERIFY2(nextRequest(300).isEmpty(),
             "the recovery clear was issued more than once");
}

// A new snapshot can race a truncate that was already issued by an earlier
// save. If that snapshot's stale guard needs a retry, the second save's clear
// intent must remain armed until the retry has established its revision.
void TstEditorController::recoveryClearIntentSurvivesOverlappingSnapshot()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("first"));
    const QJsonObject firstSnapshot = nextRequest();
    respondResult(reqId(firstSnapshot),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    m_controller->save(QStringLiteral("first"), QStringLiteral("r1"));
    const QJsonObject firstSave = nextRequest();
    respondResult(reqId(firstSave), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTRY_COMPARE(savedSpy.count(), 1);

    const QJsonObject firstClear = nextRequest();
    QCOMPARE(reqExpectedRevision(firstClear), QStringLiteral("rec1"));

    // This report is the second snapshot write, while the first clear is still
    // in flight. The save that follows it must eventually clear this newer slot.
    m_controller->reportContent(QStringLiteral("second"));
    const QJsonObject secondSnapshot = nextRequest();
    QCOMPARE(reqPath(secondSnapshot), recoveryFilePath());
    QCOMPARE(reqExpectedRevision(secondSnapshot), QStringLiteral("rec1"));

    m_controller->save(QStringLiteral("second"), QStringLiteral("r2"));
    const QJsonObject secondSave = nextRequest();
    QCOMPARE(reqPath(secondSave), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(secondSave), {{"path", "/foo/f.txt"}, {"revision", "r3"}});
    QTRY_COMPARE(savedSpy.count(), 2);

    // The first truncate wins before the overlapping snapshot answers.
    respondResult(reqId(firstClear),
                  {{"path", recoveryFilePath()}, {"revision", "rec2"}});
    respondError(reqId(secondSnapshot), ch::rpc::kRevisionMismatch,
                 QStringLiteral("stale snapshot guard"), QJsonObject{});

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), recoveryFilePath());
    respondResult(reqId(stat), {{"path", recoveryFilePath()}, {"revision", "rec2"}});

    const QJsonObject retry = nextRequest();
    QCOMPARE(reqPath(retry), recoveryFilePath());
    QCOMPARE(reqExpectedRevision(retry), QStringLiteral("rec2"));
    respondResult(reqId(retry),
                  {{"path", recoveryFilePath()}, {"revision", "rec3"}});

    // The deferred intent now clears the retried snapshot, rather than leaving
    // the bytes saved by the main file write as a stale recovery offer.
    const QJsonObject secondClear = nextRequest();
    QCOMPARE(reqPath(secondClear), recoveryFilePath());
    QCOMPARE(reqExpectedRevision(secondClear), QStringLiteral("rec3"));
    respondResult(reqId(secondClear),
                  {{"path", recoveryFilePath()}, {"revision", "rec4"}});
    QVERIFY2(nextRequest(300).isEmpty(),
             "the overlapping snapshot resurrected after both saves succeeded");
}

// SPEC 5.6 reconnect.
//
// A file.watch subscription does not live in the wire, it lives in the
// codeharbord PROCESS: remote/src/files.ts keeps FileWatchService's
// subscriptions in a plain per-process Map. A reconnect therefore hands the
// client a DIFFERENT server whose registry is empty and which has never heard
// of the id we are holding. An editor that does not re-establish its watch goes
// silently blind to external changes for the rest of the session.
//
// tst_liveeditorreconnect proves this end to end against a real dropped SSH
// session; these cases pin the same contract on the default suite, where the
// exact request stream can be inspected and adversarial interleavings can be
// produced on demand.
// ---------------------------------------------------------------------------

void TstEditorController::reconnectTransport()
{
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    // 1. EOF on the dying channel while the consumer is STILL attached. This
    //    ordering is not incidental: SessionBootstrap::unwire() closes before
    //    it detaches precisely so pending calls get their synthetic failure and
    //    EditorController learns its subscription id died with the process.
    m_clientSide->disconnectFromServer();
    QTRY_COMPARE(closedSpy.count(), 1);

    // 2. Detach. This must NOT read as "a new server is available" — see
    //    detachingTransportNeverResubscribes().
    m_client->setTransport(nullptr);
    delete m_serverSide;
    m_serverSide = nullptr;
    delete m_clientSide;
    m_clientSide = nullptr;
    delete m_server;
    m_server = nullptr;
    m_serverBuf.clear();

    // 3. Bind the replacement. makePair() ends in setTransport(), which is what
    //    emits transportBound() and drives the re-subscribe.
    makePair();
}

// Teardown is not a reconnect. setTransport(nullptr) deliberately announces
// nothing, because a consumer that "re-established" there would only write into
// a client with no transport bound — every such call fails synchronously and
// the subscription it thinks it holds does not exist.
void TstEditorController::detachingTransportNeverResubscribes()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy boundSpy(m_client, &CodeharbordClient::transportBound);
    // Every call() that cannot transmit announces itself here, so a resubscribe
    // fired against a detached client cannot hide.
    QSignalSpy warningSpy(m_client, &CodeharbordClient::protocolWarning);

    m_client->setTransport(nullptr);
    QTest::qWait(100);

    QCOMPARE(boundSpy.count(), 0);
    QVERIFY2(warningSpy.isEmpty(),
             qPrintable(QStringLiteral("a detach provoked RPC traffic: %1")
                            .arg(warningSpy.value(0).value(0).toString())));
    // The buffer is the user's and survives a teardown untouched.
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
}

// The load-bearing case. After the swap the controller must, on the NEW server:
// subscribe again (its old id is a dead token), and then close the hole the
// outage left — a change made while the client was down produced no watchEvent
// anywhere, so only an explicit stat can discover it.
void TstEditorController::reconnectResubscribesAndReconcilesCleanBuffer()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    reconnectTransport();

    // FIRST request on the new transport is the re-subscribe. Emphatically not
    // file.unwatch: the only peer that could receive a cancellation for "sub1"
    // is the replacement, which never minted it — and whose own ids start over
    // from scratch, so such a request could even cancel a stranger's watcher.
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    QCOMPARE(reqPath(watch), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    // Then the reconciliation stat, on the MAIN file (not the recovery path).
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(stat), {{"path", "/foo/f.txt"}, {"revision", "r2"}});

    // The revision moved and the buffer is clean, so the bytes written during
    // the outage are fetched. Without this the user stares at stale content
    // whose next save is refused for a revision they never saw change.
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "changed while away"},
                                {"revision", "r2"},
                                {"truncated", false}});
    servePermissionStat();

    QTRY_COMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(),
             QStringLiteral("changed while away"));
    QCOMPARE(contentSpy.at(0).at(1).toString(), QStringLiteral("r2"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r2"));
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // And the new subscription is the only one: a watchEvent on the new server
    // reloads exactly once.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub2"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r3"}});
    const QJsonObject reread = nextRequest();
    QCOMPARE(method(reread), kReadFile);
    respondResult(reqId(reread), {{"path", "/foo/f.txt"},
                                  {"encoding", "utf-8"},
                                  {"content", "later"},
                                  {"revision", "r3"},
                                  {"truncated", false}});
    servePermissionStat();
    QTRY_COMPARE(contentSpy.count(), 2);
    QVERIFY2(nextRequest(300).isEmpty(),
             "a second watcher double-delivered the change");
}

// Same round trip with unsaved edits in the buffer: reconciliation may FLAG the
// divergence but must never apply it (SPEC 8.7). Re-subscribing is not licence
// to overwrite the user's work.
void TstEditorController::reconnectLeavesADirtyBufferAloneAndFlagsIt()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    // Unsaved edits, exactly as the page reports them; the SPEC 11.3 snapshot
    // it triggers is served so it cannot be confused with later traffic.
    m_controller->reportContent(QStringLiteral("the user's unsaved work"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QVERIFY(reqPath(snapshot) == recoveryFilePath());
    respondResult(reqId(snapshot),
                  {{"path", reqPath(snapshot)}, {"revision", "rec1"}});
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    reconnectTransport();

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(stat), {{"path", "/foo/f.txt"}, {"revision", "r2"}});

    QTRY_COMPARE(m_controller->fileState(),
                 QStringLiteral("externally_modified"));
    // No reload was issued, no content was delivered, and the guarded revision
    // is still the one the buffer was loaded at.
    QVERIFY2(nextRequest(300).isEmpty(),
             "a dirty buffer was re-read: the user's edits were clobbered");
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
}

// A reconnect landing on top of a file.watch that never got an answer. The dead
// request is failed synchronously by the transport teardown, so the in-flight
// guard must be released and the generation superseded: exactly ONE subscribe
// may go out on the new server — not zero (guard stuck raised, the pane is
// blind), not two (two live watchers double-delivering every change).
void TstEditorController::reconnectOverAnUnansweredWatchSubscribesExactlyOnce()
{
    makePair();

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/f.txt"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "hello"},
                                {"revision", "r1"},
                                {"truncated", false}});

    // Both follow-ups are read off the wire and deliberately LEFT UNANSWERED:
    // the session dies with them outstanding. The recovery probe is chained
    // behind the permission stat, so it is never even issued.
    const QJsonObject deadWatch = nextRequest();
    QCOMPARE(method(deadWatch), kWatch);
    const QJsonObject deadPermissionStat = nextRequest();
    QCOMPARE(method(deadPermissionStat), kStat);
    QCOMPARE(reqPath(deadPermissionStat), QStringLiteral("/foo/f.txt"));

    reconnectTransport();

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    QCOMPARE(reqPath(watch), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), QStringLiteral("/foo/f.txt"));
    // Nothing moved while we were away, so there is nothing to reconcile.
    respondResult(reqId(stat), {{"path", "/foo/f.txt"}, {"revision", "r1"}});

    QVERIFY2(nextRequest(300).isEmpty(),
             "the reconnect issued a second subscribe or an unwanted reload");
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
}

// The reconciliation stat is a SNAPSHOT question — "did the file move while we
// were away?" — asked against the revision the buffer held when it was issued.
// If a save settles before the answer arrives, that answer is stale and the
// save is authoritative. Acting on it anyway would reload the file over a write
// the user just made, one round trip after telling them it succeeded.
void TstEditorController::reconcileLosesToASaveThatSettledFirst()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    reconnectTransport();

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    // Read the stat off the wire but HOLD its answer: the round trip is now
    // open, which is the window this case is about.
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), QStringLiteral("/foo/f.txt"));

    // The page saves while the stat is still in flight and the save wins the
    // race, adopting r3.
    m_controller->save(QStringLiteral("the user's write"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqPath(write), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(write), {{"path", "/foo/f.txt"}, {"revision", "r3"}});
    QTRY_COMPARE(savedSpy.count(), 1);
    QCOMPARE(m_controller->revision(), QStringLiteral("r3"));

    // ...and only NOW the stale snapshot lands, reporting the pre-save world.
    respondResult(reqId(stat), {{"path", "/foo/f.txt"}, {"revision", "r2"}});

    QVERIFY2(nextRequest(300).isEmpty(),
             "the stale reconcile re-read the file over a completed save");
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r3"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
}

// ---------------------------------------------------------------------------
// SPEC 8.2 read-only.
//
// Read-only used to be a setter nobody called: a file the session user could
// not write opened freely editable, let the user type into it, and only failed
// at save time — after the recovery machinery had already snapshotted work that
// could never be written back.
//
// The derivation is deliberately CONSERVATIVE. StatResult (remote/src/files.ts)
// carries `mode` and `kind` but no uid/gid, and the client does not know the
// remote euid, so a set write bit proves nothing about US. The provable case is
// the negative — no write bit set anywhere — and that is what is claimed here.
// ---------------------------------------------------------------------------

void TstEditorController::unwritableFileOpensReadOnlyAndRefusesSave()
{
    makePair();

    QSignalSpy readOnlySpy(m_controller, &EditorController::readOnlyChanged);
    QSignalSpy saveErrorSpy(m_controller, &EditorController::saveError);

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/ro.txt"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/ro.txt"},
                                {"encoding", "utf-8"},
                                {"content", "locked"},
                                {"revision", "r1"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});

    // 0444 — readable by everyone, writable by nobody.
    servePermissionStat(0444);

    QTRY_COMPARE(m_controller->readOnly(), true);
    QCOMPARE(readOnlySpy.count(), 1);
    QCOMPARE(readOnlySpy.at(0).at(0).toBool(), true);

    // A buffer that can never be saved is never offered unsaved changes it
    // could not apply (SPEC 11.3): the recovery probe is not even issued.
    QVERIFY2(nextRequest(300).isEmpty(),
             "a read-only open went looking for a recovery snapshot");

    // ...and a debounced report racing the derivation writes no snapshot.
    m_controller->reportContent(QStringLiteral("typed anyway"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a read-only buffer accumulated recovery state");
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // Ctrl+S is refused HERE, with a reason, instead of going out as a write
    // that fails. The page guards its own key binding the same way, but its
    // conflict/error notices call save() directly and bypass that guard.
    m_controller->save(QStringLiteral("typed anyway"), QStringLiteral("r1"));
    QTRY_COMPARE(saveErrorSpy.count(), 1);
    QVERIFY(saveErrorSpy.at(0).at(0).toString().contains(
        QStringLiteral("read-only"), Qt::CaseInsensitive));
    QVERIFY2(nextRequest(300).isEmpty(), "a read-only save reached the server");
    // Nothing failed and nothing moved: the buffer is still a clean r1.
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
}

// X12: `mode` comes from lstat and is symlink-blind (a link's own bits are
// 0777/whatever the link is), so it cannot see the target's real permissions.
// The server's `writable` flag (C2) is fs.access(W_OK) on the LINK-FOLLOWED
// target and is authoritative: an unwritable-LOOKING mode with writable:true
// opens editable.
void TstEditorController::writableFlagOverridesUnwritableMode()
{
    makePair();

    QSignalSpy readOnlySpy(m_controller, &EditorController::readOnlyChanged);

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/link.txt"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/link.txt"},
                                {"encoding", "utf-8"},
                                {"content", "via symlink"},
                                {"revision", "r1"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});

    // mode 0444 alone would say UNWRITABLE, but writable:true wins.
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), QStringLiteral("/foo/link.txt"));
    respondResult(reqId(stat), {{"path", "/foo/link.txt"},
                                {"kind", "file"},
                                {"mode", 0444},
                                {"writable", true},
                                {"revision", "rperm"}});

    // Not read-only, so the recovery probe goes ahead (SPEC 11.3): a writable
    // file may legitimately be offered its snapshot.
    const QJsonObject rec = nextRequest();
    QCOMPARE(method(rec), kStat);
    QCOMPARE(reqPath(rec), recoveryFilePath());
    respondError(reqId(rec), -32002, QStringLiteral("ENOENT"));

    QTest::qWait(100);
    QCOMPARE(m_controller->readOnly(), false);
    QVERIFY2(readOnlySpy.isEmpty(),
             "the writable flag was ignored in favour of the symlink-blind mode");
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
}

// The mirror image: a writable-LOOKING mode with writable:false (the server's
// fs.access denied) is read-only. The flag forces the honest verdict `mode`
// alone would miss.
void TstEditorController::unwritableFlagForcesReadOnlyOverWritableMode()
{
    makePair();

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/link.txt"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/link.txt"},
                                {"encoding", "utf-8"},
                                {"content", "via symlink"},
                                {"revision", "r1"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});

    // mode 0644 alone would say writable, but writable:false wins.
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    respondResult(reqId(stat), {{"path", "/foo/link.txt"},
                                {"kind", "file"},
                                {"mode", 0644},
                                {"writable", false},
                                {"revision", "rperm"}});

    QTRY_COMPARE(m_controller->readOnly(), true);
    // Read-only => never offered unsaved changes it could not apply (SPEC 11.3):
    // the recovery probe is not even issued.
    QVERIFY2(nextRequest(300).isEmpty(),
             "a read-only (writable:false) open went looking for a recovery snapshot");
}

// A base64 read means the bytes on screen are NOT the file's bytes. save()
// sends utf-8, so writing that buffer back would destroy the file — the buffer
// is read-only even though the file itself is perfectly writable.
void TstEditorController::binaryFileOpensReadOnly()
{
    makePair();

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/logo.png"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/logo.png"},
                                {"encoding", "base64"},
                                {"content", "iVBORw0KGgo="},
                                {"revision", "r1"},
                                {"truncated", false}});
    QTRY_COMPARE(m_controller->readOnly(), true);

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});
    servePermissionStat(0644); // the FILE is writable; the BUFFER still is not

    QTest::qWait(100);
    QCOMPARE(m_controller->readOnly(), true);
}

// Read-only is DERIVED on every load, never latched at open. A chmod bumps
// ctime, so it bumps the revision (revisionFrom in remote/src/files.ts), so it
// arrives as an ordinary external change — and the reload behind it must re-ask
// the server, in BOTH directions.
void TstEditorController::readOnlyIsRederivedOnReloadAndNotLatched()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));
    QCOMPARE(m_controller->readOnly(), false);

    QSignalSpy readOnlySpy(m_controller, &EditorController::readOnlyChanged);

    // chmod 444 on a clean buffer -> watch event -> reload -> re-derive.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r2"}});
    QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "hello"},
                                {"revision", "r2"},
                                {"truncated", false}});
    servePermissionStat(0444);
    QTRY_COMPARE(m_controller->readOnly(), true);

    // chmod 644 back: the pane must become editable again, or a latched verdict
    // leaves the user staring at a file they are allowed to write.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r3"}});
    read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "hello"},
                                {"revision", "r3"},
                                {"truncated", false}});
    servePermissionStat(0644);
    QTRY_COMPARE(m_controller->readOnly(), false);

    QCOMPARE(readOnlySpy.count(), 2);
    QCOMPARE(readOnlySpy.at(0).at(0).toBool(), true);
    QCOMPARE(readOnlySpy.at(1).at(0).toBool(), false);
}

// A chmod during an outage produces no watch event anywhere, and a buffer that
// is not reloaded has no other chance to re-derive. The reconciliation stat the
// reconnect already makes carries the answer, so it is used.
void TstEditorController::reconnectRederivesReadOnlyFromTheReconcileStat()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));
    QCOMPARE(m_controller->readOnly(), false);

    reconnectTransport();

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    // The CONTENT did not move (same revision), so nothing reloads — but the
    // file was chmod'd 444 while we were down and the pane must learn it.
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(stat), {{"path", "/foo/f.txt"},
                                {"kind", "file"},
                                {"mode", 0444},
                                {"revision", "r1"}});

    QTRY_COMPARE(m_controller->readOnly(), true);
    QVERIFY2(nextRequest(300).isEmpty(), "an unchanged file was re-read");
}

// The WebChannel page attaches long after the load (see
// contentBufferedUntilPageReportsReady). A read-only verdict reached while it
// was absent must be replayed to it, and AHEAD of the buffer it applies to —
// otherwise Monaco renders an unwritable file as freely editable.
void TstEditorController::readOnlyReachesAPageThatConnectsLate()
{
    makePair(); // deliberately NO ready(): the page has not connected yet

    QStringList order;
    connect(m_controller, &EditorController::readOnlyChanged, m_controller,
            [&order](bool ro) {
                order << (ro ? QStringLiteral("readOnly=true")
                             : QStringLiteral("readOnly=false"));
            });
    connect(m_controller, &EditorController::contentLoaded, m_controller,
            [&order](const QString&, const QString&) {
                order << QStringLiteral("content");
            });

    m_controller->open(QStringLiteral("/foo/ro.txt"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/ro.txt"},
                                {"encoding", "utf-8"},
                                {"content", "locked"},
                                {"revision", "r1"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});
    servePermissionStat(0444);

    QTRY_COMPARE(m_controller->readOnly(), true);
    QVERIFY2(!order.contains(QStringLiteral("content")),
             "content was pushed at a page that has not connected");

    m_controller->ready();
    QTRY_VERIFY(order.contains(QStringLiteral("content")));
    QCOMPARE(order, QStringList({QStringLiteral("readOnly=true"),
                                 QStringLiteral("readOnly=true"),
                                 QStringLiteral("content")}));
}

// ---------------------------------------------------------------------------
// SPEC 8.2 Disconnected.
//
// Disconnected is the state this controller STARTS in, and it is in the SPEC 8.2
// list, but nothing used to enter it again: a pane went on advertising "clean"
// right through an outage in which it could neither read nor write, and the
// terminal panes (TerminalController::onTransportFinished) already report the
// same drop honestly. Coming BACK has to be honest too — the load or save the
// drop killed has no outcome left to report, so the buffer's dirty flag decides.
// ---------------------------------------------------------------------------
void TstEditorController::transportDropParksTheFileStateUntilRebind()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy stateSpy(m_controller, &EditorController::fileStateChanged);
    reconnectTransport();

    QStringList seen;
    for (const auto& args : stateSpy)
        seen << args.at(0).toString();
    QCOMPARE(seen, QStringList({QStringLiteral("disconnected"),
                                QStringLiteral("clean")}));

    // Drain the reconnect's own round trips so the wire is quiet for part two.
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    respondResult(reqId(stat), {{"path", "/foo/f.txt"}, {"revision", "r1"}});
    QVERIFY(nextRequest(300).isEmpty());

    // Part two: with unsaved edits pending, the rebind must come back to
    // "modified". Landing on "clean" would tell the pane — and the Dev Session
    // row (SPEC 8.2) — that work still living only in the page is safe on disk.
    m_controller->reportContent(QStringLiteral("unsaved work"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    respondResult(reqId(snapshot),
                  {{"path", reqPath(snapshot)}, {"revision", "rec1"}});
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));

    stateSpy.clear();
    reconnectTransport();

    seen.clear();
    for (const auto& args : stateSpy)
        seen << args.at(0).toString();
    QCOMPARE(seen, QStringList({QStringLiteral("disconnected"),
                                QStringLiteral("modified")}));
}

// A save carries bytes that are, by definition, not on the server yet. If the
// write FAILS those bytes exist only inside the page — and the controller may
// never have seen a reportContent for them, because the page debounces its
// report by 500 ms and Ctrl+S beats that. A buffer wrongly believed clean is
// silently auto-reloaded over by the next external change (SPEC 8.7).
// Ctrl+S can beat the first contentLoaded signal, especially while an SSH
// round trip is slow. The previous file's model must not be written with the
// new path or have the load's state replaced by Saving.
void TstEditorController::saveWhileLoadingIsRefused()
{
    makePair();
    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/f.txt"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);

    QSignalSpy errorSpy(m_controller, &EditorController::saveError);
    m_controller->save(QStringLiteral("old page bytes"), QString());
    QTRY_COMPARE(errorSpy.count(), 1);
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(
                 QStringLiteral("still loading"), Qt::CaseInsensitive),
             qPrintable(errorSpy.at(0).at(0).toString()));
    QCOMPARE(m_controller->fileState(), QStringLiteral("loading"));
    QVERIFY2(nextRequest(300).isEmpty(), "a save escaped while the file was loading");

    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "loaded"},
                                {"revision", "r1"},
                                {"truncated", false}});
    serveWatchThenNoRecovery();
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
}

void TstEditorController::failedSaveLeavesTheBufferDirty()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    m_controller->save(QStringLiteral("typed, then saved inside the debounce"),
                       QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    respondError(reqId(write), -32000, QStringLiteral("EIO: write failed"));
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("error"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r5"}});

    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("externally_modified"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a buffer whose save FAILED was treated as clean and re-read");
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
}

// Typing continues while a save is in flight. The bytes that land on the server
// are the ones the save carried, NOT what the buffer holds when the reply
// arrives, so the buffer is still dirty and the recovery snapshot holding those
// extra keystrokes must survive (SPEC 11.3).
void TstEditorController::editsDuringASaveSurviveTheSaveReply()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    m_controller->save(QStringLiteral("first edit"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqPath(write), QStringLiteral("/foo/f.txt"));
    QCOMPARE(reqContent(write), QStringLiteral("first edit"));

    // The user keeps typing; the page's debounced report lands first.
    m_controller->reportContent(QStringLiteral("first edit + more typing"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QCOMPARE(reqPath(snapshot), recoveryFilePath());
    QCOMPARE(snapshotContentOf(snapshot), QStringLiteral("first edit + more typing"));
    respondResult(reqId(snapshot),
                  {{"path", reqPath(snapshot)}, {"revision", "rec1"}});

    // Only now does the write succeed.
    respondResult(reqId(write), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTRY_COMPARE(savedSpy.count(), 1);
    QCOMPARE(m_controller->revision(), QStringLiteral("r2"));

    // Still dirty: the state says so, and the snapshot holding the keystrokes
    // that were NOT saved was not truncated.
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("modified"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "the recovery snapshot holding unsaved keystrokes was truncated");

    // ...so an external change is flagged instead of clobbering them.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r5"}});
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("externally_modified"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "keystrokes made during a save were re-read over");
    QCOMPARE(m_controller->revision(), QStringLiteral("r2"));
}

void TstEditorController::revertingDuringASaveStaysDirty()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    m_controller->save(QStringLiteral("new"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);

    // The page reports a revert before the write answers. The old baseline is
    // not the post-save server content, so this must remain unsaved work.
    m_controller->reportContent(QStringLiteral("orig"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QCOMPARE(reqPath(snapshot), recoveryFilePath());
    respondResult(reqId(snapshot),
                  {{"path", reqPath(snapshot)}, {"revision", "rec1"}});

    respondResult(reqId(write),
                  {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("modified"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r2"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a revert reported during save was treated as clean");
}

// The page debounces reportContent by 500 ms, which outlives an open(). A report
// arriving mid-load describes the buffer of the file the pane is LEAVING, so
// honouring it would file one file's edits under another file's recovery path
// and mark a buffer nobody has edited dirty.
void TstEditorController::reportContentDuringALoadIsIgnored()
{
    makePair();

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/f.txt"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);

    m_controller->reportContent(QStringLiteral("the previous file's buffer"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a report that arrived mid-load wrote a recovery snapshot");

    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "loaded"},
                                {"revision", "r1"},
                                {"truncated", false}});
    serveWatchThenNoRecovery();
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // Genuinely clean, so an external change auto-reloads. Had the mid-load
    // report been taken as an edit this would have been flagged instead.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r5"}});
    const QJsonObject reread = nextRequest();
    QCOMPARE(method(reread), kReadFile);
    QCOMPARE(reqPath(reread), QStringLiteral("/foo/f.txt"));
}

void TstEditorController::reportIdenticalContentDoesNotStayDirty()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("hello"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "content equal to the loaded bytes created a recovery snapshot");
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
}

// Two loads overlap (open A, then open B before A answered). The replies are
// separate requests and can arrive in either order, so the older one must be
// DROPPED: applying it would push the wrong file's bytes at the page and, worse,
// adopt the wrong revision as this buffer's save guard.
void TstEditorController::staleLoadReplyNeverWins()
{
    makePair();

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/a.txt"));
    const QJsonObject readA = nextRequest();
    QCOMPARE(method(readA), kReadFile);
    QCOMPARE(reqPath(readA), QStringLiteral("/foo/a.txt")); // left unanswered

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    m_controller->open(QStringLiteral("/foo/b.txt"));
    const QJsonObject readB = nextRequest();
    QCOMPARE(method(readB), kReadFile);
    QCOMPARE(reqPath(readB), QStringLiteral("/foo/b.txt"));
    respondResult(reqId(readB), {{"path", "/foo/b.txt"},
                                 {"encoding", "utf-8"},
                                 {"content", "B"},
                                 {"revision", "rB"},
                                 {"truncated", false}});
    serveWatchThenNoRecovery();
    QTRY_COMPARE(contentSpy.count(), 1);
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // A's read finally answers, out of order.
    respondResult(reqId(readA), {{"path", "/foo/a.txt"},
                                 {"encoding", "utf-8"},
                                 {"content", "A"},
                                 {"revision", "rA"},
                                 {"truncated", false}});
    QTest::qWait(100);

    QCOMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(), QStringLiteral("B"));
    QCOMPARE(m_controller->revision(), QStringLiteral("rB"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "the superseded load re-subscribed a watcher or re-probed recovery");
}

// A write that answers after the pane switched files describes a file this
// controller no longer holds. Adopting its revision would guard the NEW file's
// next save with the OLD file's token — every save refused as a conflict — and
// the page would be told a save it never asked for succeeded.
void TstEditorController::saveReplyAfterAFileSwitchIsDropped()
{
    makePair();
    openClean(QStringLiteral("/foo/a.txt"), QStringLiteral("A"),
              QStringLiteral("r1"));

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    m_controller->save(QStringLiteral("A edited"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqPath(write), QStringLiteral("/foo/a.txt")); // left unanswered

    m_controller->open(QStringLiteral("/foo/b.txt"));
    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/b.txt"));
    respondResult(reqId(read), {{"path", "/foo/b.txt"},
                                {"encoding", "utf-8"},
                                {"content", "B"},
                                {"revision", "rB"},
                                {"truncated", false}});
    serveWatchThenNoRecovery();
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    respondResult(reqId(write), {{"path", "/foo/a.txt"}, {"revision", "r2"}});
    QTest::qWait(100);

    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("rB"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a save for the previous file wrote against the new file");
}

// A ReadFileResult with truncated=true is a PREFIX of the file (remote/src/
// files.ts only fills a requested byte window). The bytes on screen are not the
// file's bytes, so writing them back would delete everything past the prefix —
// the same reason a base64 (binary) read is not writable back (SPEC 8.2).
void TstEditorController::truncatedReadIsNotWritableBack()
{
    makePair();

    QSignalSpy saveErrorSpy(m_controller, &EditorController::saveError);

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/huge.log"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/huge.log"},
                                {"encoding", "utf-8"},
                                {"content", "the first window of a huge file"},
                                {"revision", "r1"},
                                {"truncated", true}});
    QTRY_COMPARE(m_controller->readOnly(), true);

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});
    servePermissionStat(0644); // the FILE is writable; the BUFFER still is not

    QTest::qWait(100);
    QCOMPARE(m_controller->readOnly(), true);

    m_controller->save(QStringLiteral("the first window of a huge file"),
                       QStringLiteral("r1"));
    QTRY_COMPARE(saveErrorSpy.count(), 1);
    QVERIFY2(nextRequest(300).isEmpty(),
             "a truncated buffer was written back over the whole file");
}

// The exact revision-mismatch payload the real server sends — RevisionMismatchError
// in remote/src/files.ts attaches {path, expected, currentRevision} — must
// short-circuit the fallback file.stat, and the Conflict it produces must then
// survive the watch event for that same external write.
void TstEditorController::conflictKeepsItsStateWhenTheWatchEventLands()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);

    m_controller->save(QStringLiteral("stale write"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    respondError(reqId(write), ch::rpc::kRevisionMismatch,
                 QStringLiteral("Revision mismatch for /foo/f.txt"),
                 QJsonObject{{"path", "/foo/f.txt"},
                             {"expected", "r1"},
                             {"currentRevision", "r9"}});

    QTRY_COMPARE(conflictSpy.count(), 1);
    QCOMPARE(conflictSpy.at(0).at(0).toString(), QStringLiteral("r9"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("conflict"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "the server had already named the current revision");
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));

    // The watch event for that same external write lands moments later. It says
    // strictly less than the conflict already on screen (the buffer diverges) and
    // must not overwrite it: the page's reload/overwrite affordance belongs to
    // Conflict, and a pane that slid back to "externally modified" would report
    // a refused save as a routine external change.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r9"}});
    QTest::qWait(100);
    QCOMPARE(m_controller->fileState(), QStringLiteral("conflict"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a conflicted buffer was re-read over the user's refused edits");
}

// fileStateChanged is one-shot like every other bridge signal, so a page that
// connects after the load missed all of them and renders an empty status bar
// until the next transition. ready() replays the current state.
void TstEditorController::lateReadyReplaysTheFileState()
{
    makePair(); // deliberately NO ready(): the page has not connected yet

    m_controller->open(QStringLiteral("/foo/f.txt"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "hello"},
                                {"revision", "r1"},
                                {"truncated", false}});
    serveWatchThenNoRecovery();
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    QSignalSpy stateSpy(m_controller, &EditorController::fileStateChanged);
    m_controller->ready();

    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).toString(), QStringLiteral("clean"));
}

// SPEC 8.3: a file.readFile with no `length` makes the server read the WHOLE
// file into memory (remote/src/files.ts), which is then held again here and once
// more as a JS string in the Monaco page — and, because files.ts derives
// `truncated` from the requested length, such a read cannot even report that the
// file was too big. Every read this controller issues must therefore be a
// bounded window, on the first load and on every reload.
void TstEditorController::readsAskForABoundedWindow()
{
    makePair();

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/f.txt"));

    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqOffset(read), qint64(0));
    QCOMPARE(reqLength(read), qint64(EditorController::kMaxEditableReadBytes));
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "hello"},
                                {"revision", "r1"},
                                {"truncated", false}});
    serveWatchThenNoRecovery();
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // The reload path is a second, independent call site and drifts silently if
    // it is not pinned: an unbounded reload would re-read the whole file every
    // time an external change lands.
    m_controller->requestReload();
    const QJsonObject reread = nextRequest();
    QCOMPARE(method(reread), kReadFile);
    QCOMPARE(reqOffset(reread), qint64(0));
    QCOMPARE(reqLength(reread), qint64(EditorController::kMaxEditableReadBytes));
    respondResult(reqId(reread), {{"path", "/foo/f.txt"},
                                  {"encoding", "utf-8"},
                                  {"content", "hello"},
                                  {"revision", "r1"},
                                  {"truncated", false}});
    servePermissionStat();
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
}

// A read that came back truncated=true is a PREFIX of a file bigger than the
// ceiling. "clean" asserts the buffer IS what the server has, which is false, so
// the pane must not report it: the file settles in FileState::ReadOnly, the one
// SPEC 8.2 state from which no save can be issued. The prefix is still shown —
// that is what makes a huge log readable — but it can never be written back over
// the tail nobody read.
void TstEditorController::aTruncatedFileIsNotOpenedForEditing()
{
    makePair();

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/huge.log"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/huge.log"},
                                {"encoding", "utf-8"},
                                {"content", "the first window of a huge file"},
                                {"revision", "r1"},
                                {"truncated", true}});

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});
    servePermissionStat(0644); // the FILE is writable; the BUFFER still is not

    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("read_only"));
    QCOMPARE(m_controller->readOnly(), true);
    // No recovery probe: a buffer that can never be saved must not be offered
    // unsaved changes it could never apply (SPEC 11.3).
    QVERIFY2(nextRequest(300).isEmpty(),
             "a truncated open went looking for a recovery snapshot");

    // A transport drop must not launder the state back to "clean" on rebind:
    // ReadOnly describes the bytes this pane holds, and a reconnect does not
    // make a prefix whole.
    reconnectTransport();
    const QJsonObject rewatch = nextRequest();
    QCOMPARE(method(rewatch), kWatch);
    respondResult(reqId(rewatch), {{"subscriptionId", "sub2"}});
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    respondResult(reqId(stat), {{"path", "/foo/huge.log"}, {"revision", "r1"}});
    QTest::qWait(100);
    QCOMPARE(m_controller->fileState(), QStringLiteral("read_only"));
    QCOMPARE(m_controller->readOnly(), true);
}

// The file was DELETED while the session was down. No watchEvent exists for it
// (the codeharbord that held the subscription died first), so the reconnect's
// file.stat is the only chance to notice — and it comes back as an error. The
// pane must stop claiming the pre-outage state, and the revision baseline must
// SURVIVE, because it is what turns the user's next save into a guarded write
// the server refuses instead of an unguarded create that silently resurrects the
// file.
void TstEditorController::deletedWhileDisconnectedStopsClaimingClean()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    reconnectTransport();

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    // lstat fails: the remote service reports it as a plain internal error
    // carrying the Node message (dispatch() in remote/src/codeharbord.ts).
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), QStringLiteral("/foo/f.txt"));
    respondError(reqId(stat), -32603,
                 QStringLiteral("ENOENT: no such file or directory, lstat "
                                "'/foo/f.txt'"));

    // A clean buffer has nothing to lose, so the controller VERIFIES rather than
    // guesses: it re-reads, and that read fails too.
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));
    respondError(reqId(read), -32603,
                 QStringLiteral("ENOENT: no such file or directory, open "
                                "'/foo/f.txt'"));

    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("error"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));

    // And the baseline still guards: the page saves with the revision it was
    // handed, so the write goes out GUARDED (not create-only) and the server
    // refuses it. Recreating a deleted file stays an explicit user choice.
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    m_controller->save(QStringLiteral("hello, typed"), m_controller->revision());
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqExpectedRevision(write), QStringLiteral("r1"));
    respondError(reqId(write), ch::rpc::kRevisionMismatch,
                 QStringLiteral("File no longer exists: /foo/f.txt"));

    // No currentRevision in the payload -> the controller stats, and that fails
    // too, so the conflict is surfaced with an empty revision.
    const QJsonObject conflictStat = nextRequest();
    QCOMPARE(method(conflictStat), kStat);
    respondError(reqId(conflictStat), -32603, QStringLiteral("ENOENT"));

    QTRY_COMPARE(conflictSpy.count(), 1);
    QCOMPARE(conflictSpy.at(0).at(0).toString(), QString());
    QCOMPARE(m_controller->fileState(), QStringLiteral("conflict"));
}

// Same vanished file, but the buffer holds unsaved work. The reconciliation may
// FLAG the divergence and must never re-read over it (SPEC 8.7) — a failed read
// would be no excuse to throw the user's only copy of those edits away.
void TstEditorController::deletedWhileDisconnectedWithUnsavedWorkIsOnlyFlagged()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("the user's unsaved work"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QVERIFY(reqPath(snapshot) == recoveryFilePath());
    respondResult(reqId(snapshot),
                  {{"path", reqPath(snapshot)}, {"revision", "rec1"}});
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    reconnectTransport();

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    respondError(reqId(stat), -32603,
                 QStringLiteral("ENOENT: no such file or directory, lstat "
                                "'/foo/f.txt'"));

    QTRY_COMPARE(m_controller->fileState(),
                 QStringLiteral("externally_modified"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a dirty buffer was re-read over the user's unsaved edits");
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
}

// A reload the SYSTEM started (here: a watch event on a clean buffer) takes a
// round trip, and the page debounces its reportContent by 500 ms — so a
// keystroke typed just after the change was noticed lands INSIDE that round
// trip. Installing the fetched bytes then would replace a buffer holding work
// that exists nowhere on the server: silent data loss, one keystroke wide. The
// reply must recognise that the buffer moved and flag the divergence instead,
// exactly as if the change had been noticed a moment later against an
// already-dirty buffer (SPEC 8.7).
void TstEditorController::editsTypedDuringASystemReloadAreNotClobbered()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    // Clean buffer + external change -> the controller starts re-reading.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r5"}});
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));

    // The user types while that read is in flight; the debounced report lands
    // first and is snapshotted (SPEC 11.3).
    m_controller->reportContent(QStringLiteral("work typed during the reload"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QCOMPARE(reqPath(snapshot), recoveryFilePath());
    QCOMPARE(snapshotContentOf(snapshot),
             QStringLiteral("work typed during the reload"));
    respondResult(reqId(snapshot),
                  {{"path", reqPath(snapshot)}, {"revision", "rec1"}});

    // ...and only now does the reload answer.
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "the server's bytes"},
                                {"revision", "r5"},
                                {"truncated", false}});
    QTest::qWait(100);

    // Nothing was pushed at the page, the baseline was NOT re-adopted (a save
    // must still be refused as a conflict), and the snapshot holding the
    // keystrokes was not truncated.
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("externally_modified"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "the abandoned reload still re-derived permissions or cleared the "
             "recovery snapshot holding the user's keystrokes");

    // The buffer is still dirty, so the NEXT external change is flagged too
    // rather than auto-reloaded over those keystrokes.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r6"}});
    QTest::qWait(100);
    QCOMPARE(m_controller->fileState(), QStringLiteral("externally_modified"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a dirty buffer was re-read after the abandoned reload");
    QCOMPARE(contentSpy.count(), 0);
}

// The twin of the case above, for the OTHER way a system reload can land on
// bytes that exist nowhere but in the page. Ctrl/Cmd+S beats the page's 500 ms
// debounce, so a save can carry keystrokes the controller was never told about:
// m_editSerial has not moved, and the reload's own "did the buffer change?"
// guard therefore sees nothing. Installing the fetched bytes then replaces the
// buffer the write is still carrying, re-baselines the revision that write is
// guarded by (so its reply is refused as a conflict) and drops the only copy of
// the edits. The save is the authority over this file until its reply lands.
void TstEditorController::aSaveInFlightIsNotClobberedByASystemReload()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);

    // Clean buffer + external change -> the controller starts re-reading.
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r5"}});
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));

    // The user types and hits Ctrl+S inside the debounce, so NO reportContent
    // ever reached the controller for these bytes.
    m_controller->save(QStringLiteral("typed, then saved inside the debounce"),
                       QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqPath(write), QStringLiteral("/foo/f.txt"));
    QCOMPARE(reqExpectedRevision(write), QStringLiteral("r1"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("saving"));

    // ...and only now does the abandoned reload answer.
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "the server's bytes"},
                                {"revision", "r5"},
                                {"truncated", false}});
    QTest::qWait(100);

    // Nothing was pushed at the page, the save's guard revision still stands,
    // and the pane is still reporting the write it is waiting on.
    QCOMPARE(contentSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("saving"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "the abandoned reload still re-derived permissions or cleared the "
             "recovery snapshot holding the bytes the save is carrying");

    // The write's own reply decides the outcome, and it is the honest one: the
    // file really did move, so the guarded write is refused.
    respondError(reqId(write), ch::rpc::kRevisionMismatch,
                 QStringLiteral("stale revision"),
                 QJsonObject{{"currentRevision", "r5"}});
    QTRY_COMPARE(conflictSpy.count(), 1);
    QCOMPARE(conflictSpy.at(0).at(0).toString(), QStringLiteral("r5"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("conflict"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
    QCOMPARE(contentSpy.count(), 0);
}

// The third way a load and a save can collide, and the one the guards above do
// NOT cover. The save callback's own check is `m_path != path`, and both an
// open() of the SAME path and every reload() leave m_path equal — so a load that
// really did take the buffer over is invisible to it.
//
// A reload the USER asked for is allowed to replace the buffer even with a save
// in flight (that is what "discard my edits" means), so it commits: it adopts the
// server's revision, marks the buffer clean and drops the recovery snapshot. The
// write's reply then arrives describing bytes the pane has thrown away. Acting on
// it would re-baseline the save guard to a revision that matches nothing on
// screen, mark a buffer clean that nobody checked, clear the recovery snapshot of
// the newly loaded buffer, and tell the page a save succeeded that it never asked
// for. The load owns this pane from its commit onwards.
void TstEditorController::aSaveReplyIsDroppedAfterAnExplicitReloadTookOverTheBuffer()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);

    m_controller->save(QStringLiteral("the user's edits"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqExpectedRevision(write), QStringLiteral("r1"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("saving"));

    // The user presses Reload while that write is still on the wire. Same path,
    // so nothing in the save's callback can tell this happened.
    m_controller->requestReload();
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "the server's bytes"},
                                {"revision", "r9"},
                                {"truncated", false}});

    // The reload committed: the pane now holds the server's bytes at r9.
    QTRY_COMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(), QStringLiteral("the server's bytes"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r9"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    // reload() re-derives permissions after committing (a chmod reaches us as an
    // ordinary external change), so answer that stat and leave the queue empty.
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    respondResult(reqId(stat), {{"path", "/foo/f.txt"},
                                {"revision", "r9"},
                                {"writable", true}});
    QTest::qWait(50);

    // Only now does the write answer, and it answers SUCCESSFULLY — the worst
    // case, because every branch of the success path rewrites pane state.
    respondResult(reqId(write), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTest::qWait(100);

    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(conflictSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r9"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(contentSpy.count(), 1);
    QVERIFY2(nextRequest(300).isEmpty(),
             "the superseded save touched the reloaded buffer's bookkeeping "
             "(a recovery-snapshot truncation or a permissions re-derive)");
}

// The same hole reached the other way: re-opening the file the pane is already
// showing. open() leaves m_path identical, so the save callback's path check
// cannot see it, and before the load-generation guard the write's reply ran in
// full against the freshly loaded buffer. Note that open(), unlike reload(),
// does NOT truncate the recovery snapshot — it re-probes it, which is how
// unsaved work is offered back — so a superseded save clearing it here would
// destroy work the pane may be in the middle of offering to the user.
//
// This case answers the write with a CONFLICT rather than a success, because the
// conflict path is two asynchronous hops (the write reply, then a stat to find
// the current revision) and therefore gives a load two windows to land in, not
// one. Reporting it would put the pane into `conflict` over a revision mismatch
// that concerns bytes it no longer holds, with a page-level Reload/Overwrite
// prompt the user cannot answer meaningfully.
void TstEditorController::aSaveConflictIsDroppedAfterASamePathReopenTookOverTheBuffer()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    QSignalSpy errorSpy(m_controller, &EditorController::saveError);
    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    m_controller->save(QStringLiteral("the user's edits"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(m_controller->fileState(), QStringLiteral("saving"));

    // Re-open the very same path while the write is in flight.
    m_controller->open(QStringLiteral("/foo/f.txt"));
    // Re-opening drops the previous subscription first (SPEC 8.7).
    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    respondResult(reqId(unwatch), {});
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "the server's bytes"},
                                {"revision", "r9"},
                                {"truncated", false}});
    QTRY_COMPARE(contentSpy.count(), 1);
    serveWatchThenNoRecovery();
    QCOMPARE(m_controller->revision(), QStringLiteral("r9"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // The write is refused as stale, WITHOUT a revision attached, so the
    // controller would fall through to its stat hop.
    respondError(reqId(write), ch::rpc::kRevisionMismatch,
                 QStringLiteral("stale revision"), QJsonObject{});
    QTest::qWait(100);

    // Dropped at the first hop: no conflict, no error, no stat on the wire, and
    // the reopened buffer's state is exactly as the load left it.
    QCOMPARE(conflictSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("r9"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "the superseded save chased a conflict revision for a buffer the "
             "pane had already replaced");
}

// A successful save retires the crash-recovery snapshot, because the file on the
// server now IS the buffer. But the controller only learns a snapshot exists when
// that snapshot's own write answers, and clearRecovery() used to read "no snapshot
// known" as "nothing to do" — so a save whose reply beat the snapshot's reply left
// the snapshot behind entirely.
//
// That matters when the user keeps typing between the page's debounced report and
// the save, which is ordinary: the surviving snapshot then holds OLDER text than
// the file, so the next open offers to restore work the save already superseded,
// and accepting it walks the buffer backwards. The offer is a real dialog now, so
// this is a wrong answer the user can act on.
void TstEditorController::aSuccessfulSaveStillClearsASnapshotThatWasStillInFlight()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    // The page reports its buffer; the snapshot write goes out and stays unanswered.
    m_controller->reportContent(QStringLiteral("older edits"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QCOMPARE(reqPath(snapshot), recoveryFilePath());

    // The user types more and saves before that snapshot has answered.
    m_controller->save(QStringLiteral("newer edits"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqPath(write), QStringLiteral("/foo/f.txt"));

    // The save succeeds first. It cannot truncate the snapshot yet — it does not
    // know the revision to guard the truncate against — so it must defer, not skip.
    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    respondResult(reqId(write), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTRY_COMPARE(savedSpy.count(), 1);
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a truncate was guarded against a snapshot revision nobody knew yet");

    // Now the snapshot answers, and the deferred truncate runs, guarded by the
    // revision that reply just supplied.
    respondResult(reqId(snapshot),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});
    const QJsonObject truncate = nextRequest();
    QVERIFY2(!truncate.isEmpty(),
             "the snapshot survived a successful save, so the next open would "
             "offer to restore text the save had already superseded");
    QCOMPARE(method(truncate), kWriteFile);
    QCOMPARE(reqPath(truncate), recoveryFilePath());
    QCOMPARE(reqContent(truncate), QString());
    QCOMPARE(reqExpectedRevision(truncate), QStringLiteral("rec1"));
    respondResult(reqId(truncate),
                  {{"path", recoveryFilePath()}, {"revision", "rec2"}});

    // Exactly once: nothing re-arms the deferred clear.
    QVERIFY2(nextRequest(300).isEmpty(), "the deferred clear fired more than once");
}

// The deferral has to survive the snapshot write's stale-guard RETRY, which adds two
// round trips: a stat for the current revision, then the re-write. The discriminating
// order is a save that succeeds DURING the stat hop — if the chain stops being
// counted for that hop, the truncate the save asks for is decided on what is known
// right then (no snapshot revision at all, because the first write was refused), so
// the request is dropped, and the retry write that follows then leaves a snapshot of
// pre-save text sitting behind a successful save.
void TstEditorController::aDeferredTruncateSurvivesTheSnapshotRetryChain()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("older edits"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(reqPath(snapshot), recoveryFilePath());

    // The snapshot write is refused first: a snapshot from a previous session already
    // occupies the slot, so the controller stats it and will retry once.
    respondError(reqId(snapshot), ch::rpc::kRevisionMismatch,
                 QStringLiteral("stale create guard"), QJsonObject{});
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), recoveryFilePath());

    // The user saves, and that save SUCCEEDS while the retry chain is parked on its
    // stat. This is the window the count has to keep covered.
    m_controller->save(QStringLiteral("newer edits"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(reqPath(write), QStringLiteral("/foo/f.txt"));
    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    respondResult(reqId(write), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTRY_COMPARE(savedSpy.count(), 1);

    // Nothing may be truncated yet: no snapshot revision exists to guard against.
    QVERIFY2(nextRequest(300).isEmpty(),
             "a truncate ran during the retry's stat hop, before the retry write had "
             "established what it would be guarding against");

    respondResult(reqId(stat), {{"path", recoveryFilePath()}, {"revision", "recX"}});
    const QJsonObject retry = nextRequest();
    QCOMPARE(reqPath(retry), recoveryFilePath());
    QCOMPARE(reqExpectedRevision(retry), QStringLiteral("recX"));
    respondResult(reqId(retry),
                  {{"path", recoveryFilePath()}, {"revision", "recY"}});

    // Only now, with the snapshot really on the server, does the deferred truncate
    // run — guarded by the revision the retry produced.
    const QJsonObject truncate = nextRequest();
    QVERIFY2(!truncate.isEmpty(),
             "the deferred truncate was lost across the retry chain, leaving a "
             "snapshot of pre-save text behind a successful save");
    QCOMPARE(reqPath(truncate), recoveryFilePath());
    QCOMPARE(reqContent(truncate), QString());
    QCOMPARE(reqExpectedRevision(truncate), QStringLiteral("recY"));
}

// The in-flight count and the deferred-truncate intent are shared pane state, so a
// snapshot write belonging to a file the pane has since left must not touch either.
// Without a generation stamp the stale reply decrements the NEW file's count, which
// fires that file's deferred truncate early — guarded by the wrong revision, and
// before the write it was meant to wait for has answered.
void TstEditorController::aStaleSnapshotReplyDoesNotDisturbTheNextFilesBookkeeping()
{
    makePair();
    openClean(QStringLiteral("/foo/a.txt"), QStringLiteral("A"),
              QStringLiteral("ra1"));

    // A snapshot for the FIRST file goes out and is left unanswered.
    m_controller->reportContent(QStringLiteral("edits in a.txt"));
    const QJsonObject staleSnapshot = nextRequest();
    QCOMPARE(reqPath(staleSnapshot), recoveryFilePath());

    // Switch to another file.
    m_controller->open(QStringLiteral("/foo/b.txt"));
    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    respondResult(reqId(unwatch), {});
    const QJsonObject read = nextRequest();
    QCOMPARE(reqPath(read), QStringLiteral("/foo/b.txt"));
    respondResult(reqId(read), {{"path", "/foo/b.txt"},
                                {"encoding", "utf-8"},
                                {"content", "B"},
                                {"revision", "rb1"},
                                {"truncated", false}});
    serveWatchThenNoRecovery();

    // The new file gets its own snapshot, and a save that defers a truncate.
    m_controller->reportContent(QStringLiteral("edits in b.txt"));
    const QJsonObject liveSnapshot = nextRequest();
    QCOMPARE(reqPath(liveSnapshot), recoveryFilePath());
    m_controller->save(QStringLiteral("saved b"), QStringLiteral("rb1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(reqPath(write), QStringLiteral("/foo/b.txt"));
    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    respondResult(reqId(write), {{"path", "/foo/b.txt"}, {"revision", "rb2"}});
    QTRY_COMPARE(savedSpy.count(), 1);

    // Now the FIRST file's snapshot finally answers. It must change nothing.
    respondResult(reqId(staleSnapshot),
                  {{"path", recoveryFilePath()}, {"revision", "stale"}});
    QVERIFY2(nextRequest(300).isEmpty(),
             "a snapshot reply from the previous file released the current file's "
             "deferred truncate early");

    // The current file's own snapshot answers, and only then does the truncate run,
    // guarded by that snapshot's revision rather than the stale one.
    respondResult(reqId(liveSnapshot),
                  {{"path", recoveryFilePath()}, {"revision", "recB"}});
    const QJsonObject truncate = nextRequest();
    QVERIFY2(!truncate.isEmpty(), "the deferred truncate never ran");
    QCOMPARE(reqPath(truncate), recoveryFilePath());
    QCOMPARE(reqContent(truncate), QString());
    QCOMPARE(reqExpectedRevision(truncate), QStringLiteral("recB"));
}

// The half of that rule which must NOT fire. If the user types again AFTER the save
// succeeded, those bytes are unsaved work the save did not write, and the snapshot
// holding them must survive — so a fresh report cancels a truncate the save had
// deferred rather than being deleted by it.
void TstEditorController::aReportAfterASaveCancelsTheTruncateThatSaveDeferred()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("older edits"));
    const QJsonObject firstSnapshot = nextRequest();
    QCOMPARE(reqPath(firstSnapshot), recoveryFilePath());

    m_controller->save(QStringLiteral("saved bytes"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(reqPath(write), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(write), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTest::qWait(50);

    // The user types again while the first snapshot is STILL unanswered. This is
    // newer than anything the save wrote.
    m_controller->reportContent(QStringLiteral("typed after the save"));
    const QJsonObject secondSnapshot = nextRequest();
    QCOMPARE(reqPath(secondSnapshot), recoveryFilePath());

    // Both snapshot writes now answer, oldest first.
    respondResult(reqId(firstSnapshot),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});
    respondResult(reqId(secondSnapshot),
                  {{"path", recoveryFilePath()}, {"revision", "rec2"}});
    QTest::qWait(100);

    QVERIFY2(nextRequest(300).isEmpty(),
             "the save's deferred truncate deleted a snapshot of work typed AFTER "
             "that save, which exists nowhere else");
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));
}

// ---------------------------------------------------------------------------
// Saves are SERIALISED, not raced (EditorController::save / issueSave).
//
// The save key beats an SSH round trip comfortably, so pressing it twice is an
// ordinary thing to do. Racing the two writes made the second carry the SAME
// expectedRevision as the first, which the first had just retired — so the
// server refused it and the pane announced "file changed on disk" for a change
// that was this client's own save. These four cases pin the four outcomes.

// Twice on an UNCHANGED buffer: the write already on the wire IS this save, so
// no second write is sent and the one reply answers both.
void TstEditorController::aSecondSaveOfTheSameBufferIsNotASecondWriteAndNotAConflict()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    QSignalSpy errorSpy(m_controller, &EditorController::saveError);

    m_controller->save(QStringLiteral("world"), QStringLiteral("r1"));
    const QJsonObject write = nextRequest();
    QCOMPARE(method(write), kWriteFile);
    QCOMPARE(reqExpectedRevision(write), QStringLiteral("r1"));

    // Second press, same buffer, before the first answers.
    m_controller->save(QStringLiteral("world"), QStringLiteral("r1"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a second save of an unchanged buffer put a second write on the "
             "wire, guarded by the revision the first one is about to retire");

    respondResult(reqId(write), {{"path", "/foo/f.txt"}, {"revision", "r2"}});

    QTRY_COMPARE(savedSpy.count(), 1);
    QCOMPARE(savedSpy.at(0).at(0).toString(), QStringLiteral("r2"));
    QCOMPARE(conflictSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r2"));
    QVERIFY2(nextRequest(300).isEmpty(), "the coalesced save was sent after all");
}

// Twice with DIFFERENT bytes: the second save is real work, so it is issued —
// but only once the first write has answered, and guarded by the revision that
// write produced rather than the stale one the page could only have known.
void TstEditorController::aSecondSaveOfChangedBytesIsRewrittenAgainstTheRevisionItNowNeeds()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);

    m_controller->save(QStringLiteral("first"), QStringLiteral("r1"));
    const QJsonObject write1 = nextRequest();
    QCOMPARE(method(write1), kWriteFile);
    QCOMPARE(reqContent(write1), QStringLiteral("first"));

    // The user types and saves again inside the round trip. The page can only
    // offer the revision it still believes in, which is the one write1 is about
    // to replace.
    m_controller->save(QStringLiteral("second"), QStringLiteral("r1"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "the queued save raced the write it should have waited for");

    respondResult(reqId(write1), {{"path", "/foo/f.txt"}, {"revision", "r2"}});

    const QJsonObject write2 = nextRequest();
    QCOMPARE(method(write2), kWriteFile);
    QCOMPARE(reqPath(write2), QStringLiteral("/foo/f.txt"));
    QCOMPARE(reqContent(write2), QStringLiteral("second"));
    QCOMPARE(reqExpectedRevision(write2), QStringLiteral("r2"));

    // The superseded write reported nothing: the file is not the buffer yet, so
    // the page must not be told it is saved and the pane must stay in Saving.
    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(conflictSpy.count(), 0);
    QCOMPARE(m_controller->fileState(), QStringLiteral("saving"));

    respondResult(reqId(write2), {{"path", "/foo/f.txt"}, {"revision", "r3"}});

    QTRY_COMPARE(savedSpy.count(), 1);
    QCOMPARE(savedSpy.at(0).at(0).toString(), QStringLiteral("r3"));
    QCOMPARE(conflictSpy.count(), 0);
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r3"));
}

// A genuine conflict — someone else moved the file — still reports exactly one
// notice. The queued bytes are dropped rather than fired at a server that just
// refused this pane's write; nothing is lost, because the buffer stays dirty and
// the page's Overwrite/Retry re-send it as it stands at the click.
void TstEditorController::aQueuedSaveIsDroppedWhenTheWriteItWaitedOnIsRefused()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    QSignalSpy savedSpy(m_controller, &EditorController::saved);

    m_controller->save(QStringLiteral("first"), QStringLiteral("r1"));
    const QJsonObject write1 = nextRequest();
    QCOMPARE(method(write1), kWriteFile);

    m_controller->save(QStringLiteral("second"), QStringLiteral("r1"));

    respondError(reqId(write1), ch::rpc::kRevisionMismatch,
                 QStringLiteral("stale revision"),
                 QJsonObject{{"currentRevision", "r9"}});

    QTRY_COMPARE(conflictSpy.count(), 1);
    QCOMPARE(conflictSpy.at(0).at(0).toString(), QStringLiteral("r9"));
    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(m_controller->fileState(), QStringLiteral("conflict"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "the queued save was written on top of a refused revision, or "
             "chased a second conflict of its own");
}

// The chain is per-FILE. A write still on the wire for the file the pane has
// LEFT must neither hold the new file's saves back nor, when it finally answers,
// release the new file's chain out from under it.
void TstEditorController::aSaveOnANewFileDoesNotJoinThePreviousFilesSaveChain()
{
    makePair();
    openClean(QStringLiteral("/foo/a.txt"), QStringLiteral("aaa"),
              QStringLiteral("ra1"));

    m_controller->save(QStringLiteral("a-edited"), QStringLiteral("ra1"));
    const QJsonObject writeA = nextRequest();
    QCOMPARE(method(writeA), kWriteFile);
    QCOMPARE(reqPath(writeA), QStringLiteral("/foo/a.txt"));
    // Deliberately left unanswered: /foo/a.txt's write is still on the wire.

    m_controller->open(QStringLiteral("/foo/b.txt"));
    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/b.txt"));
    respondResult(reqId(read), {{"path", "/foo/b.txt"},
                                {"encoding", "utf-8"},
                                {"content", "bbb"},
                                {"revision", "rb1"},
                                {"truncated", false}});
    serveWatchThenNoRecovery();

    QSignalSpy savedSpy(m_controller, &EditorController::saved);

    m_controller->save(QStringLiteral("b-edited"), QStringLiteral("rb1"));
    const QJsonObject writeB1 = nextRequest();
    QVERIFY2(method(writeB1) == kWriteFile,
             "the new file's save queued behind a write belonging to the file "
             "the pane had already left");
    QCOMPARE(reqPath(writeB1), QStringLiteral("/foo/b.txt"));

    // The abandoned file's write finally answers. It must decide nothing here.
    respondResult(reqId(writeA), {{"path", "/foo/a.txt"}, {"revision", "ra2"}});
    QTest::qWait(100);
    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(m_controller->revision(), QStringLiteral("rb1"));

    // ...including releasing /foo/b.txt's chain: this third save must still
    // queue behind writeB1, not go straight out guarded by the stale rb1.
    m_controller->save(QStringLiteral("b-edited-again"), QStringLiteral("rb1"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "the previous file's reply released the current file's save chain");

    respondResult(reqId(writeB1), {{"path", "/foo/b.txt"}, {"revision", "rb2"}});
    const QJsonObject writeB2 = nextRequest();
    QCOMPARE(method(writeB2), kWriteFile);
    QCOMPARE(reqPath(writeB2), QStringLiteral("/foo/b.txt"));
    QCOMPARE(reqContent(writeB2), QStringLiteral("b-edited-again"));
    QCOMPARE(reqExpectedRevision(writeB2), QStringLiteral("rb2"));
    QCOMPARE(savedSpy.count(), 0);

    respondResult(reqId(writeB2), {{"path", "/foo/b.txt"}, {"revision", "rb3"}});
    QTRY_COMPARE(savedSpy.count(), 1);
    QCOMPARE(savedSpy.at(0).at(0).toString(), QStringLiteral("rb3"));
}

// The mirror image: a reload the USER asked for (the page's conflict/error
// "Reload" button calls requestReload) DOES replace the buffer — and must then
// retire the crash-recovery snapshot of the edits it just threw away, or the
// next time the pane opens this file it offers back "unsaved changes" the user
// deliberately discarded (SPEC 11.3).
void TstEditorController::anExplicitReloadDiscardsTheRecoverySnapshotItThrewAway()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("edits the user will discard"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QCOMPARE(reqPath(snapshot), recoveryFilePath());
    respondResult(reqId(snapshot),
                  {{"path", reqPath(snapshot)}, {"revision", "rec1"}});
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    m_controller->requestReload();

    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "orig"},
                                {"revision", "r1"},
                                {"truncated", false}});

    // The snapshot is truncated by a zero-length, revision-GUARDED write (the
    // C1 catalog has no delete), guarded on the snapshot's own revision so a
    // slot another writer replaced is refused rather than destroyed.
    const QJsonObject clear = nextRequest();
    QCOMPARE(method(clear), kWriteFile);
    QCOMPARE(reqPath(clear), recoveryFilePath());
    QCOMPARE(reqContent(clear), QString());
    QCOMPARE(reqExpectedRevision(clear), QStringLiteral("rec1"));
    QCOMPARE(reqMode(clear), 0600);
    respondResult(reqId(clear), {{"path", reqPath(clear)}, {"revision", "rec2"}});

    servePermissionStat();

    // The buffer WAS replaced (that is what was asked for) and is clean again.
    QTRY_COMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(), QStringLiteral("orig"));
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
}

// The Monaco page can reload underneath the controller (a renderer crash, a
// re-navigated bundle): it re-runs its handshake, and its buffer is gone. When
// that buffer held unsaved work the ONLY surviving copy is this pane's recovery
// snapshot, and only an open() probes for one — so a page reload over a dirty
// buffer must re-open the file and offer the snapshot back, not quietly re-read
// the server's bytes and bury it (SPEC 11.3).
void TstEditorController::aPageReloadWithUnsavedWorkIsOfferedItsRecoverySnapshot()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("work only the page holds"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QCOMPARE(reqPath(snapshot), recoveryFilePath());
    respondResult(reqId(snapshot),
                  {{"path", reqPath(snapshot)}, {"revision", "rec1"}});
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));

    QSignalSpy recoverySpy(m_controller, &EditorController::recoveryAvailable);

    // The page reloads: a SECOND ready() with nothing held to replay.
    m_controller->ready();

    // A full re-open, so the old watcher is released first (SPEC 8.7).
    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    QCOMPARE(reqSubscriptionId(unwatch), QStringLiteral("sub1"));

    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "orig"},
                                {"revision", "r1"},
                                {"truncated", false}});

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    servePermissionStat();

    const QJsonObject recStat = nextRequest();
    QCOMPARE(method(recStat), kStat);
    QCOMPARE(reqPath(recStat), recoveryFilePath());
    respondResult(reqId(recStat),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});

    const QJsonObject recRead = nextRequest();
    QCOMPARE(method(recRead), kReadFile);
    QCOMPARE(reqPath(recRead), recoveryFilePath());
    respondResult(
        reqId(recRead),
        {{"path", recoveryFilePath()},
         {"encoding", "utf-8"},
         {"content", snapshotEnvelope(QStringLiteral("/foo/f.txt"),
                                      QStringLiteral("work only the page holds"))},
         {"revision", "rec1"},
         {"truncated", false}});

    QTRY_COMPARE(recoverySpy.count(), 1);
    QCOMPARE(recoverySpy.at(0).at(0).toString(),
             QStringLiteral("work only the page holds"));
}

// EditorFactory::create() is a Q_INVOKABLE, so a controller returned with no
// parent reaches QML with JavaScriptOwnership and can be collected while a live
// pane is still driving it. With no pane to parent to, the factory keeps it.
void TstEditorController::anUnownedControllerIsKeptAliveByTheFactory()
{
    QPointer<EditorController> orphan;
    QPointer<EditorController> owned;
    QObject pane;
    {
        // A null client is enough here: the constructor only connects to one if
        // it has one, and this case is about QObject ownership, not RPC.
        EditorFactory factory(nullptr);
        orphan = factory.create();
        QVERIFY(orphan);
        QCOMPARE(orphan->parent(), &factory);

        // The empty paneId that goes with the default call has NO bearing on
        // ownership. It only leaves the recovery key unset, which disables the
        // per-pane snapshot until EditorPaneView pushes one in (SPEC 11.3) —
        // exactly what setRecoveryId() exists for.
        QVERIFY(orphan->recoveryId().isEmpty());
        orphan->setRecoveryId(QStringLiteral("settled-later"));
        QCOMPARE(orphan->recoveryId(), QStringLiteral("settled-later"));
        QCOMPARE(orphan->parent(), &factory);

        // A real pane still owns its own controller; the fallback is only for
        // the null case.
        owned = factory.create(&pane, QStringLiteral("pane-7"));
        QVERIFY(owned);
        QCOMPARE(owned->parent(), &pane);
        QCOMPARE(owned->recoveryId(), QStringLiteral("pane-7"));
    }

    QVERIFY2(orphan.isNull(),
             "the factory did not own the controller it returned unparented");
    QVERIFY2(!owned.isNull(),
             "the factory destroyed a controller a live pane owns");
}

// A file the daemon's STRICT UTF-8 decoder refused comes back base64 carrying
// the file's exact bytes (remote/src/files.ts). Those bytes are what the user
// asked to see: the pane must show the file with replacement characters where
// the invalid sequences are, NOT the base64 text. It stays read-only and
// unsaveable, because that is derived from the wire encoding and not from what
// the payload turned out to contain.
void TstEditorController::aBase64ReadIsDecodedBeforeItReachesThePage()
{
    makePair();

    // "hello\xffworld\n" — valid base64, and NOT valid UTF-8 once decoded.
    const QString encoded = QStringLiteral("aGVsbG//d29ybGQK");
    const QString decoded = QStringLiteral("hello\uFFFDworld\n");

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/mixed.log"));
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/mixed.log"},
                                {"encoding", "base64"},
                                {"content", encoded},
                                {"revision", "r1"},
                                {"truncated", false}});

    QTRY_COMPARE(contentSpy.count(), 1);
    QVERIFY2(contentSpy.at(0).at(0).toString() != encoded,
             "the raw base64 payload was pushed at the page as if it were the file");
    QCOMPARE(contentSpy.at(0).at(0).toString(), decoded);
    QCOMPARE(m_controller->revision(), QStringLiteral("r1"));

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});
    servePermissionStat(0644); // the FILE is writable; the BUFFER still is not
    QTRY_COMPARE(m_controller->readOnly(), true);

    // ...and the save path still refuses it, which is what keeps a decoded
    // buffer (replacement characters and all) from ever being written back over
    // the bytes it stands in for.
    QSignalSpy errorSpy(m_controller, &EditorController::saveError);
    m_controller->save(decoded, QStringLiteral("r1"));
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY2(nextRequest(300).isEmpty(),
             "a decoded base64 buffer was written back over the file");

    // The reload path reads the same wire shape and must decode it too. The
    // buffer is clean, so an external change re-reads on its own (SPEC 8.7).
    contentSpy.clear();
    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/mixed.log"},
                                   {"event", "modified"},
                                   {"revision", "r2"}});
    const QJsonObject reread = nextRequest();
    QCOMPARE(method(reread), kReadFile);
    respondResult(reqId(reread), {{"path", "/foo/mixed.log"},
                                  {"encoding", "base64"},
                                  {"content", "YWdhaW7+/mFnYWluCg=="},
                                  {"revision", "r2"},
                                  {"truncated", false}});
    servePermissionStat(0644);

    QTRY_COMPARE(contentSpy.count(), 1);
    QCOMPARE(contentSpy.at(0).at(0).toString(),
             QStringLiteral("again\uFFFD\uFFFDagain\n"));
    QCOMPARE(m_controller->readOnly(), true);
}

// A base64 payload that is not valid base64 is a server bug or a corrupted
// frame: there is nothing legible to show, and showing the payload would be
// worse than showing nothing. It is an honest read failure, and it must not
// half-apply — the buffer, its revision and its recovery snapshot stay put.
void TstEditorController::anUndecodableBase64ReadIsAReadFailure()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("good"),
              QStringLiteral("r1"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);

    sendNotification(kWatchEvent, {{"subscriptionId", "sub1"},
                                   {"path", "/foo/f.txt"},
                                   {"event", "modified"},
                                   {"revision", "r2"}});
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "base64"},
                                {"content", "not %% base64 at all"},
                                {"revision", "r2"},
                                {"truncated", false}});

    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("error"));
    QCOMPARE(contentSpy.count(), 0);
    QVERIFY2(m_controller->revision() == QStringLiteral("r1"),
             "an undecodable read re-baselined the buffer's save guard");
    QVERIFY2(nextRequest(300).isEmpty(),
             "an undecodable read still re-derived permissions or truncated the "
             "recovery snapshot");
}

// A reload the USER asked for is allowed to replace the buffer with a write
// still on the wire (aSaveReplyIsDroppedAfterAnExplicitReloadTookOverTheBuffer
// pins that). What must ALSO happen is that the reload retires that write's
// save chain. Otherwise the next Ctrl+S sees "a save is already in flight",
// parks the user's bytes in the queue, and the old write's reply — which the
// load-generation guard drops — throws them away without emitting saved,
// saveConflict or saveError. The page would then be left believing the file was
// written, the pane would call the buffer clean, and the next external change
// would auto-reload straight over work that exists nowhere else.
void TstEditorController::aSaveIssuedAfterAnExplicitReloadIsNotSwallowedByTheOldWrite()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("hello"),
              QStringLiteral("r1"));

    QSignalSpy savedSpy(m_controller, &EditorController::saved);
    QSignalSpy conflictSpy(m_controller, &EditorController::saveConflict);
    QSignalSpy errorSpy(m_controller, &EditorController::saveError);

    m_controller->save(QStringLiteral("the user's edits"), QStringLiteral("r1"));
    const QJsonObject abandoned = nextRequest();
    QCOMPARE(method(abandoned), kWriteFile);
    // Deliberately left unanswered: this write stays on the wire throughout.

    // The user presses Reload while it is still out there.
    m_controller->requestReload();
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "the server's bytes"},
                                {"revision", "r9"},
                                {"truncated", false}});
    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    respondResult(reqId(stat), {{"path", "/foo/f.txt"},
                                {"revision", "r9"},
                                {"writable", true}});
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // The user edits the reloaded buffer and saves it. This save belongs to the
    // buffer on screen and must go out NOW, guarded by the revision the reload
    // adopted — not queue behind a write for bytes the pane has discarded.
    m_controller->save(QStringLiteral("typed after the reload"),
                       QStringLiteral("r9"));
    const QJsonObject write = nextRequest();
    QVERIFY2(method(write) == kWriteFile,
             "the save queued behind the write the reload superseded, where its "
             "reply would silently throw the user's bytes away");
    QCOMPARE(reqPath(write), QStringLiteral("/foo/f.txt"));
    QCOMPARE(reqContent(write), QStringLiteral("typed after the reload"));
    QCOMPARE(reqExpectedRevision(write), QStringLiteral("r9"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("saving"));

    // The abandoned write finally answers. It decides nothing: not the pane's
    // state, not the revision, and emphatically not this chain's flag.
    respondResult(reqId(abandoned), {{"path", "/foo/f.txt"}, {"revision", "r2"}});
    QTest::qWait(100);
    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(m_controller->fileState(), QStringLiteral("saving"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r9"));

    // The live write is the one outcome the page sees.
    respondResult(reqId(write), {{"path", "/foo/f.txt"}, {"revision", "r10"}});
    QTRY_COMPARE(savedSpy.count(), 1);
    QCOMPARE(savedSpy.at(0).at(0).toString(), QStringLiteral("r10"));
    QCOMPARE(m_controller->revision(), QStringLiteral("r10"));
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(conflictSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);
}

// The Monaco page can reload at any moment (a renderer crash, a retargeted
// bundle) — including while the SSH session is down. Its buffer is gone, so it
// needs the file back, but nothing can be fetched over a dead transport: the
// request fails synchronously AFTER open() has already cleared the dirty flag,
// the revision baseline and the whole recovery slot. The pane would end up
// blank, claiming "clean", with the only copy of the user's unsaved work sitting
// in a snapshot nothing would ever probe again. The fetch is therefore deferred
// to the next transport bind, which performs it in full.
void TstEditorController::aPageReloadDuringAnOutageIsServedWhenTheTransportReturns()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    m_controller->reportContent(QStringLiteral("work only the page holds"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QCOMPARE(reqPath(snapshot), recoveryFilePath());
    respondResult(reqId(snapshot),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));

    QSignalSpy contentSpy(m_controller, &EditorController::contentLoaded);
    QSignalSpy recoverySpy(m_controller, &EditorController::recoveryAvailable);

    // The session dies. Not reconnectTransport(): the page has to reload in the
    // middle of the outage, so the drop and the rebind are separate steps here.
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    m_clientSide->disconnectFromServer();
    QTRY_COMPARE(closedSpy.count(), 1);
    m_client->setTransport(nullptr);
    delete m_serverSide;
    m_serverSide = nullptr;
    delete m_clientSide;
    m_clientSide = nullptr;
    delete m_server;
    m_server = nullptr;
    m_serverBuf.clear();
    QCOMPARE(m_controller->fileState(), QStringLiteral("disconnected"));

    // The page reloads while there is nothing to fetch over: a second ready()
    // with no held buffer to replay. Nothing may be mutated or attempted.
    m_controller->ready();
    QTest::qWait(100);
    QCOMPARE(m_controller->fileState(), QStringLiteral("disconnected"));
    QCOMPARE(contentSpy.count(), 0);

    // The replacement server arrives, and the debt is paid. The buffer was
    // dirty, so this is a full re-open — which is what puts the crash-recovery
    // snapshot back in front of the user (SPEC 11.3). No file.unwatch: the id
    // died with the process that minted it.
    makePair();

    const QJsonObject read = nextRequest();
    QVERIFY2(method(read) == kReadFile,
             "the reloaded page was never served: the pane re-subscribed and "
             "reconciled as if its buffer were still on screen");
    QCOMPARE(reqPath(read), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "orig"},
                                {"revision", "r1"},
                                {"truncated", false}});

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    QCOMPARE(reqPath(watch), QStringLiteral("/foo/f.txt"));
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    servePermissionStat();

    const QJsonObject recStat = nextRequest();
    QCOMPARE(method(recStat), kStat);
    QCOMPARE(reqPath(recStat), recoveryFilePath());
    respondResult(reqId(recStat),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});

    const QJsonObject recRead = nextRequest();
    QCOMPARE(method(recRead), kReadFile);
    QCOMPARE(reqPath(recRead), recoveryFilePath());
    respondResult(
        reqId(recRead),
        {{"path", recoveryFilePath()},
         {"encoding", "utf-8"},
         {"content", snapshotEnvelope(QStringLiteral("/foo/f.txt"),
                                      QStringLiteral("work only the page holds"))},
         {"revision", "rec1"},
         {"truncated", false}});

    QTRY_COMPARE(contentSpy.count(), 1);
    QTRY_COMPARE(recoverySpy.count(), 1);
    QCOMPARE(recoverySpy.at(0).at(0).toString(),
             QStringLiteral("work only the page holds"));
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // The debt is settled, not standing: a later rebind reconciles normally
    // instead of re-opening the file underneath the page.
    reconnectTransport();
    const QJsonObject rewatch = nextRequest();
    QCOMPARE(method(rewatch), kWatch);
    respondResult(reqId(rewatch), {{"subscriptionId", "sub3"}});
    const QJsonObject reconcile = nextRequest();
    QCOMPARE(method(reconcile), kStat);
    QCOMPARE(reqPath(reconcile), QStringLiteral("/foo/f.txt"));
}

// Every refusal save() makes has to be audible. The page hands its buffer over
// fire-and-forget, cancels the crash-recovery timer it had armed, and then waits
// for saved / saveConflict / saveError; a silent return leaves the user believing
// the file was written when nothing was even sent.
void TstEditorController::aSaveWithNothingToWriteToIsRefusedWithAReason()
{
    makePair();

    // Nothing has been opened in this pane, so there is no path to write to.
    QSignalSpy errorSpy(m_controller, &EditorController::saveError);
    m_controller->save(QStringLiteral("bytes with nowhere to go"), QString());
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(
                 QStringLiteral("no file is open"), Qt::CaseInsensitive),
             qPrintable(errorSpy.at(0).at(0).toString()));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a save with no file open reached the server");

    // ...and a controller whose borrowed RPC client has been destroyed (the
    // session is being torn down) says so rather than swallowing the save.
    EditorController clientless(nullptr, kPaneId);
    QSignalSpy clientlessErrors(&clientless, &EditorController::saveError);
    clientless.save(QStringLiteral("bytes with nobody to send them to"),
                    QStringLiteral("r1"));
    QCOMPARE(clientlessErrors.count(), 1);
    QVERIFY2(clientlessErrors.at(0).at(0).toString().contains(
                 QStringLiteral("disconnected"), Qt::CaseInsensitive),
             qPrintable(clientlessErrors.at(0).at(0).toString()));
}

// The twin of recoveryDirArrivingAfterOpenStillOffersSnapshot, for the OTHER
// half of the recovery key. The pane's layout id is assigned by the region's
// layout logic and can settle after the controller exists — EditorPaneView
// pushes it in through setRecoveryId() — so the slot it names has never been
// probed. Until it arrives recovery is DISABLED rather than sharing one unkeyed
// snapshot file between panes.
void TstEditorController::recoveryIdArrivingAfterOpenStillOffersSnapshot()
{
    makePair();
    m_controller->setRecoveryId(QString());
    m_controller->ready();
    m_controller->open(QStringLiteral("/foo/f.txt"));

    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "orig"},
                                {"revision", "r1"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});
    servePermissionStat();
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
    QVERIFY2(nextRequest(300).isEmpty(),
             "a pane with no stable id probed a recovery slot it cannot own");

    QSignalSpy recoverySpy(m_controller, &EditorController::recoveryAvailable);
    m_controller->setRecoveryId(kPaneId);
    QCOMPARE(m_controller->recoveryId(), kPaneId);

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), recoveryFilePath());
    respondResult(reqId(stat),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});

    const QJsonObject recRead = nextRequest();
    QCOMPARE(method(recRead), kReadFile);
    QCOMPARE(reqPath(recRead), recoveryFilePath());
    respondResult(reqId(recRead),
                  {{"path", recoveryFilePath()},
                   {"encoding", "utf-8"},
                   {"content", snapshotEnvelope(QStringLiteral("/foo/f.txt"),
                                                QStringLiteral("recovered"))},
                   {"revision", "rec1"},
                   {"truncated", false}});

    QTRY_COMPARE(recoverySpy.count(), 1);
    QCOMPARE(recoverySpy.at(0).at(0).toString(), QStringLiteral("recovered"));
}

// There is ONE snapshot file per pane and the pane REUSES it as it switches
// files, which is why the snapshot is a JSON envelope recording the path it
// belongs to. A snapshot this pane wrote for a file it has since left must never
// be handed back as the current file's unsaved work: accepting it would paste
// one file's text into another.
void TstEditorController::aSnapshotFromAPreviousFileIsNotOfferedAsThisOnes()
{
    makePair();
    openClean(QStringLiteral("/foo/a.txt"), QStringLiteral("A"),
              QStringLiteral("ra1"));

    m_controller->reportContent(QStringLiteral("edits in a.txt"));
    const QJsonObject snapshot = nextRequest();
    QCOMPARE(method(snapshot), kWriteFile);
    QCOMPARE(reqPath(snapshot), recoveryFilePath());
    QCOMPARE(snapshotPathOf(snapshot), QStringLiteral("/foo/a.txt"));
    respondResult(reqId(snapshot),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});

    // Switch to another file. Its recovery slot is the very same file.
    QSignalSpy recoverySpy(m_controller, &EditorController::recoveryAvailable);
    m_controller->open(QStringLiteral("/foo/b.txt"));
    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    respondResult(reqId(unwatch), {});
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    QCOMPARE(reqPath(read), QStringLiteral("/foo/b.txt"));
    respondResult(reqId(read), {{"path", "/foo/b.txt"},
                                {"encoding", "utf-8"},
                                {"content", "B"},
                                {"revision", "rb1"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});
    servePermissionStat();

    const QJsonObject recStat = nextRequest();
    QCOMPARE(method(recStat), kStat);
    QCOMPARE(reqPath(recStat), recoveryFilePath());
    respondResult(reqId(recStat),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});

    const QJsonObject recRead = nextRequest();
    QCOMPARE(method(recRead), kReadFile);
    respondResult(reqId(recRead),
                  {{"path", recoveryFilePath()},
                   {"encoding", "utf-8"},
                   {"content", snapshotEnvelope(QStringLiteral("/foo/a.txt"),
                                                QStringLiteral("edits in a.txt"))},
                   {"revision", "rec1"},
                   {"truncated", false}});

    QTest::qWait(100);
    QVERIFY2(recoverySpy.isEmpty(),
             "a snapshot belonging to the PREVIOUS file was offered as this "
             "file's unsaved work");
    QCOMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // The slot's revision was still adopted, so b.txt's own snapshot is a
    // guarded overwrite rather than a create the server would refuse.
    m_controller->reportContent(QStringLiteral("edits in b.txt"));
    const QJsonObject next = nextRequest();
    QCOMPARE(method(next), kWriteFile);
    QCOMPARE(reqPath(next), recoveryFilePath());
    QCOMPARE(reqExpectedRevision(next), QStringLiteral("rec1"));
    QCOMPARE(snapshotPathOf(next), QStringLiteral("/foo/b.txt"));
}

// The snapshot is read back through the ordinary file.readFile wire shape, so it
// arrives base64 whenever the daemon's strict UTF-8 decoder refuses its bytes.
// Reading that payload as if it were the text hands the envelope parser a wall
// of base64, which does not parse — and the user's unsaved work is silently
// dropped from the offer. It must be decoded first, exactly as open() and
// reload() decode the file itself.
void TstEditorController::aBase64SnapshotIsDecodedBeforeItIsOffered()
{
    makePair();
    openClean(QStringLiteral("/foo/f.txt"), QStringLiteral("orig"),
              QStringLiteral("r1"));

    QSignalSpy recoverySpy(m_controller, &EditorController::recoveryAvailable);

    m_controller->open(QStringLiteral("/foo/f.txt"));
    const QJsonObject unwatch = nextRequest();
    QCOMPARE(method(unwatch), kUnwatch);
    respondResult(reqId(unwatch), {});
    const QJsonObject read = nextRequest();
    QCOMPARE(method(read), kReadFile);
    respondResult(reqId(read), {{"path", "/foo/f.txt"},
                                {"encoding", "utf-8"},
                                {"content", "orig"},
                                {"revision", "r1"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});
    servePermissionStat();

    const QJsonObject recStat = nextRequest();
    QCOMPARE(method(recStat), kStat);
    QCOMPARE(reqPath(recStat), recoveryFilePath());
    respondResult(reqId(recStat),
                  {{"path", recoveryFilePath()}, {"revision", "rec1"}});

    const QString envelope = snapshotEnvelope(QStringLiteral("/foo/f.txt"),
                                              QStringLiteral("recovered edits"));
    const QJsonObject recRead = nextRequest();
    QCOMPARE(method(recRead), kReadFile);
    respondResult(
        reqId(recRead),
        {{"path", recoveryFilePath()},
         {"encoding", "base64"},
         {"content", QString::fromLatin1(envelope.toUtf8().toBase64())},
         {"revision", "rec1"},
         {"truncated", false}});

    QTRY_COMPARE(recoverySpy.count(), 1);
    QCOMPARE(recoverySpy.at(0).at(0).toString(), QStringLiteral("recovered edits"));
}

QTEST_GUILESS_MAIN(TstEditorController)
#include "tst_editorcontroller.moc"
