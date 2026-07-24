#include "CodeharbordClient.h"
#include "RpcTypes.h"

#include <QCoreApplication>
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

#include <optional>

using ch::CodeharbordClient;
using ch::RpcError;

namespace {

// Serialize a JSON-RPC object as one framed (newline-terminated) wire line.
QByteArray jsonLine(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
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
    void liveServerInfoOverProcess();

private:
    void makePair();

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
