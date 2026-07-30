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
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QtTest/QtTest>

#include <cstring>
#include <optional>

using ch::CodeharbordClient;
using ch::RpcError;

namespace {

// Serialize a JSON-RPC object as one framed (newline-terminated) wire line.
QByteArray jsonLine(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

// An in-process QIODevice the test drives byte for byte. Two things a real
// socket cannot be made to do on demand live here:
//  * queueSilently() followed by announceEof() delivers an end-of-stream while a
//    COMPLETE frame is still sitting unread. Qt is free to pick that ordering on
//    a real socket, and it is the ordering that used to lose the response.
//  * setShortWrite() makes write() report fewer bytes than it was handed — the
//    truncated-frame case ch::SshChannelDevice::writeData() really can produce.
// No Q_OBJECT: only inherited QIODevice signals are emitted, so no moc is needed.
class ScriptedDevice : public QIODevice {
public:
    ScriptedDevice() { open(QIODevice::ReadWrite); }

    // Make bytes readable WITHOUT emitting readyRead().
    void queueSilently(const QByteArray& bytes) { m_in += bytes; }
    void announceEof() { emit readChannelFinished(); }
    void setShortWrite(bool on) { m_shortWrite = on; }

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
    qint64 writeData(const char*, qint64 maxSize) override
    {
        return m_shortWrite ? maxSize / 2 : maxSize;
    }

private:
    QByteArray m_in;
    bool m_shortWrite = false;
};

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
    void destroyingClientFailsPendingCallbacks();
    void oversizedNewlinelessInputIsBounded();
    void detachingTransportFailsPendingOnce();
    void retryFromRebindFailureUsesNewTransport();
    void rebindAfterCloseRevivesClient();
    void methodWithIdIsNotANotification();
    void nullCallbackIsTolerated();
    void nonObjectJsonWarns();
    void responseQueuedBeforeEofIsStillDelivered();
    void shortWriteFailsCallAndClosesTransport();
    void transportBoundFiresOnlyForNonNullBind();
    void liveServerInfoOverProcess();

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
    m_client->call(QStringLiteral("ping"), QJsonValue(),
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

void TstRpcClient::liveServerInfoOverProcess()
{
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty())
        QSKIP("node not on PATH");

    QProcess proc;
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

QTEST_GUILESS_MAIN(TstRpcClient)
#include "tst_rpcclient.moc"
