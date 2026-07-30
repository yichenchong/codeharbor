#include "CodeharbordClient.h"
#include "EditorController.h"
#include "RpcTypes.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
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

const auto kReadFile = QString::fromLatin1(ch::rpc::kMethodReadFile);
const auto kWriteFile = QString::fromLatin1(ch::rpc::kMethodWriteFile);
const auto kWatch = QString::fromLatin1(ch::rpc::kMethodWatch);
const auto kUnwatch = QString::fromLatin1(ch::rpc::kMethodUnwatch);
const auto kStat = QString::fromLatin1(ch::rpc::kMethodStat);
const auto kWatchEvent = QString::fromLatin1(ch::rpc::kWatchEventNotification);

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
    void saveSuccessAdoptsRevisionAndCleans();
    void saveStaleRevisionConflictsNoOverwrite();
    void saveConflictFallsBackToStat();
    void externalChangeWhileCleanReloads();
    void externalChangeWhileDirtyDoesNotReload();
    void recoverySnapshotWrittenAndOfferedOnReopen();
    void unwatchIssuedOnDestruction();
    void reopenUnwatchesPreviousSubscription();
    void contentBufferedUntilPageReportsReady();
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
    void editsDuringASaveSurviveTheSaveReply();
    void reportContentDuringALoadIsIgnored();

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
    m_controller = new EditorController(m_client);
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
    QVERIFY2(!reqPath(stat).contains(QStringLiteral(".codeharbor-recovery")),
             "the permission stat must target the file itself");
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
    QVERIFY(reqPath(stat).contains(QStringLiteral(".codeharbor-recovery")));
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
    QVERIFY(reqPath(rec).contains(QStringLiteral(".codeharbor-recovery")));
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
    QVERIFY(recoveryPath.contains(QStringLiteral(".codeharbor-recovery")));
    QCOMPARE(reqContent(rec), QStringLiteral("recovered edits"));
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
                                   {"content", "recovered edits"},
                                   {"revision", "rec1"},
                                   {"truncated", false}});

    QTRY_COMPARE(recoverySpy.count(), 1);
    QCOMPARE(recoverySpy.at(0).at(0).toString(), QStringLiteral("recovered edits"));
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
    QVERIFY(reqPath(stat).contains(QStringLiteral(".codeharbor-recovery")));
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
    QVERIFY(recoveryPath.contains(QStringLiteral(".codeharbor-recovery")));
    QCOMPARE(reqContent(snapshot), QStringLiteral("edited"));
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
    QVERIFY(reqPath(snapshot).contains(QStringLiteral(".codeharbor-recovery")));
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
    QVERIFY(reqPath(snapshot).contains(QStringLiteral(".codeharbor-recovery")));
    QCOMPARE(reqContent(snapshot), QStringLiteral("first edit + more typing"));
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
    QVERIFY(reqPath(snapshot).contains(QStringLiteral(".codeharbor-recovery")));
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

QTEST_GUILESS_MAIN(TstEditorController)
#include "tst_editorcontroller.moc"
