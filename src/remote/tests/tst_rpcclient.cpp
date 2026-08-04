#include "CodeharbordClient.h"
#include "RpcTypes.h"

#include <QCoreApplication>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QString>
#include <QtTest/QtTest>
#include <QScopeGuard>

#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

using ch::CodeharbordClient;
using ch::RpcError;

namespace {

// Serialize a JSON-RPC object as one framed (newline-terminated) wire line.
QByteArray jsonLine(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

// An in-process QIODevice the test drives byte for byte. Three things a real
// socket cannot be made to do on demand live here:
//  * queueSilently() followed by announceEof() delivers an end-of-stream while a
//    COMPLETE frame is still sitting unread. Qt is free to pick that ordering on
//    a real socket, and it is the ordering that used to lose the response.
//  * deliver() hands over a chunk and announces it SYNCHRONOUSLY, so a test can
//    observe exactly what one readyRead() of a multi-frame chunk does.
//  * setShortWrite() makes write() report fewer bytes than it was handed — the
//    truncated-frame case ch::SshChannelDevice::writeData() really can produce.
// No Q_OBJECT: only inherited QIODevice signals are emitted, so no moc is needed.
class ScriptedDevice : public QIODevice {
public:
    ScriptedDevice() { open(QIODevice::ReadWrite); }

    // Make bytes readable WITHOUT emitting readyRead().
    void queueSilently(const QByteArray& bytes) { m_in += bytes; }
    void announceEof() { emit readChannelFinished(); }
    // Make bytes readable AND announce them, all inside this call.
    void deliver(const QByteArray& bytes)
    {
        m_in += bytes;
        emit readyRead();
    }
    void setWriteHook(std::function<void(const QByteArray&)> hook)
    {
        m_writeHook = std::move(hook);
    }
    void setShortWrite(bool on) { m_shortWrite = on; }
    // Everything the client has written since the last call, as raw wire bytes.
    QByteArray takeWritten() { return std::exchange(m_out, QByteArray()); }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override
    {
        return m_in.size() + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_in.size());
        std::memcpy(data, m_in.constData(), static_cast<size_t>(n));
        m_in.remove(0, n);
        return n;
    }
    qint64 writeData(const char* data, qint64 maxSize) override
    {
        const qint64 n = m_shortWrite ? maxSize / 2 : maxSize;
        m_out.append(data, n);
        if (m_writeHook)
            m_writeHook(QByteArray(data, n));
        return n;
    }

    QByteArray m_in;
    QByteArray m_out;
    std::function<void(const QByteArray&)> m_writeHook;
    bool m_shortWrite = false;
};

// The JSON-RPC ids of every `ping` frame the client has written since the last
// call, in order, WITHOUT answering any of them. Any other frame the client
// wrote is consumed and ignored — the tests that care about those read them
// from takeWritten() themselves.
QList<qint64> takePingIds(ScriptedDevice& device)
{
    QList<qint64> ids;
    const QList<QByteArray> lines = device.takeWritten().split('\n');
    for (const QByteArray& line : lines) {
        if (line.isEmpty())
            continue;
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (request.value(QStringLiteral("method")).toString() ==
            QLatin1String(ch::rpc::kMethodPing))
            ids.append(request.value(QStringLiteral("id")).toInteger());
    }
    return ids;
}

// Answer one probe by id, exactly as a live daemon's keepalive handler would.
void answerPing(ScriptedDevice& device, qint64 id)
{
    device.deliver(jsonLine({{"jsonrpc", "2.0"},
                             {"id", id},
                             {"result", QJsonObject{{"pong", true}}}}));
}

// Answer every probe written since the last call, and report how many.
int answerPings(ScriptedDevice& device)
{
    const QList<qint64> ids = takePingIds(device);
    for (const qint64 id : ids)
        answerPing(device, id);
    return static_cast<int>(ids.size());
}

// How many probes the client has written since the last call, answering none.
int countPings(ScriptedDevice& device)
{
    return static_cast<int>(takePingIds(device).size());
}

// Spin the event loop until the client writes a probe, and return its id. Fails
// the calling test (returns 0) if none appears within `budgetMs`.
qint64 awaitPing(ScriptedDevice& device, int budgetMs = 2000)
{
    for (int waited = 0; waited < budgetMs; waited += 10) {
        const QList<qint64> ids = takePingIds(device);
        if (!ids.isEmpty())
            return ids.first();
        QTest::qWait(10);
    }
    return 0;
}

} // namespace

// Exercises the CodeharbordClient JSONL/JSON-RPC transport layer against a
// QLocalSocket pair (no network): the client's transport is one socket, the
// test writes canned server frames on the other. One best-effort live test
// spawns the real Node service.
class TstRpcClient : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void matchesResponsesById();
    void errorResponseDeliversRpcError();
    void partialLineThenCompleted();
    void notificationRouted();
    void unknownAndDuplicateIdWarn();
    void pendingFailOnClose();
    void bothResultAndErrorTreatedAsError();
    void neitherResultNorErrorFailsCallback();
    void errorNullFieldTreatedAsSuccess();
    void malformedErrorObjectWarns();
    void emptyMethodIsSentAndRouted();
    void responseWithInvalidJsonrpcFailsPending();
    void largeLineRoutes();
    void nonRoutableIdWarns();
    void crlfAndWhitespaceFraming();
    void utf8SplitAcrossChunkBoundary();
    void reentrantCallFromCallback();
    void paramsOmittedWhenNullPresentOtherwise();
    void setTransportTwiceDetachesOld();
    void callWithNoTransportFailsCallbackOnce();
    void callAfterTransportClosedFailsOnce();
    void callbackDeletingClientMidDispatchIsSafe();
    void reconnectFromDestructorSweepIsRefused();
    void destroyingClientFailsPendingCallbacks();
    void oversizedNewlinelessInputIsBounded();
    void oversizedDelimitedInputIsRejected();
    void detachingTransportFailsPendingOnce();
    void retryFromRebindFailureUsesNewTransport();
    void rebindAfterCloseRevivesClient();
    void closeCallbackRebindKeepsNewTransportAlive();
    void methodWithIdIsNotANotification();
    void responseCarryingMethodIsNeverAResult();
    void errorCodeOutOfIntRangeWarns();
    void rebindFromASweptCallbackAnnouncesBoundOnce();
    void nullCallbackIsTolerated();
    void nonObjectJsonWarns();
    void closeDrainProcessesAllBoundedChunks();
    void responseQueuedBeforeEofIsStillDelivered();
    void closeMidChunkDropsTheRestOfTheChunk();
    void nullResultIsSuccess();
    void completeFrameThenPartialInOneRead();
    void outOfRangeIdWarns();
    void notificationWithoutParamsRouted();
    void errorWithoutDataYieldsUndefinedData();
    void transportDestroyedWhileBoundFailsPending();
    void rebindSameDeviceAfterCloseRevivesClient();
    void shortWriteFailsCallAndClosesTransport();
    void synchronousResponseDuringWriteIsRouted();
    void transportBoundFiresOnlyForNonNullBind();
    void liveServerInfoOverProcess();
    void heartbeatAnsweredKeepsClientAliveAndOffThePendingCount();
    void heartbeatSilentPeerFailsPendingOnceAndClosesOnce();
    void silentPeerIsNotKilledWithTheHeartbeatDisabled();
    void heartbeatSparesASlowButLivePeer();
    void heartbeatStopsWhenTransportUnbound();
    void heartbeatWriteFailureDeletingClientIsSafe();
    void retiredProbeAnswerDoesNotClobberTheLiveProbe();
    void rebindFromInsideACallbackKeepsProbeBookkeepingStraight();
    void reentrantFeedFromACallbackDispatchesEveryFrameOnce();
    void aBurstOfFramesInOneReadIsRoutedOnce();
    void pendingCountIsNotNarrowedToInt();
    void reEnablingTheRunningHeartbeatIsANoOp();
    void repeatedlyReEnablingCannotSuppressSilenceDetection();
    void reArmingAgainstASilentPeerCannotPileUpProbes();
    void outgoingOversizedFrameIsRejectedWithoutClosingTransport();
    void decodeFileContentValidatesEncoding();

private:
    void makePair();
    // Second socket pair for the transport-swap cases. Both ends stay owned by
    // the caller's stack frame, so they outlive the swap under test.
    void makeSwapPair(QLocalServer* server, QLocalSocket* clientSide,
                      QLocalSocket** serverSide);

    QLocalServer* m_server = nullptr;
    QLocalSocket* m_clientSide = nullptr; // transport bound to the client
    QLocalSocket* m_serverSide = nullptr; // test writes canned frames here
    CodeharbordClient* m_client = nullptr;
    static int s_seq;
};

int TstRpcClient::s_seq = 0;

void TstRpcClient::init()
{
    m_client = new CodeharbordClient;
}

void TstRpcClient::cleanup()
{
    // Delete the client first so it disconnects from the transport cleanly.
    delete m_client;
    m_client = nullptr;
    delete m_serverSide;
    m_serverSide = nullptr;
    delete m_clientSide;
    m_clientSide = nullptr;
    delete m_server;
    m_server = nullptr;
}

void TstRpcClient::makePair()
{
    const QString name =
        QStringLiteral("ch_rpc_test_%1_%2")
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

void TstRpcClient::makeSwapPair(QLocalServer* server, QLocalSocket* clientSide,
                                QLocalSocket** serverSide)
{
    const QString name = QStringLiteral("ch_rpc_test_%1_swap_%2")
                             .arg(QCoreApplication::applicationPid())
                             .arg(++s_seq);
    QLocalServer::removeServer(name);
    QVERIFY(server->listen(name));

    clientSide->connectToServer(name);
    QVERIFY(clientSide->waitForConnected(2000));
    QVERIFY(server->waitForNewConnection(2000));
    *serverSide = server->nextPendingConnection();
    QVERIFY(*serverSide != nullptr);
}

void TstRpcClient::matchesResponsesById()
{
    makePair();

    QJsonValue r1;
    QJsonValue r2;
    bool f1 = false;
    bool f2 = false;

    const int id1 = m_client->call(
        QStringLiteral("file.stat"), QJsonObject{{"path", "/a"}},
        [&](QJsonValue res, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            r1 = res;
            f1 = true;
        });
    const int id2 = m_client->call(
        QStringLiteral("file.readFile"), QJsonObject{{"path", "/b"}},
        [&](QJsonValue res, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            r2 = res;
            f2 = true;
        });
    QVERIFY(id2 > id1);

    // Two responses, out of order, in a single chunk (multiple messages/read).
    QByteArray chunk;
    chunk += jsonLine({{"jsonrpc", "2.0"},
                       {"id", id2},
                       {"result", QJsonObject{{"path", "/b"}, {"content", "B"}}}});
    chunk += jsonLine({{"jsonrpc", "2.0"},
                       {"id", id1},
                       {"result", QJsonObject{{"path", "/a"}, {"size", 7}}}});
    m_serverSide->write(chunk);
    m_serverSide->flush();

    QTRY_VERIFY(f1 && f2);
    QCOMPARE(r1.toObject().value("size").toInt(), 7);
    QCOMPARE(r2.toObject().value("content").toString(), QStringLiteral("B"));
    QCOMPARE(m_client->pendingCount(), 0);
}

void TstRpcClient::errorResponseDeliversRpcError()
{
    makePair();

    std::optional<RpcError> got;
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("file.writeFile"), QJsonObject{{"path", "/x"}},
        [&](QJsonValue, std::optional<RpcError> err) {
            got = err;
            fired = true;
        });

    const QJsonObject err{{"code", ch::rpc::kRevisionMismatch},
                          {"message", "stale revision"},
                          {"data", QJsonObject{{"currentRevision", "r2"}}}};
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"error", err}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value());
    // RPC_REVISION_MISMATCH is surfaced like any other code.
    QCOMPARE(got->code, ch::rpc::kRevisionMismatch);
    QCOMPARE(got->code, -32001);
    QCOMPARE(got->message, QStringLiteral("stale revision"));
    QCOMPARE(got->data.toObject().value("currentRevision").toString(),
             QStringLiteral("r2"));
}

void TstRpcClient::partialLineThenCompleted()
{
    makePair();

    QJsonValue res;
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue r, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            res = r;
            fired = true;
        });

    const QByteArray full = jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"pong", true}}}});
    const int half = full.size() / 2; // split mid-JSON, before the trailing '\n'

    m_serverSide->write(full.left(half));
    m_serverSide->flush();
    QTest::qWait(150);
    QVERIFY(!fired); // no complete line yet -> callback must not fire

    m_serverSide->write(full.mid(half));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QVERIFY(res.toObject().value("pong").toBool());
}

void TstRpcClient::notificationRouted()
{
    makePair();

    QSignalSpy spy(m_client, &CodeharbordClient::notificationReceived);
    const QJsonObject params{{"subscriptionId", "s1"},
                             {"path", "/watched"},
                             {"event", "modified"}};
    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"method", ch::rpc::kWatchEventNotification},
                                  {"params", params}}));
    m_serverSide->flush();

    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(),
             QString::fromLatin1(ch::rpc::kWatchEventNotification));
    const QJsonValue routed = spy.at(0).at(1).value<QJsonValue>();
    QCOMPARE(routed.toObject().value("path").toString(),
             QStringLiteral("/watched"));
}

void TstRpcClient::unknownAndDuplicateIdWarn()
{
    makePair();

    QSignalSpy spy(m_client, &CodeharbordClient::protocolWarning);

    // Unknown id: no pending call to route to.
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", 4242}, {"result", QJsonObject{}}}));
    m_serverSide->flush();
    QTRY_COMPARE(spy.count(), 1);

    // Duplicate id: a second response after the callback already fired.
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError>) { fired = true; });
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);

    const int before = spy.count();
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    m_serverSide->flush();
    QTRY_COMPARE(spy.count(), before + 1);
}

void TstRpcClient::pendingFailOnClose()
{
    makePair();

    int failures = 0;
    std::optional<RpcError> last;
    for (int i = 0; i < 3; ++i) {
        m_client->call(QStringLiteral("file.stat"), QJsonObject{{"path", "/p"}},
                       [&](QJsonValue, std::optional<RpcError> err) {
                           if (err.has_value()) {
                               ++failures;
                               last = err;
                           }
                       });
    }
    QCOMPARE(m_client->pendingCount(), 3);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    m_serverSide->disconnectFromServer();

    QTRY_COMPARE(failures, 3);
    QCOMPARE(m_client->pendingCount(), 0);
    QCOMPARE(closedSpy.count(), 1); // idempotent: exactly one close signal
    QVERIFY(last.has_value());
    QCOMPARE(last->code, -32603);
}

void TstRpcClient::bothResultAndErrorTreatedAsError()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    std::optional<RpcError> got;
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("file.stat"), QJsonObject{{"path", "/a"}},
        [&](QJsonValue, std::optional<RpcError> err) {
            got = err;
            fired = true;
        });

    // JSON-RPC 2.0 forbids both fields; the client warns but resolves as error.
    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"result", QJsonObject{{"ok", true}}},
                                  {"error", QJsonObject{{"code", -32000},
                                                        {"message", "boom"}}}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32000);
    QCOMPARE(got->message, QStringLiteral("boom"));
    QTRY_COMPARE(warnSpy.count(), 1);
    QCOMPARE(m_client->pendingCount(), 0);
}

void TstRpcClient::neitherResultNorErrorFailsCallback()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    std::optional<RpcError> got;
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            got = err;
            fired = true;
        });

    // A response with neither result nor error is malformed: the pending call
    // must be failed (not left hanging) and the violation flagged.
    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"}, {"id", id}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32603);
    QTRY_COMPARE(warnSpy.count(), 1);
    QCOMPARE(m_client->pendingCount(), 0);
}

void TstRpcClient::errorNullFieldTreatedAsSuccess()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    std::optional<RpcError> got;
    QJsonValue res;
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("file.stat"), QJsonObject{{"path", "/a"}},
        [&](QJsonValue r, std::optional<RpcError> err) {
            res = r;
            got = err;
            fired = true;
        });

    // Many servers spell a success as {"result":…, "error": null}. The null
    // error must NOT be reported as a failure, and must not warn about "both".
    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"result", QJsonObject{{"size", 5}}},
                                  {"error", QJsonValue(QJsonValue::Null)}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(!got.has_value());
    QCOMPARE(res.toObject().value("size").toInt(), 5);
    QCOMPARE(warnSpy.count(), 0);
    QCOMPARE(m_client->pendingCount(), 0);
}

void TstRpcClient::malformedErrorObjectWarns()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    std::optional<RpcError> got;
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("file.stat"), QJsonObject{{"path", "/a"}},
        [&](QJsonValue, std::optional<RpcError> err) {
            got = err;
            fired = true;
        });

    // Error object without the required code/message: warn, but still fail the
    // callback (best effort, code defaults to 0) so the caller cannot hang.
    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"error", QJsonObject{{"reason", "nope"}}}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, 0);
    QTRY_COMPARE(warnSpy.count(), 1);

    // A second response whose `error` is not even an object: warn and fail with
    // the synthetic internal-error code.
    std::optional<RpcError> got2;
    bool fired2 = false;
    const int id2 = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            got2 = err;
            fired2 = true;
        });
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id2}, {"error", QStringLiteral("boom")}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired2);
    QVERIFY(got2.has_value());
    QCOMPARE(got2->code, -32603);
    QTRY_COMPARE(warnSpy.count(), 2);
    QCOMPARE(m_client->pendingCount(), 0);
}
void TstRpcClient::responseWithInvalidJsonrpcFailsPending()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    int failures = 0;
    std::optional<RpcError> last;
    const qint64 wrongVersionId =
        m_client->call(QStringLiteral("ping"), QJsonValue(),
                       [&](QJsonValue, std::optional<RpcError> err) {
                           ++failures;
                           last = err;
                       });
    const qint64 missingVersionId =
        m_client->call(QStringLiteral("ping"), QJsonValue(),
                       [&](QJsonValue, std::optional<RpcError> err) {
                           ++failures;
                           last = err;
                       });

    m_serverSide->write(jsonLine({{"jsonrpc", "1.0"},
                                  {"id", wrongVersionId},
                                  {"result", QJsonObject{}}}));
    m_serverSide->write(
        jsonLine({{"id", missingVersionId}, {"result", QJsonObject{}}}));
    m_serverSide->flush();

    QTRY_COMPARE(failures, 2);
    QVERIFY(last.has_value());
    QCOMPARE(last->code, -32603);
    QCOMPARE(warnSpy.count(), 2);
    QCOMPARE(m_client->pendingCount(), 0);
}
void TstRpcClient::emptyMethodIsSentAndRouted()
{
    makePair();

    std::optional<RpcError> got;
    bool fired = false;
    const qint64 id =
        m_client->call(QString(), QJsonValue(),
                       [&](QJsonValue, std::optional<RpcError> err) {
                           got = err;
                           fired = true;
                       });

    m_clientSide->flush();
    QVERIFY(m_serverSide->waitForReadyRead(2000));
    const QJsonObject request =
        QJsonDocument::fromJson(m_serverSide->readAll().trimmed()).object();
    QVERIFY(request.contains(QStringLiteral("method")));
    QCOMPARE(request.value(QStringLiteral("method")).toString(), QString());
    QCOMPARE(request.value(QStringLiteral("id")).toInteger(), id);

    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"error", QJsonObject{{"code", -32601},
                                        {"message", "Method not found"}}}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32601);
}



void TstRpcClient::largeLineRoutes()
{
    makePair();

    QJsonValue res;
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("file.readFile"), QJsonObject{{"path", "/big"}},
        [&](QJsonValue r, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            res = r;
            fired = true;
        });

    // A single very large frame (~2 MiB payload) must route correctly even when
    // the transport delivers it across many separate reads.
    const QString big(2 * 1024 * 1024, QChar('x'));
    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"result", QJsonObject{{"content", big}}}}));
    m_serverSide->flush();

    QTRY_VERIFY_WITH_TIMEOUT(fired, 8000);
    QCOMPARE(res.toObject().value("content").toString().size(), big.size());
    QCOMPARE(m_client->pendingCount(), 0);
}

void TstRpcClient::nonRoutableIdWarns()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError>) { fired = true; });

    // String id: unroutable (this client only issues integer ids).
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", "abc"}, {"result", QJsonObject{}}}));
    // Fractional id must NOT truncate and mis-route to the pending integer id.
    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", id + 0.5},
                                  {"result", QJsonObject{}}}));
    m_serverSide->flush();

    QTRY_COMPARE(warnSpy.count(), 2);
    QVERIFY(!fired);
    QCOMPARE(m_client->pendingCount(), 1); // still awaiting the real response
    // Complete the request before this test's stack locals go away. The client's
    // destructor correctly fails pending callbacks, but this callback captures
    // `fired` by reference and must not outlive it.
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QCOMPARE(m_client->pendingCount(), 0);
}

void TstRpcClient::crlfAndWhitespaceFraming()
{
    makePair();

    bool fired = false;
    QJsonValue res;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue r, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            res = r;
            fired = true;
        });

    // Blank and whitespace-only lines are skipped; the frame uses CRLF.
    QByteArray chunk = "\r\n   \r\n";
    const QByteArray body =
        QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"result", QJsonObject{{"pong", true}}}})
            .toJson(QJsonDocument::Compact);
    chunk += body + "\r\n";
    m_serverSide->write(chunk);
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(res.toObject().value("pong").toBool());
}

void TstRpcClient::utf8SplitAcrossChunkBoundary()
{
    makePair();

    bool fired = false;
    QString content;
    const int id = m_client->call(
        QStringLiteral("file.readFile"), QJsonObject{{"path", "/u"}},
        [&](QJsonValue r, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            content = r.toObject().value("content").toString();
            fired = true;
        });

    // "café ☕" — multibyte UTF-8; split the wire bytes mid-codepoint.
    const QByteArray full = jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", id},
         {"result", QJsonObject{{"content", QString::fromUtf8("caf\xC3\xA9 \xE2\x98\x95")}}}});
    // Find a split point inside a multibyte sequence (a continuation byte).
    int split = full.indexOf('\xC3') + 1; // between the two bytes of é
    QVERIFY(split > 0 && split < full.size());
    m_serverSide->write(full.left(split));
    m_serverSide->flush();
    QTest::qWait(100);
    QVERIFY(!fired);
    m_serverSide->write(full.mid(split));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QCOMPARE(content, QString::fromUtf8("caf\xC3\xA9 \xE2\x98\x95"));
}

void TstRpcClient::reentrantCallFromCallback()
{
    makePair();

    bool first = false;
    bool second = false;
    int secondId = -1;
    const int firstId = m_client->call(
        QStringLiteral("a"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError>) {
            first = true;
            // Re-enter: issue a fresh call from inside the dispatch. The pending
            // entry for firstId must already be erased (no iterator invalidation).
            secondId = m_client->call(
                QStringLiteral("b"), QJsonValue(),
                [&](QJsonValue, std::optional<RpcError>) { second = true; });
        });

    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", firstId}, {"result", QJsonObject{}}}));
    m_serverSide->flush();
    QTRY_VERIFY(first);
    QVERIFY(secondId > firstId);
    QCOMPARE(m_client->pendingCount(), 1); // only the re-entrant call remains

    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", secondId}, {"result", QJsonObject{}}}));
    m_serverSide->flush();
    QTRY_VERIFY(second);
    QCOMPARE(m_client->pendingCount(), 0);
}

void TstRpcClient::paramsOmittedWhenNullPresentOtherwise()
{
    makePair();

    // Null/undefined params must be omitted from the wire (JSON-RPC 2.0 params
    // is a structured value or absent, never null).
    m_client->call(QStringLiteral("ping"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError>) {});
    m_clientSide->flush();
    QVERIFY(m_serverSide->waitForReadyRead(2000));
    QByteArray sent = m_serverSide->readAll();
    QJsonObject req = QJsonDocument::fromJson(sent.trimmed()).object();
    QVERIFY(!req.contains(QStringLiteral("params")));
    QCOMPARE(req.value("jsonrpc").toString(), QStringLiteral("2.0"));
    QVERIFY(req.value("id").isDouble());

    // A real params object is carried through verbatim.
    m_client->call(QStringLiteral("file.stat"), QJsonObject{{"path", "/z"}},
                   [&](QJsonValue, std::optional<RpcError>) {});
    m_clientSide->flush();
    QVERIFY(m_serverSide->waitForReadyRead(2000));
    sent = m_serverSide->readAll();
    req = QJsonDocument::fromJson(sent.trimmed()).object();
    QVERIFY(req.contains(QStringLiteral("params")));
    QCOMPARE(req.value("params").toObject().value("path").toString(),
             QStringLiteral("/z"));
}

void TstRpcClient::setTransportTwiceDetachesOld()
{
    makePair();

    // Rebinding FAILS the in-flight call. The id was minted for the transport
    // being dropped, and a SPEC 5.6 reconnect binds a channel onto a different
    // `codeharbord` process that never heard of it, so preserving the callback
    // would hang its caller forever.
    int fired = 0;
    std::optional<RpcError> got;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            ++fired;
            got = err;
        });

    QLocalSocket* oldSide = m_serverSide;
    QLocalServer server2;
    QLocalSocket newClient;
    QLocalSocket* newServer = nullptr;
    makeSwapPair(&server2, &newClient, &newServer);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    m_client->setTransport(&newClient);

    QCOMPARE(fired, 1);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32603);
    QCOMPARE(m_client->pendingCount(), 0);
    // A swap is not a close: whoever swapped the transport already knows.
    QCOMPARE(closedSpy.count(), 0);

    // A frame on the OLD transport must be ignored (its hooks are disconnected)
    // and the stale id must not resurrect the callback.
    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    oldSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    oldSide->flush();
    QTest::qWait(150);
    QCOMPARE(fired, 1);

    // The NEW transport carries fresh work normally.
    bool second = false;
    const int id2 = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            second = true;
        });
    newServer->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id2}, {"result", QJsonObject{}}}));
    newServer->flush();
    QTRY_VERIFY(second);
    QCOMPARE(warnSpy.count(), 0);

    m_client->setTransport(nullptr);
}

// A detach is not a close, but it must still honour call()'s exactly-once
// guarantee: once the transport is gone, nobody is left who could ever answer
// the ids still in flight.
void TstRpcClient::detachingTransportFailsPendingOnce()
{
    makePair();

    int fired = 0;
    std::optional<RpcError> got;
    m_client->call(QStringLiteral("ping"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       ++fired;
                       got = err;
                   });
    QCOMPARE(m_client->pendingCount(), 1);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    QSignalSpy boundSpy(m_client, &CodeharbordClient::transportBound);
    m_client->setTransport(nullptr);

    QCOMPARE(fired, 1);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32603);
    QCOMPARE(m_client->pendingCount(), 0);
    QCOMPARE(closedSpy.count(), 0);
    QCOMPARE(boundSpy.count(), 0);
}

// The usual answer to a failed call is "retry". A retry issued from INSIDE the
// rebind sweep must go out on the transport being BOUND, not the one being
// dropped, and the pending entry it creates must survive the sweep that caused
// it.
void TstRpcClient::retryFromRebindFailureUsesNewTransport()
{
    makePair();

    bool retried = false;
    bool retryAnswered = false;
    int retryId = -1;
    m_client->call(QStringLiteral("first"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       QVERIFY(err.has_value());
                       retried = true;
                       retryId = m_client->call(
                           QStringLiteral("retry"), QJsonValue(),
                           [&](QJsonValue, std::optional<RpcError> e) {
                               QVERIFY(!e.has_value());
                               retryAnswered = true;
                           });
                   });

    QLocalServer server2;
    QLocalSocket newClient;
    QLocalSocket* newServer = nullptr;
    makeSwapPair(&server2, &newClient, &newServer);

    m_client->setTransport(&newClient);

    QVERIFY(retried);
    QVERIFY(retryId > 0);
    QCOMPARE(m_client->pendingCount(), 1);

    // The retry frame really left on the NEW socket.
    newClient.flush();
    QVERIFY(newServer->waitForReadyRead(2000));
    const QJsonObject sent =
        QJsonDocument::fromJson(newServer->readAll().trimmed()).object();
    QCOMPARE(sent.value(QStringLiteral("method")).toString(),
             QStringLiteral("retry"));
    QCOMPARE(sent.value(QStringLiteral("id")).toInt(), retryId);

    newServer->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", retryId}, {"result", QJsonObject{}}}));
    newServer->flush();
    QTRY_VERIFY(retryAnswered);

    m_client->setTransport(nullptr);
}

// A close latches the client dead so no further call is written into a corpse.
// Binding a replacement must clear that latch: SPEC 5.6 reconnect wires a
// brand-new channel, and every call after it has to go out for real.
void TstRpcClient::rebindAfterCloseRevivesClient()
{
    makePair();

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    m_serverSide->disconnectFromServer();
    QTRY_COMPARE(closedSpy.count(), 1);

    int failed = 0;
    m_client->call(QStringLiteral("ping"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       if (err.has_value())
                           ++failed;
                   });
    QCOMPARE(failed, 1); // dead: nothing was written, the callback failed inline

    QLocalServer server2;
    QLocalSocket newClient;
    QLocalSocket* newServer = nullptr;
    makeSwapPair(&server2, &newClient, &newServer);

    QSignalSpy boundSpy(m_client, &CodeharbordClient::transportBound);
    m_client->setTransport(&newClient);
    QCOMPARE(boundSpy.count(), 1);

    bool answered = false;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            answered = true;
        });
    QCOMPARE(m_client->pendingCount(), 1);
    newServer->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    newServer->flush();
    QTRY_VERIFY(answered);

    m_client->setTransport(nullptr);
}
void TstRpcClient::closeCallbackRebindKeepsNewTransportAlive()
{
    ScriptedDevice first;
    ScriptedDevice second;
    m_client->setTransport(&first);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    QSignalSpy boundSpy(m_client, &CodeharbordClient::transportBound);
    int failures = 0;
    m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(err.has_value());
            ++failures;
            // Reconnect directly from the failed request's callback. The close
            // notification still belongs to `first`, not to `second`.
            m_client->setTransport(&second);
        });

    first.announceEof();

    QCOMPARE(failures, 1);
    QCOMPARE(closedSpy.count(), 0);
    QCOMPARE(boundSpy.count(), 1);
    QCOMPARE(m_client->transport(), static_cast<QIODevice*>(&second));

    bool answered = false;
    const qint64 id =
        m_client->call(QStringLiteral("ping"), QJsonValue(),
                       [&](QJsonValue, std::optional<RpcError> err) {
                           QVERIFY(!err.has_value());
                           answered = true;
                       });
    second.deliver(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    QVERIFY(answered);
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}


// A message carrying BOTH a method and an id is a request aimed at the client
// (JSON-RPC 2.0 section 4.1 defines a notification as a request WITHOUT an id),
// not a notification. Treating it as one would also swallow a malformed RESPONSE
// that echoed `method` back and leave its caller pending forever.
void TstRpcClient::methodWithIdIsNotANotification()
{
    makePair();

    QSignalSpy notifySpy(m_client, &CodeharbordClient::notificationReceived);
    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);

    std::optional<RpcError> got;
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            got = err;
            fired = true;
        });

    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"method", ch::rpc::kWatchEventNotification},
                                  {"params", QJsonObject{{"path", "/w"}}}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QCOMPARE(notifySpy.count(), 0);
    QVERIFY(got.has_value()); // failed, not left hanging
    QCOMPARE(got->code, -32603);
    QVERIFY(warnSpy.count() >= 1);
    QCOMPARE(m_client->pendingCount(), 0);

    // An explicit null id is still a notification: no request this client issues
    // can carry one, so there is nothing it could be a response to.
    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", QJsonValue(QJsonValue::Null)},
                                  {"method", ch::rpc::kWatchEventNotification},
                                  {"params", QJsonObject{{"path", "/w"}}}}));
    m_serverSide->flush();
    QTRY_COMPARE(notifySpy.count(), 1);
}

// The sibling of the case above, and the one it does not reach: a message
// carrying an id, a method AND a result. JSON-RPC 2.0 section 5 says a response
// object has jsonrpc/id and exactly one of result/error — never `method` — so
// this is either a REQUEST aimed at the client or a corrupted response, and its
// `result` is not an answer to anything. Handing it to the caller as a success
// would resolve a pending request with a payload from a message that was never
// a reply to it.
void TstRpcClient::responseCarryingMethodIsNeverAResult()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    QSignalSpy notifySpy(m_client, &CodeharbordClient::notificationReceived);

    QJsonValue res(QJsonValue::Undefined);
    std::optional<RpcError> got;
    bool fired = false;
    const qint64 id = m_client->call(
        QStringLiteral("file.stat"), QJsonObject{{"path", "/a"}},
        [&](QJsonValue r, std::optional<RpcError> err) {
            res = r;
            got = err;
            fired = true;
        });

    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"method", ch::rpc::kWatchEventNotification},
                                  {"result", QJsonObject{{"size", 7}}}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value()); // failed, NOT resolved with the bogus result
    QCOMPARE(got->code, -32603);
    QVERIFY(res.isNull());
    QCOMPARE(notifySpy.count(), 0); // and it is not a notification either
    QVERIFY(warnSpy.count() >= 1);
    QCOMPARE(m_client->pendingCount(), 0);
}

// An error `code` that is a JSON number but not a whole value an int can hold
// is best-effort'd to 0 — which is indistinguishable from a server that sent
// {"code": 0}. The loss must be REPORTED, not silent, exactly like the
// missing-code case beside it.
void TstRpcClient::errorCodeOutOfIntRangeWarns()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    std::optional<RpcError> got;
    bool fired = false;
    const qint64 id = m_client->call(
        QStringLiteral("file.stat"), QJsonObject{{"path", "/a"}},
        [&](QJsonValue, std::optional<RpcError> err) {
            got = err;
            fired = true;
        });

    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"error", QJsonObject{{"code", 1.0e12},
                                                        {"message", "huge"}}}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value());
    QCOMPARE(got->message, QStringLiteral("huge"));
    QCOMPARE(got->code, 0); // best effort, and flagged rather than swallowed
    QTRY_COMPARE(warnSpy.count(), 1);
    QCOMPARE(m_client->pendingCount(), 0);
}

// A callback failed by a rebind's pending-sweep may rebind AGAIN — reconnecting
// is the plausible reaction to a transport error. That nested setTransport()
// runs the whole bind itself, so the outer call must notice it no longer holds
// the device it was binding and stop: otherwise it drains a transport it does
// not own and announces transportBound() a second time, making every consumer
// re-establish its server-side state (a file.watch subscription) twice over.
void TstRpcClient::rebindFromASweptCallbackAnnouncesBoundOnce()
{
    ScriptedDevice first;
    ScriptedDevice second;
    ScriptedDevice third;
    m_client->setTransport(&first);

    QSignalSpy boundSpy(m_client, &CodeharbordClient::transportBound);

    int swept = 0;
    m_client->call(QStringLiteral("workspace.list"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       QVERIFY(err.has_value());
                       ++swept;
                       m_client->setTransport(&third);
                   });
    QCOMPARE(m_client->pendingCount(), 1);

    m_client->setTransport(&second);

    QCOMPARE(swept, 1);
    // The callback's bind is the one that stands, and it was announced once.
    QVERIFY(m_client->transport() == &third);
    QCOMPARE(boundSpy.count(), 1);

    // And the surviving transport really carries traffic.
    bool answered = false;
    const qint64 id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            answered = true;
        });
    third.deliver(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    QVERIFY(answered);
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}

// A null callback is a legitimate fire-and-forget request. The response must be
// matched and dropped, and the teardown sweep must skip it, instead of throwing
// std::bad_function_call out of a Qt slot.
void TstRpcClient::nullCallbackIsTolerated()
{
    makePair();

    const int id = m_client->call(QStringLiteral("ping"), QJsonValue(), nullptr);
    QCOMPARE(m_client->pendingCount(), 1);
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    m_serverSide->flush();
    QTRY_COMPARE(m_client->pendingCount(), 0);

    // Same again, but torn down with the request still in flight.
    m_client->call(QStringLiteral("ping"), QJsonValue(), nullptr);
    QCOMPARE(m_client->pendingCount(), 1);
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    m_serverSide->disconnectFromServer();
    QTRY_COMPARE(closedSpy.count(), 1);
    QCOMPARE(m_client->pendingCount(), 0);
}

// Well-formed JSON that is not an object — a JSON-RPC 2.0 batch array, a bare
// scalar — is unroutable. It must warn without disturbing the pending call, and
// the warning must not quote a parse error that never happened ("no error").
void TstRpcClient::nonObjectJsonWarns()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError>) { fired = true; });

    m_serverSide->write(QByteArray("[{\"jsonrpc\":\"2.0\",\"id\":1}]\n"));
    m_serverSide->write(QByteArray("\"just a string\"\n"));
    m_serverSide->flush();

    QTRY_COMPARE(warnSpy.count(), 2);
    for (int i = 0; i < warnSpy.count(); ++i) {
        const QString message = warnSpy.at(i).at(0).toString();
        QVERIFY2(!message.contains(QStringLiteral("no error")),
                 qPrintable(message));
    }
    QVERIFY(!fired);
    QCOMPARE(m_client->pendingCount(), 1);
    // As above, finish the request while the callback's captured local still
    // exists; cleanup destroys the client after this test function returns.
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QCOMPARE(m_client->pendingCount(), 0);
}

// A response that physically arrived before the peer shut down must be
// DELIVERED, not reported as "transport closed with request pending". Qt may
// announce end-of-stream with complete frames still unread, so the close path
// drains what is left before failing anything.
void TstRpcClient::responseQueuedBeforeEofIsStillDelivered()
{
    ScriptedDevice device;
    m_client->setTransport(&device);

    QJsonValue res;
    std::optional<RpcError> err;
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue r, std::optional<RpcError> e) {
            res = r;
            err = e;
            fired = true;
        });

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    device.queueSilently(jsonLine({{"jsonrpc", "2.0"},
                                   {"id", id},
                                   {"result", QJsonObject{{"pong", true}}}}));
    device.announceEof(); // end-of-stream with the frame still unread

    QVERIFY(fired);
    QVERIFY(!err.has_value());
    QVERIFY(res.toObject().value(QStringLiteral("pong")).toBool());
    QCOMPARE(closedSpy.count(), 1); // announced exactly once, after the drain
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}
void TstRpcClient::closeDrainProcessesAllBoundedChunks()
{
    ScriptedDevice device;
    m_client->setTransport(&device);

    QJsonValue result;
    bool fired = false;
    const qint64 id =
        m_client->call(QStringLiteral("ping"), QJsonValue(),
                       [&](QJsonValue value, std::optional<RpcError> err) {
                           QVERIFY(!err.has_value());
                           result = value;
                           fired = true;
                       });
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    // Put one complete blank line exactly at the bounded read size before the
    // response. Closing must drain both reads instead of failing the response
    // after only the first chunk.
    QByteArray firstChunk(16 * 1024 * 1024, ' ');
    firstChunk.append('\n');
    firstChunk += jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"ok", true}}}});
    device.queueSilently(firstChunk);
    device.announceEof();

    QVERIFY(fired);
    QVERIFY(result.toObject().value(QStringLiteral("ok")).toBool());
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}


// A SHORT write leaves a truncated frame on the wire, so the peer glues the next
// request onto the fragment and the byte stream is desynchronised beyond repair.
// The client must fail this call once AND declare the transport dead, rather than
// keep emitting frames the server can only answer with parse errors.
void TstRpcClient::shortWriteFailsCallAndClosesTransport()
{
    ScriptedDevice device;
    m_client->setTransport(&device);

    int firstFailures = 0;
    m_client->call(QStringLiteral("first"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       if (err.has_value())
                           ++firstFailures;
                   });
    QCOMPARE(m_client->pendingCount(), 1);

    device.setShortWrite(true);
    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    int fired = 0;
    std::optional<RpcError> got;
    m_client->call(QStringLiteral("second"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       ++fired;
                       got = err;
                   });

    QCOMPARE(fired, 1);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32603);
    QCOMPARE(firstFailures, 1);     // the already-pending caller was failed too
    QCOMPARE(closedSpy.count(), 1); // the desynchronised stream was declared dead
    QVERIFY(warnSpy.count() >= 1);
    QCOMPARE(m_client->pendingCount(), 0);

    // Nothing more is written onto a stream that can no longer be framed.
    int afterFailures = 0;
    m_client->call(QStringLiteral("third"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       if (err.has_value())
                           ++afterFailures;
                   });
    QCOMPARE(afterFailures, 1);
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}
void TstRpcClient::synchronousResponseDuringWriteIsRouted()
{
    ScriptedDevice device;
    m_client->setTransport(&device);

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    int callbacks = 0;
    device.setWriteHook([&device](const QByteArray& bytes) {
        const QJsonObject request =
            QJsonDocument::fromJson(bytes.trimmed()).object();
        const qint64 id = request.value(QStringLiteral("id")).toInteger();
        device.deliver(
            jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", 7}}));
    });

    const qint64 id =
        m_client->call(QStringLiteral("ping"), QJsonValue(),
                       [&](QJsonValue result, std::optional<RpcError> err) {
                           QVERIFY(!err.has_value());
                           QCOMPARE(result.toInt(), 7);
                           ++callbacks;
                       });

    QVERIFY(id > 0);
    QCOMPARE(callbacks, 1);
    QCOMPARE(warnSpy.count(), 0);
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}


// transportBound() announces a NEW, usable transport and nothing else: exactly
// once per non-null bind, never for a re-bind of the same device, never for a
// detach, and late enough that a handler's very first call() goes out on the
// transport just bound.
void TstRpcClient::transportBoundFiresOnlyForNonNullBind()
{
    QSignalSpy boundSpy(m_client, &CodeharbordClient::transportBound);
    makePair();
    QCOMPARE(boundSpy.count(), 1);

    m_client->setTransport(m_clientSide); // same device: a no-op, not an event
    QCOMPARE(boundSpy.count(), 1);

    int handlerId = 0;
    connect(m_client, &CodeharbordClient::transportBound, m_client, [&] {
        handlerId = m_client->call(QStringLiteral("ping"), QJsonValue(), nullptr);
    });

    ScriptedDevice device;
    m_client->setTransport(&device);
    QCOMPARE(boundSpy.count(), 2);
    QVERIFY(handlerId > 0);
    QCOMPARE(m_client->pendingCount(), 1); // the handler's call really registered

    m_client->setTransport(nullptr);
    QCOMPARE(boundSpy.count(), 2);
    QCOMPARE(m_client->pendingCount(), 0);
}

void TstRpcClient::callWithNoTransportFailsCallbackOnce()
{
    // No transport was ever bound (init() creates a bare client). call() must
    // deliver a synthetic error exactly once, synchronously, and register
    // nothing — never orphan a callback that could not otherwise fire.
    int fired = 0;
    std::optional<RpcError> got;
    m_client->call(QStringLiteral("file.stat"), QJsonObject{{"path", "/a"}},
                   [&](QJsonValue, std::optional<RpcError> err) {
                       ++fired;
                       got = err;
                   });

    QCOMPARE(fired, 1);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32603);
    QVERIFY(got->data.isNull());
    QCOMPARE(m_client->pendingCount(), 0); // nothing registered => no leak
}

void TstRpcClient::callAfterTransportClosedFailsOnce()
{
    makePair();

    // Latch the transport closed.
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    m_serverSide->disconnectFromServer();
    QTRY_COMPARE(closedSpy.count(), 1);
    QCOMPARE(m_client->pendingCount(), 0);

    // A fresh call on a latched-closed transport must fail the callback exactly
    // once and register no pending entry.
    int fired = 0;
    std::optional<RpcError> got;
    m_client->call(QStringLiteral("ping"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       ++fired;
                       got = err;
                   });

    QCOMPARE(fired, 1);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32603);
    QCOMPARE(m_client->pendingCount(), 0);
}

void TstRpcClient::callbackDeletingClientMidDispatchIsSafe()
{
    makePair();

    // A callback that deletes the client from inside the dispatch loop must not
    // trigger a use-after-free when onReadyRead() resumes.
    bool fired = false;
    const int id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError>) {
            fired = true;
            delete m_client;   // reentrant self-delete mid-dispatch
            m_client = nullptr;
        });

    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(m_client == nullptr); // deleted cleanly; no crash / UAF
}

// Destroying the client is the last path that could orphan a pending callback:
// once it is gone nothing can answer the ids it minted. Every request in flight
// must be failed exactly once, and a retry issued from inside that teardown must
// be rejected synchronously rather than registered on a dying object.
void TstRpcClient::destroyingClientFailsPendingCallbacks()
{
    makePair();

    int firedFirst = 0;
    int firedRetry = 0;
    std::optional<RpcError> got;
    int retryId = -1;

    m_client->call(QStringLiteral("first"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       ++firedFirst;
                       got = err;
                       retryId = m_client->call(
                           QStringLiteral("retry"), QJsonValue(),
                           [&](QJsonValue, std::optional<RpcError> e) {
                               // Nobody is left to answer, so the retry fails
                               // immediately instead of being queued.
                               QVERIFY(e.has_value());
                               ++firedRetry;
                           });
                   });
    // A fire-and-forget request (null callback) must be swept without tripping
    // over the empty std::function.
    m_client->call(QStringLiteral("second"), QJsonValue(), nullptr);
    QCOMPARE(m_client->pendingCount(), 2);

    delete m_client;
    m_client = nullptr;

    QCOMPARE(firedFirst, 1);
    QCOMPARE(firedRetry, 1);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32603);
    QCOMPARE(got->message,
             QStringLiteral("client destroyed with request pending"));
    QVERIFY(retryId > 0);
}

// The other thing a callback failed by the destructor might do: react to the
// transport error by driving a RECONNECT. setTransport() exists to clear the
// close latch, so without a one-way teardown latch that call would revive a
// client already inside ~CodeharbordClient — wiring signals into a
// half-destroyed QObject and re-opening a pending map nothing will ever
// service. Teardown must be final, and every request issued during it must
// still be failed exactly once.
void TstRpcClient::reconnectFromDestructorSweepIsRefused()
{
    makePair();

    ScriptedDevice replacement;
    int firedFirst = 0;
    int firedAfterRebind = 0;
    int boundAfterDelete = 0;
    connect(m_client, &CodeharbordClient::transportBound, m_client,
            [&] { ++boundAfterDelete; });

    m_client->call(
        QStringLiteral("first"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(err.has_value());
            ++firedFirst;
            // The reconnect a consumer would plausibly attempt here.
            m_client->setTransport(&replacement);
            QVERIFY(m_client->transport() != &replacement); // refused
            // And a request issued after that refusal is still failed once,
            // never queued onto the dying client.
            m_client->call(QStringLiteral("afterRebind"), QJsonValue(),
                           [&](QJsonValue, std::optional<RpcError> e) {
                               QVERIFY(e.has_value());
                               QCOMPARE(e->code, -32603);
                               ++firedAfterRebind;
                           });
        });
    QCOMPARE(m_client->pendingCount(), 1);

    delete m_client;
    m_client = nullptr;

    QCOMPARE(firedFirst, 1);
    QCOMPARE(firedAfterRebind, 1);
    QCOMPARE(boundAfterDelete, 0); // nothing was ever bound during teardown
}

void TstRpcClient::oversizedNewlinelessInputIsBounded()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    std::optional<RpcError> got;
    m_client->call(QStringLiteral("ping"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) { got = err; });

    // Stream >16 MiB with no newline: a malformed unframed line. The client must
    // bound its read buffer, warn, and tear the transport down (failing pending
    // callers) rather than grow memory without limit.
    const QByteArray blob(17 * 1024 * 1024, 'x');
    m_serverSide->write(blob);
    m_serverSide->flush();

    QTRY_VERIFY_WITH_TIMEOUT(closedSpy.count() >= 1, 10000);
    QVERIFY(warnSpy.count() >= 1);
    QVERIFY(got.has_value()); // the pending call was failed on the reset
    QCOMPARE(got->code, -32603);
    QCOMPARE(m_client->pendingCount(), 0);
}
void TstRpcClient::oversizedDelimitedInputIsRejected()
{
    ScriptedDevice device;
    m_client->setTransport(&device);

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    std::optional<RpcError> got;
    m_client->call(QStringLiteral("ping"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) { got = err; });

    // The newline is present, but the frame itself is over the 16 MiB cap. It
    // must be rejected before JSON parsing rather than accepted just because
    // the peer remembered to terminate it.
    QByteArray oversized(16 * 1024 * 1024 + 1, 'x');
    oversized.append('\n');
    device.deliver(oversized);

    QCOMPARE(closedSpy.count(), 1);
    QVERIFY(warnSpy.count() >= 1);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32603);
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}


// One readyRead() can hand over several frames at once, and a callback is free
// to tear the session down: SessionBootstrap really does close the RPC channel
// from inside a response callback. Everything after that point in the chunk
// belongs to a connection that no longer exists — dispatching it would emit a
// notification AFTER transportClosed() and warn about ids the close just swept.
void TstRpcClient::closeMidChunkDropsTheRestOfTheChunk()
{
    ScriptedDevice device;
    m_client->setTransport(&device);

    QSignalSpy notifySpy(m_client, &CodeharbordClient::notificationReceived);
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    bool fired = false;
    const qint64 id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            fired = true;
            device.announceEof(); // the peer goes away from inside the callback
        });

    QByteArray chunk;
    chunk += jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}});
    chunk += jsonLine({{"jsonrpc", "2.0"},
                       {"method", ch::rpc::kWatchEventNotification},
                       {"params", QJsonObject{{"path", "/w"}}}});
    device.deliver(chunk);

    QVERIFY(fired);
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(notifySpy.count(), 0);
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}

// The shape EVERY void handler answers with: remote/src/codeharbord.ts turns a
// handler that returns undefined into {"result": null}, so workspace.deleteGroup
// and friends are answered exactly like this. A null result is a SUCCESS with a
// null value, not a missing member — getting that wrong would report every
// mutation as a malformed response.
void TstRpcClient::nullResultIsSuccess()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    QJsonValue res(QJsonValue::Undefined);
    std::optional<RpcError> got;
    bool fired = false;
    const qint64 id = m_client->call(
        QStringLiteral("workspace.deleteGroup"), QJsonObject{{"id", "g1"}},
        [&](QJsonValue r, std::optional<RpcError> err) {
            res = r;
            got = err;
            fired = true;
        });

    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"result", QJsonValue(QJsonValue::Null)}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(!got.has_value());
    QVERIFY(res.isNull());
    QCOMPARE(warnSpy.count(), 0);
    QCOMPARE(m_client->pendingCount(), 0);
}

// The framing case the two existing ones do not cover between them: a single
// read carrying one COMPLETE frame followed by the head of the next. The
// complete one must be dispatched immediately and the fragment held back — the
// reader tracks how far it has already scanned for a newline, and a stale scan
// offset here would skip past the newline that finishes the second frame.
void TstRpcClient::completeFrameThenPartialInOneRead()
{
    makePair();

    bool f1 = false;
    bool f2 = false;
    const qint64 id1 =
        m_client->call(QStringLiteral("a"), QJsonValue(),
                       [&](QJsonValue, std::optional<RpcError> err) {
                           QVERIFY(!err.has_value());
                           f1 = true;
                       });
    const qint64 id2 =
        m_client->call(QStringLiteral("b"), QJsonValue(),
                       [&](QJsonValue, std::optional<RpcError> err) {
                           QVERIFY(!err.has_value());
                           f2 = true;
                       });

    const QByteArray first =
        jsonLine({{"jsonrpc", "2.0"}, {"id", id1}, {"result", QJsonObject{}}});
    const QByteArray second = jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", id2},
         {"result", QJsonObject{{"content", QString(4096, QChar('y'))}}}});
    const qsizetype cut = second.size() / 2;

    m_serverSide->write(first + second.left(cut));
    m_serverSide->flush();
    QTRY_VERIFY(f1);
    QTest::qWait(100);
    QVERIFY(!f2);
    QCOMPARE(m_client->pendingCount(), 1);

    m_serverSide->write(second.mid(cut));
    m_serverSide->flush();
    QTRY_VERIFY(f2);
    QCOMPARE(m_client->pendingCount(), 0);
}

// A JSON number too large for a 64-bit integer is not an id this client can
// have issued. It must warn and leave the pending call alone rather than clamp
// or wrap into some other caller's id.
void TstRpcClient::outOfRangeIdWarns()
{
    makePair();

    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    bool fired = false;
    const qint64 id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError>) { fired = true; });

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", 1.0e19}, {"result", QJsonObject{}}}));
    m_serverSide->flush();

    QTRY_COMPARE(warnSpy.count(), 1);
    QVERIFY(!fired);
    QCOMPARE(m_client->pendingCount(), 1);

    // Finish the real request while this test's stack locals still exist.
    m_serverSide->write(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QCOMPARE(m_client->pendingCount(), 0);
}

// `params` is optional on a JSON-RPC notification. Omitting it must still
// dispatch the notification (with an undefined params value) instead of being
// dropped as unroutable.
void TstRpcClient::notificationWithoutParamsRouted()
{
    makePair();

    QSignalSpy notifySpy(m_client, &CodeharbordClient::notificationReceived);
    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"method", ch::rpc::kWatchEventNotification}}));
    m_serverSide->flush();

    QTRY_COMPARE(notifySpy.count(), 1);
    QCOMPARE(notifySpy.at(0).at(0).toString(),
             QString::fromLatin1(ch::rpc::kWatchEventNotification));
    QVERIFY(notifySpy.at(0).at(1).value<QJsonValue>().isUndefined());
    QCOMPARE(warnSpy.count(), 0);
}

// `data` is optional on a JSON-RPC error object. When it is absent the caller
// gets an UNDEFINED value (not a null one), which the documented way of reading
// it — toObject()/toString() — folds to the same empty result either way.
void TstRpcClient::errorWithoutDataYieldsUndefinedData()
{
    makePair();

    std::optional<RpcError> got;
    bool fired = false;
    const qint64 id = m_client->call(
        QStringLiteral("file.stat"), QJsonObject{{"path", "/a"}},
        [&](QJsonValue, std::optional<RpcError> err) {
            got = err;
            fired = true;
        });

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", id},
         {"error", QJsonObject{{"code", -32602}, {"message", "Invalid params"}}}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32602);
    QCOMPARE(got->message, QStringLiteral("Invalid params"));
    QVERIFY(got->data.isUndefined());
    QVERIFY(got->data.toObject().isEmpty());
}

// The transport is owned by the CALLER, which may simply delete it — a
// stack-allocated device going out of scope is enough. QIODevice emits no
// readChannelFinished() from its destructor, so without watching destroyed()
// every pending callback would sit in the map until the client itself died:
// the caller never learns its request failed. Rebinding afterwards must also be
// safe, i.e. must not disconnect() through a freed pointer.
void TstRpcClient::transportDestroyedWhileBoundFailsPending()
{
    auto* device = new ScriptedDevice;
    m_client->setTransport(device);

    int fired = 0;
    std::optional<RpcError> got;
    m_client->call(QStringLiteral("ping"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       ++fired;
                       got = err;
                   });
    QCOMPARE(m_client->pendingCount(), 1);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    delete device; // the caller destroys the transport out from under us

    QCOMPARE(fired, 1);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32603);
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(m_client->pendingCount(), 0);
    QVERIFY(m_client->transport() == nullptr);

    // A later bind must not touch the freed device.
    ScriptedDevice second;
    m_client->setTransport(&second);
    const qint64 id = m_client->call(QStringLiteral("ping"), QJsonValue(), nullptr);
    QCOMPARE(m_client->pendingCount(), 1);
    second.deliver(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}

// A close latches the client dead. Nothing in the API forces a reconnect to
// allocate a NEW QIODevice, so a caller that reopens the same device object must
// get a working client back instead of one whose every call() fails forever.
void TstRpcClient::rebindSameDeviceAfterCloseRevivesClient()
{
    ScriptedDevice device;
    m_client->setTransport(&device);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    QSignalSpy boundSpy(m_client, &CodeharbordClient::transportBound);
    device.announceEof();
    QCOMPARE(closedSpy.count(), 1);

    int failed = 0;
    m_client->call(QStringLiteral("ping"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       if (err.has_value())
                           ++failed;
                   });
    QCOMPARE(failed, 1); // dead: nothing was written

    m_client->setTransport(&device); // the SAME device, reopened by its owner
    QCOMPARE(boundSpy.count(), 1);

    bool answered = false;
    const qint64 id = m_client->call(
        QStringLiteral("ping"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            answered = true;
        });
    QCOMPARE(m_client->pendingCount(), 1);
    device.deliver(
        jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}}));
    QVERIFY(answered);

    m_client->setTransport(nullptr);
}

void TstRpcClient::liveServerInfoOverProcess()
{
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty())
        QSKIP("node not on PATH");

    QProcess proc;
    // The process is a stack object. Detach even when a later assertion fails,
    // because QTest continues unwinding the case before cleanup() runs and the
    // stack process would otherwise leave a dangling QIODevice pointer.
    const auto detach = qScopeGuard([this] {
        if (m_client && m_client->transport())
            m_client->setTransport(nullptr);
    });
    // Point the child at a throwaway database. Without this it inherits an
    // unset CODEHARBOR_DB and codeharbord opens the DEVELOPER'S real workspace
    // database under ~/.local/share, which makes this case order-dependent,
    // lets it fail for reasons unrelated to the transport it is testing (a
    // stored schema version newer than this build's is correctly refused), and
    // means a test writes to real user data. tst_workspacedb's sibling process
    // case already isolates itself this way; the two had drifted apart.
    QTemporaryDir dbDir;
    QVERIFY(dbDir.isValid());
    QProcessEnvironment envp = QProcessEnvironment::systemEnvironment();
    envp.insert(QStringLiteral("CODEHARBOR_DB"),
                dbDir.filePath(QStringLiteral("ws.db")));
    proc.setProcessEnvironment(envp);
    proc.setProgram(node);
    proc.setArguments({QStringLiteral(CH_REPO_ROOT "/remote/src/codeharbord.ts"),
                       QStringLiteral("rpc"), QStringLiteral("--stdio")});
    proc.start();
    if (!proc.waitForStarted(3000))
        QSKIP("failed to start node codeharbord");

    m_client->setTransport(&proc);

    QJsonValue result;
    std::optional<RpcError> err;
    bool fired = false;
    m_client->call(QStringLiteral("server.info"), QJsonObject{},
                   [&](QJsonValue r, std::optional<RpcError> e) {
                       result = r;
                       err = e;
                       fired = true;
                   });

    QTRY_VERIFY_WITH_TIMEOUT(fired, 8000);
    QVERIFY(!err.has_value());
    QCOMPARE(result.toObject().value("name").toString(),
             QStringLiteral("codeharbord"));

    m_client->setTransport(nullptr);
    proc.closeWriteChannel();
    proc.terminate();
    if (!proc.waitForFinished(2000))
        proc.kill();
}

// --- transport heartbeat -----------------------------------------------------
//
// The heartbeat exists for the failure the rest of this file cannot produce: a
// transport that stays open, readable and writable while the peer behind it has
// stopped answering. There is no EOF, no disconnected(), no write error — every
// mechanism the client already had is silent — so without a heartbeat every
// pending caller waits forever, which in the app is an editor pane stuck on
// "saving…" for the rest of the session.
//
// All four use ScriptedDevice, whose writeData() now records the bytes, so the
// test can see the probes and decide whether to answer them. Intervals are
// compressed to tens of milliseconds; production runs at
// kDefaultHeartbeatIntervalMs / kDefaultHeartbeatMisses.

// A peer that answers probes is left alone forever, and the probe traffic is
// invisible: it never lands in pendingCount(), never produces a warning, and
// never disturbs an ordinary request travelling beside it.
void TstRpcClient::heartbeatAnsweredKeepsClientAliveAndOffThePendingCount()
{
    ScriptedDevice device;
    // Enabled BEFORE the bind on purpose: the order must not matter.
    m_client->enableHeartbeat(50, 4);
    m_client->setTransport(&device);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);

    // An ordinary request the peer takes its time over — the long transfer the
    // heartbeat must never kill.
    QJsonValue result;
    bool fired = false;
    const qint64 id = m_client->call(
        QStringLiteral("file.readFile"), QJsonObject{{"path", "/big"}},
        [&](QJsonValue r, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            result = r;
            fired = true;
        });
    QCOMPARE(m_client->pendingCount(), 1);

    // Ten intervals — two and a half times the whole death budget — of a peer
    // that answers nothing but the probes.
    int probes = 0;
    for (int i = 0; i < 10; ++i) {
        QTest::qWait(50);
        probes += answerPings(device);
        QCOMPARE(m_client->pendingCount(), 1); // the probe is never counted
        QVERIFY(!fired);
    }
    QVERIFY2(probes >= 5, "the heartbeat never actually probed");
    QCOMPARE(closedSpy.count(), 0);
    QCOMPARE(warnSpy.count(), 0); // probe replies are never "unknown ids"

    // And the slow request still completes normally afterwards.
    device.deliver(jsonLine({{"jsonrpc", "2.0"},
                             {"id", id},
                             {"result", QJsonObject{{"content", "B"}}}}));
    QVERIFY(fired);
    QCOMPARE(result.toObject().value("content").toString(), QStringLiteral("B"));
    QCOMPARE(m_client->pendingCount(), 0);
    QCOMPARE(closedSpy.count(), 0);

    m_client->setTransport(nullptr);
}

// The case the whole feature exists for: the transport stays perfectly healthy
// and the peer simply stops answering. Every pending callback must be failed
// EXACTLY once with the standard synthetic transport error, and transportClosed()
// must be emitted exactly once — the same observable behaviour as a real
// disconnect, which is what SessionBootstrap's reconnect ladder acts on.
void TstRpcClient::heartbeatSilentPeerFailsPendingOnceAndClosesOnce()
{
    ScriptedDevice device;
    m_client->enableHeartbeat(30, 3);
    m_client->setTransport(&device);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    int failures = 0;
    std::optional<RpcError> last;
    for (int i = 0; i < 3; ++i) {
        m_client->call(QStringLiteral("workspace.list"), QJsonValue(),
                       [&](QJsonValue, std::optional<RpcError> err) {
                           QVERIFY(err.has_value());
                           last = err;
                           ++failures;
                       });
    }
    QCOMPARE(m_client->pendingCount(), 3);

    QTRY_VERIFY_WITH_TIMEOUT(closedSpy.count() == 1, 5000);
    QCOMPARE(failures, 3);
    QVERIFY(last.has_value());
    QCOMPARE(last->code, -32603);
    QCOMPARE(last->message,
             QStringLiteral("transport closed with request pending"));
    QCOMPARE(m_client->pendingCount(), 0);

    // The probe really was sent before the client gave up on it, and the client
    // stays latched dead rather than closing again every interval.
    QVERIFY(countPings(device) >= 1);
    QTest::qWait(200);
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(failures, 3);
    QCOMPARE(countPings(device), 0); // the timer really stopped

    m_client->setTransport(nullptr);
}

// The control for the case above, and the proof that the heartbeat is what
// produces it: identical script, heartbeat never enabled. Nothing happens — the
// three callers hang, which is exactly the bug being fixed.
void TstRpcClient::silentPeerIsNotKilledWithTheHeartbeatDisabled()
{
    ScriptedDevice device;
    m_client->setTransport(&device); // no enableHeartbeat()

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    int failures = 0;
    for (int i = 0; i < 3; ++i) {
        m_client->call(QStringLiteral("workspace.list"), QJsonValue(),
                       [&](QJsonValue, std::optional<RpcError>) { ++failures; });
    }

    // Longer than the 90 ms budget the previous test dies inside of.
    QTest::qWait(400);
    QCOMPARE(closedSpy.count(), 0);
    QCOMPARE(failures, 0);
    QCOMPARE(m_client->pendingCount(), 3);
    QCOMPARE(countPings(device), 0); // and no probe was ever written

    m_client->setTransport(nullptr);
    QCOMPARE(failures, 3); // detach is still what rescues them today
}

// A peer midway through writing one huge frame cannot answer a probe: this is a
// single serialized JSONL stream, so its reply physically cannot be interleaved
// into the frame it is still emitting. Bytes — any bytes — are therefore the
// liveness evidence, not probe replies. Without that rule the heartbeat would
// tear down precisely the slow multi-megabyte file.readFile the no-timeout
// design exists to protect.
void TstRpcClient::heartbeatSparesASlowButLivePeer()
{
    ScriptedDevice device;
    m_client->enableHeartbeat(30, 3);
    m_client->setTransport(&device);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    QJsonValue result;
    bool fired = false;
    const qint64 id = m_client->call(
        QStringLiteral("file.readFile"), QJsonObject{{"path", "/big"}},
        [&](QJsonValue r, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            result = r;
            fired = true;
        });

    // Dribble one frame out in newline-less pieces, far past the 90 ms budget.
    // No probe is ever answered, no complete message is ever routed.
    const QByteArray frame = jsonLine({{"jsonrpc", "2.0"},
                                       {"id", id},
                                       {"result", QJsonObject{{"content", "B"}}}});
    for (qsizetype i = 0; i < frame.size() - 1; ++i) {
        device.deliver(frame.mid(i, 1));
        QTest::qWait(20);
        QVERIFY2(closedSpy.count() == 0,
                 "a peer that is still sending bytes was declared dead");
        QVERIFY(!fired);
    }

    // The final newline completes it and the caller is answered normally.
    device.deliver(frame.right(1));
    QVERIFY(fired);
    QCOMPARE(result.toObject().value("content").toString(), QStringLiteral("B"));
    QCOMPARE(closedSpy.count(), 0);

    m_client->setTransport(nullptr);
}

// Unbinding must stop the timer. The device is a stack object the caller is
// free to destroy the moment it is detached, so a tick that survived the unbind
// would probe through a dangling pointer — and one that merely "closed" an
// already-detached client would emit a disconnect the consumer never had.
void TstRpcClient::heartbeatStopsWhenTransportUnbound()
{
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    // A deliberately huge miss tolerance: this case is about the TIMER'S
    // LIFETIME, not about detection, and a peer that answers nothing must not
    // die mid-test and mask the thing being asserted.
    {
        ScriptedDevice device;
        m_client->enableHeartbeat(20, 500);
        m_client->setTransport(&device);
        QVERIFY2(awaitPing(device, 5000) != 0,
                 "the heartbeat never armed");

        // Detach, then watch the device that is STILL ALIVE but no longer bound.
        // Not one further probe may be written to it.
        m_client->setTransport(nullptr);
        QTest::qWait(200); // ten intervals
        QCOMPARE(countPings(device), 0);
    } // device destroyed while the client lives on

    // More intervals still, now with the device freed: a tick that had survived
    // the unbind would dereference a dangling pointer here.
    QTest::qWait(200);
    QCOMPARE(closedSpy.count(), 0);
    QCOMPARE(m_client->pendingCount(), 0);

    // Rebinding re-arms it against the NEW transport — the reconnect case.
    ScriptedDevice revived;
    m_client->setTransport(&revived);
    bool probed = false;
    for (int i = 0; i < 20 && !probed; ++i) {
        QTest::qWait(20);
        probed = answerPings(revived) >= 1;
    }
    QVERIFY2(probed, "the heartbeat did not re-arm on the new transport");
    QCOMPARE(closedSpy.count(), 0);

    m_client->setTransport(nullptr);
}
void TstRpcClient::heartbeatWriteFailureDeletingClientIsSafe()
{
    ScriptedDevice device;
    m_client->enableHeartbeat(10, 1000);
    m_client->setTransport(&device);
    device.setShortWrite(true);

    bool warned = false;
    connect(m_client, &CodeharbordClient::protocolWarning, m_client,
            [this, &warned](const QString& message) {
                if (!message.contains(QStringLiteral("write failed")))
                    return;
                warned = true;
                delete m_client;
                m_client = nullptr;
            });

    QTRY_VERIFY_WITH_TIMEOUT(warned, 1000);
    QVERIFY(m_client == nullptr);
}


// A probe's callback can run when that probe is no longer the one being awaited,
// and it must then do NOTHING. Two routes reach that state; this is the one that
// can be driven deterministically.
//
// Re-arming the heartbeat on a LIVE transport retires the probe in flight — it
// stays in the pending map and stays on the wire, because nothing swept it — and
// the next tick issues its successor. When the peer finally answers the RETIRED
// probe, a callback that cannot tell the two apart clears the live one. The
// damage: the one-probe-in-flight invariant breaks (the next tick sends a third
// probe while the second is still outstanding), pendingCount() stops excluding
// the right entry, and the miss counter starts measuring the wrong silence —
// which can produce either a spurious teardown or a missed one.
//
// The other route is a rebind whose pending-sweep callbacks spin a nested event
// loop; the case below covers that shape, and the same generation stamp fixes
// both.
void TstRpcClient::retiredProbeAnswerDoesNotClobberTheLiveProbe()
{
    ScriptedDevice device;
    // A huge miss tolerance: this case is about the bookkeeping, and the peer
    // deliberately leaves probes unanswered for many intervals.
    m_client->enableHeartbeat(20, 1000);
    m_client->setTransport(&device);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);
    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);

    // One ordinary caller, so pendingCount() has something real to report.
    bool callerFired = false;
    const qint64 callerId =
        m_client->call(QStringLiteral("workspace.list"), QJsonValue(),
                       [&](QJsonValue, std::optional<RpcError> err) {
                           QVERIFY(!err.has_value());
                           callerFired = true;
                       });

    const qint64 retired = awaitPing(device);
    QVERIFY2(retired != 0, "the heartbeat never armed");
    QCOMPARE(m_client->pendingCount(), 1);

    // Re-arm on the live transport. Re-arming needs a CHANGED configuration —
    // re-enabling the one already running is a deliberate no-op, so that a
    // consumer re-enabling on a loop cannot strand a probe per call — and this
    // is what a real re-arm costs: `retired` is now abandoned but still pending
    // and still on the wire, so it must keep being excluded from pendingCount().
    m_client->enableHeartbeat(20, 999);
    QCOMPARE(m_client->pendingCount(), 1);

    const qint64 live = awaitPing(device);
    QVERIFY2(live != 0, "the re-arm did not issue a successor probe");
    QVERIFY(live != retired);
    QCOMPARE(m_client->pendingCount(), 1); // both probes excluded

    // The peer answers the RETIRED probe, late. `live` is still unanswered.
    answerPing(device, retired);
    QCOMPARE(m_client->pendingCount(), 1);
    QCOMPARE(warnSpy.count(), 0); // routed normally, never an "unknown id"

    // THE ASSERTION. `live` is still outstanding, so the client must keep
    // waiting on it. A third probe here would mean the retired answer was taken
    // as an answer for `live`.
    for (int i = 0; i < 8; ++i) {
        QTest::qWait(20);
        QCOMPARE(countPings(device), 0);
    }
    QCOMPARE(closedSpy.count(), 0);

    // And the heartbeat is not wedged: answering the LIVE probe does release the
    // next one.
    answerPing(device, live);
    QVERIFY2(awaitPing(device) != 0, "the heartbeat stopped probing");

    // The ordinary caller was untouched throughout.
    QVERIFY(!callerFired);
    device.deliver(jsonLine({{"jsonrpc", "2.0"}, {"id", callerId}, {"result", 1}}));
    QVERIFY(callerFired);
    QCOMPARE(m_client->pendingCount(), 0);

    m_client->setTransport(nullptr);
}

// The other route to the same hole, and a reentrancy-safety case in its own
// right. setTransport() retires the old probe, then sweeps the old pending map;
// any callback in that sweep may spin a nested event loop, in which the timer
// fires and puts the successor probe on the NEW transport — so the sweep can
// reach the retired probe's callback after its successor is already live.
//
// Honest about its limits: failAllPending() iterates a QHash, so whether the
// nested loop runs BEFORE or AFTER the retired probe's callback is not something
// a test can pin down. This case therefore does not reliably TRIGGER the hole
// (the case above does, deterministically) — what it reliably asserts is that
// rebinding from inside a swept callback, with the timer firing underneath,
// leaves the probe bookkeeping intact and touches nothing freed.
void TstRpcClient::rebindFromInsideACallbackKeepsProbeBookkeepingStraight()
{
    ScriptedDevice first;
    ScriptedDevice second;
    m_client->enableHeartbeat(20, 1000);
    m_client->setTransport(&first);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    QVERIFY2(awaitPing(first) != 0, "the heartbeat never armed");

    // A caller whose failure callback rebinds and then lets the event loop run,
    // so the heartbeat can probe the new transport mid-sweep.
    int swept = 0;
    m_client->call(QStringLiteral("workspace.list"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       QVERIFY(err.has_value());
                       ++swept;
                       QTest::qWait(60); // three intervals, nested
                   });
    QCOMPARE(m_client->pendingCount(), 1);

    // The rebind. Its sweep fails both the caller above and the probe on
    // `first`, in an order the QHash chooses.
    m_client->setTransport(&second);
    QCOMPARE(swept, 1);
    QCOMPARE(m_client->pendingCount(), 0);

    // Exactly ONE probe may ever be in flight on the new transport. Nothing
    // answers it, so across many intervals the client must send it once and then
    // wait — a second probe would mean a retired callback cleared the live one.
    int probes = countPings(second);
    for (int i = 0; i < 10; ++i) {
        QTest::qWait(20);
        probes += countPings(second);
    }
    QCOMPARE(probes, 1);
    QCOMPARE(m_client->pendingCount(), 0); // the probe is still excluded
    QCOMPARE(closedSpy.count(), 0);

    m_client->setTransport(nullptr);
}

// --- read-buffer cursor ------------------------------------------------------
//
// onReadyRead() consumes frames through a cursor kept in members and compacts
// the buffer ONCE per read instead of once per frame. The per-frame version was
// quadratic in frames per read; the naive deferred version — a consume offset
// local to onReadyRead(), or a reference into the buffer held across a dispatch
// — is unsafe, because a response callback may re-enter the reader. These two
// cases pin both halves.

// THE RE-ENTRANCY CASE. One read delivers three responses. The first one's
// callback issues a fourth request and hands the peer's answer over
// synchronously, so a nested onReadyRead() runs while the outer loop still has
// two frames of its own chunk unconsumed — and it runs against a buffer the
// nested readAll() has just appended to, which reallocates it.
//
// Every frame must be dispatched EXACTLY ONCE regardless of which nesting level
// reaches it. A reader that tracked consumption in a local, or held a slice of
// the buffer across the dispatch, re-processes the frames the outer loop had
// already logically consumed: those ids are gone from the pending map, so each
// one becomes an "unknown id" protocol warning, which is what warnSpy pins.
void TstRpcClient::reentrantFeedFromACallbackDispatchesEveryFrameOnce()
{
    ScriptedDevice device;
    m_client->setTransport(&device);
    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);

    QList<QString> order;
    qint64 nestedId = 0;
    const qint64 a = m_client->call(
        QStringLiteral("workspace.list"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            order.append(QStringLiteral("a"));
            // Re-enter the reader from inside the reader. deliver() announces
            // synchronously, so this runs a whole nested onReadyRead() before
            // the outer loop has looked at frames b and c.
            nestedId = m_client->call(
                QStringLiteral("file.readFile"), QJsonValue(),
                [&](QJsonValue, std::optional<RpcError> nestedErr) {
                    QVERIFY(!nestedErr.has_value());
                    order.append(QStringLiteral("d"));
                });
            device.deliver(jsonLine(
                {{"jsonrpc", "2.0"}, {"id", nestedId}, {"result", QJsonObject{}}}));
        });
    const qint64 b = m_client->call(
        QStringLiteral("workspace.list"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            order.append(QStringLiteral("b"));
        });
    const qint64 c = m_client->call(
        QStringLiteral("workspace.list"), QJsonValue(),
        [&](QJsonValue, std::optional<RpcError> err) {
            QVERIFY(!err.has_value());
            order.append(QStringLiteral("c"));
        });
    QCOMPARE(m_client->pendingCount(), 3);

    // One chunk, three frames, one readyRead.
    device.deliver(
        jsonLine({{"jsonrpc", "2.0"}, {"id", a}, {"result", QJsonObject{}}}) +
        jsonLine({{"jsonrpc", "2.0"}, {"id", b}, {"result", QJsonObject{}}}) +
        jsonLine({{"jsonrpc", "2.0"}, {"id", c}, {"result", QJsonObject{}}}));

    // Exactly once each, and nothing was re-dispatched: four callbacks, four
    // distinct labels, no unknown-id warnings.
    QCOMPARE(order.size(), 4);
    QCOMPARE(order.count(QStringLiteral("a")), 1);
    QCOMPARE(order.count(QStringLiteral("b")), 1);
    QCOMPARE(order.count(QStringLiteral("c")), 1);
    QCOMPARE(order.count(QStringLiteral("d")), 1);
    QCOMPARE(warnSpy.count(), 0);
    QCOMPARE(m_client->pendingCount(), 0);
    QVERIFY(nestedId > c);

    m_client->setTransport(nullptr);
}

// THE BULK CASE. A watch-event burst really does arrive as many frames in one
// read, and deferring the compaction means the loop runs a long way with a
// growing consumed prefix still physically in the buffer. This drives that path
// with a chunk two orders of magnitude bigger than any hand-written case here:
// every response must still be routed exactly once, and the buffer must be
// empty at the end.
//
// Deliberately NOT a timing assertion. The recorded claim was that the old
// per-frame remove() was quadratic; it is not. Qt 6's QArrayDataPointer::erase
// has a front-erase fast path that only advances the data pointer, and
// QByteArray::left() deep-copies rather than sharing, so nothing forced a
// detach. Measured directly (80 000 frames, no JSON): 5 ms for the old
// discipline against 2 ms for this one — a constant factor, both linear. A
// wall-clock bound that the old code also passes is a flake, not a test.
void TstRpcClient::aBurstOfFramesInOneReadIsRoutedOnce()
{
    ScriptedDevice device;
    m_client->setTransport(&device);
    QSignalSpy warnSpy(m_client, &CodeharbordClient::protocolWarning);

    constexpr int kFrames = 20000;
    int answered = 0;
    QByteArray chunk;
    for (int i = 0; i < kFrames; ++i) {
        const qint64 id =
            m_client->call(QStringLiteral("file.watch"), QJsonValue(),
                           [&](QJsonValue, std::optional<RpcError> err) {
                               QVERIFY(!err.has_value());
                               ++answered;
                           });
        chunk += jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", i}});
    }
    QCOMPARE(m_client->pendingCount(), qsizetype(kFrames));

    device.deliver(chunk); // one read, kFrames frames

    QCOMPARE(answered, kFrames);
    QCOMPARE(m_client->pendingCount(), qsizetype(0));
    QCOMPARE(warnSpy.count(), 0);

    // The deferred compaction really ran: a partial frame arriving now must be
    // held on its own, not spliced onto 1.2 MB of already-dispatched bytes.
    const qint64 tail =
        m_client->call(QStringLiteral("file.watch"), QJsonValue(), nullptr);
    device.deliver(QByteArray("{\"jsonrpc\":\"2.0\",\"id\":") +
                   QByteArray::number(tail));
    QCOMPARE(m_client->pendingCount(), qsizetype(1));
    device.deliver(QByteArray(",\"result\":1}\n"));
    QCOMPARE(m_client->pendingCount(), qsizetype(0));
    QCOMPARE(warnSpy.count(), 0);

    m_client->setTransport(nullptr);
}

// pendingCount() reports a container size, which is 64-bit. Returning `int`
// narrowed it silently. There is no way to build two billion pending entries in
// a test, so the contract is pinned where it actually lives: in the type.
void TstRpcClient::pendingCountIsNotNarrowedToInt()
{
    static_assert(
        std::is_same_v<decltype(std::declval<const CodeharbordClient&>()
                                    .pendingCount()),
                       qsizetype>,
        "pendingCount() must not narrow QHash::size() to int");

    ScriptedDevice device;
    m_client->setTransport(&device);
    const qint64 id =
        m_client->call(QStringLiteral("workspace.list"), QJsonValue(), nullptr);
    const qsizetype pending = m_client->pendingCount();
    QCOMPARE(pending, qsizetype(1));
    device.deliver(jsonLine({{"jsonrpc", "2.0"}, {"id", id}, {"result", 1}}));
    QCOMPARE(m_client->pendingCount(), qsizetype(0));

    m_client->setTransport(nullptr);
}

// --- heartbeat: bounded probe bookkeeping ------------------------------------
//
// A re-arm is not free: it retires the probe in flight, which stays in
// m_pending and in the probe set until the peer answers it or the transport
// dies, and it zeroes the miss counter. Re-enabling the configuration ALREADY
// running must therefore do nothing at all.

// Re-enabling the running configuration issues no new probe and abandons no old
// one. Before the fix each call retired the live probe and the next tick minted
// a successor, so this loop left eight stranded entries behind.
void TstRpcClient::reEnablingTheRunningHeartbeatIsANoOp()
{
    ScriptedDevice device;
    m_client->enableHeartbeat(20, 1000); // huge tolerance: no teardown here
    m_client->setTransport(&device);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    const qint64 probe = awaitPing(device);
    QVERIFY2(probe != 0, "the heartbeat never armed");

    for (int i = 0; i < 8; ++i) {
        m_client->enableHeartbeat(20, 1000);
        // Longer than the interval on purpose: a re-arm restarts the timer, so
        // a wait of exactly one interval could race past the successor probe
        // the old code issued and let this loop pass against it.
        QTest::qWait(40);
        QCOMPARE(countPings(device), 0);
    }
    QCOMPARE(closedSpy.count(), 0);

    // And the ORIGINAL probe is still the live one, not a retired ghost:
    // answering it releases the next.
    answerPing(device, probe);
    QVERIFY2(awaitPing(device) != 0, "the heartbeat stopped probing");

    m_client->setTransport(nullptr);
}

// The purpose the bound has to preserve. A consumer that re-enables the
// heartbeat on a loop — on every reconnect attempt, say — used to reset the
// silence measurement AND restart the interval on every call, so against a peer
// that never answers the timer never even reached its first tick and the
// transport was never declared dead. The heartbeat existed and detected nothing.
void TstRpcClient::repeatedlyReEnablingCannotSuppressSilenceDetection()
{
    ScriptedDevice device;
    m_client->enableHeartbeat(20, 3); // ~60 ms of silence is fatal
    m_client->setTransport(&device);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    bool failed = false;
    m_client->call(QStringLiteral("workspace.list"), QJsonValue(),
                   [&](QJsonValue, std::optional<RpcError> err) {
                       QVERIFY(err.has_value());
                       failed = true;
                   });

    // Re-enable faster than the interval, for far longer than the whole
    // detection budget. The peer answers nothing.
    for (int i = 0; i < 60 && closedSpy.isEmpty(); ++i) {
        m_client->enableHeartbeat(20, 3);
        QTest::qWait(10);
    }

    QCOMPARE(closedSpy.count(), 1);
    QVERIFY(failed);
    QCOMPARE(m_client->pendingCount(), 0);
}

// And with a genuinely CHANGED configuration each time — which really must
// re-arm — the probes left stranded on the wire are capped instead of growing
// once per call. Nothing answers, so every probe issued here is still pending;
// counting the ones written is counting the collections' growth directly.
void TstRpcClient::reArmingAgainstASilentPeerCannotPileUpProbes()
{
    ScriptedDevice device;
    m_client->enableHeartbeat(15, 1000); // huge tolerance: no teardown here
    m_client->setTransport(&device);

    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    int probes = 0;
    for (int i = 0; i < 12; ++i) {
        m_client->enableHeartbeat(15, 1000 + i); // a real re-arm every time
        QTest::qWait(45);
        probes += countPings(device);
    }

    QVERIFY2(probes <= 4,
             qPrintable(QStringLiteral("12 re-arms against a silent peer left "
                                       "%1 probes stranded in flight")
                            .arg(probes)));
    QVERIFY2(probes >= 1, "the heartbeat never armed at all");
    // Caller traffic is still reported correctly: the stranded probes are all
    // excluded, and the count cannot go negative.
    QCOMPARE(m_client->pendingCount(), 0);
    QCOMPARE(closedSpy.count(), 0);

    m_client->setTransport(nullptr);
}
void TstRpcClient::outgoingOversizedFrameIsRejectedWithoutClosingTransport()
{
    ScriptedDevice device;
    m_client->setTransport(&device);
    QSignalSpy warningSpy(m_client, &CodeharbordClient::protocolWarning);
    QSignalSpy closedSpy(m_client, &CodeharbordClient::transportClosed);

    // JSON escapes each U+0001 as six ASCII bytes. The resulting request is
    // over the shared 16 MiB frame cap even though the input is valid UTF-8.
    const QString controlText(3 * 1024 * 1024, QChar(0x0001));
    bool called = false;
    std::optional<RpcError> observedError;
    m_client->call(QStringLiteral("file.writeFile"),
                   QJsonObject{{"path", "/large"},
                               {"content", controlText},
                               {"expectedRevision", "r"}},
                   [&](QJsonValue, std::optional<RpcError> error) {
                       called = true;
                       observedError = std::move(error);
                   });

    QVERIFY(called);
    QVERIFY(observedError.has_value());
    QCOMPARE(observedError->code, -32603);
    QCOMPARE(m_client->pendingCount(), 0);
    QVERIFY(!device.takeWritten().contains('\n'));
    QCOMPARE(warningSpy.count(), 1);
    QCOMPARE(closedSpy.count(), 0);

    // The transport remains usable after rejecting the oversized request.
    bool smallCalled = false;
    const qint64 id = m_client->call(
        QStringLiteral("ping"), QJsonObject{},
        [&](QJsonValue, std::optional<RpcError> error) {
            smallCalled = true;
            QVERIFY(!error.has_value());
        });
    QVERIFY(id > 0);
    QCOMPARE(m_client->pendingCount(), 1);
    const QJsonObject request =
        QJsonDocument::fromJson(device.takeWritten()).object();
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QStringLiteral("ping"));
    device.deliver(jsonLine({{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", QJsonObject{}}}));
    QVERIFY(smallCalled);
}

void TstRpcClient::decodeFileContentValidatesEncoding()
{
    const auto utf8 =
        ch::rpc::decodeFileContent(QJsonObject{{"encoding", "utf-8"},
                                               {"content", "hello"}});
    QVERIFY(utf8.has_value());
    QCOMPARE(*utf8, QStringLiteral("hello"));

    const auto base64 =
        ch::rpc::decodeFileContent(QJsonObject{{"encoding", "base64"},
                                               {"content", "aGVsbG8="}});
    QVERIFY(base64.has_value());
    QCOMPARE(*base64, QStringLiteral("hello"));

    QVERIFY(!ch::rpc::decodeFileContent(
                 QJsonObject{{"encoding", "latin-1"}, {"content", "hello"}})
                 .has_value());
    QVERIFY(!ch::rpc::decodeFileContent(
                 QJsonObject{{"content", "hello"}})
                 .has_value());
    QVERIFY(!ch::rpc::decodeFileContent(
                 QJsonObject{{"encoding", "utf-8"}, {"content", 7}})
                 .has_value());
}


QTEST_GUILESS_MAIN(TstRpcClient)
#include "tst_rpcclient.moc"
