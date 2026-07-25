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

private:
    void makePair();
    QJsonObject nextRequest(int timeoutMs = 3000);
    void respondResult(int id, const QJsonObject& result);
    void respondError(int id, int code, const QString& message,
                      const QJsonValue& data = QJsonValue());
    void sendNotification(const QString& m, const QJsonObject& params);
    // Drain the watch subscription + (absent) recovery stat that follow a load.
    void serveWatchThenNoRecovery();
    // ready() (the page is connected, so content flows straight through) then
    // open(path) -> readFile(content,rev) -> Clean, plus watch + empty recovery.
    void openClean(const QString& path, const QString& content, const QString& rev);

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

void TstEditorController::serveWatchThenNoRecovery()
{
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub1"}});

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
                                {"encoding", "utf8"},
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
                                {"encoding", "utf8"},
                                {"content", "externally edited"},
                                {"revision", "r5"},
                                {"truncated", false}});

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
                                {"encoding", "utf8"},
                                {"content", "orig"},
                                {"revision", "r1"},
                                {"truncated", false}});

    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

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
                                   {"encoding", "utf8"},
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
                                {"encoding", "utf8"},
                                {"content", "B"},
                                {"revision", "r2"},
                                {"truncated", false}});

    // The new file subscribes its own watcher ("sub2").
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    QCOMPARE(reqPath(watch), QStringLiteral("/foo/b.txt"));
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
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
                                {"encoding", "utf8"},
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
                                   {"encoding", "utf8"},
                                   {"content", "loaded before the page"},
                                   {"revision", "r1"},
                                   {"truncated", false}});
    QTRY_COMPARE(contentSpy.count(), 2);
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
                                {"encoding", "utf8"},
                                {"content", "edited"},
                                {"revision", "r2"},
                                {"truncated", false}});
    const QJsonObject watch = nextRequest();
    QCOMPARE(method(watch), kWatch);
    respondResult(reqId(watch), {{"subscriptionId", "sub2"}});

    const QJsonObject stat = nextRequest();
    QCOMPARE(method(stat), kStat);
    QCOMPARE(reqPath(stat), recoveryPath);
    respondResult(reqId(stat), {{"path", recoveryPath}, {"revision", "rec2"}});

    const QJsonObject recRead = nextRequest();
    QCOMPARE(method(recRead), kReadFile);
    QCOMPARE(reqPath(recRead), recoveryPath);
    respondResult(reqId(recRead), {{"path", recoveryPath},
                                   {"encoding", "utf8"},
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

QTEST_GUILESS_MAIN(TstEditorController)
#include "tst_editorcontroller.moc"
