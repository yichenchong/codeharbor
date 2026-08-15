// ch::ViewerCommandService — the client end of the viewer control channel
// (SPEC 4.3), the seam an AI coding agent running in a terminal pane reaches this
// application's viewer panes through.
//
// Everything here is driven through the REAL CodeharbordClient over an
// in-process transport, because the two things worth proving are exactly the two
// this class does: that a `viewer.command` notification off the wire becomes a
// validated signal, and that an answer becomes a `viewer.commandResult` request
// on that same wire. A stubbed client would test neither.
//
// The QML half — what each op actually does to the layout — is covered in
// src/qml/tests/tst_qmlload.cpp against the real Main.qml.

#include "ViewerCommandService.h"

#include "CodeharbordClient.h"
#include "RpcTypes.h"

#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include <cstring>

using namespace ch;

namespace {

// Minimal in-process QIODevice standing in for the RPC transport, matching the
// one in tst_appcontroller.cpp: the app test target does not link Qt6::Network,
// and Unbuffered keeps readyRead dispatch synchronous so no case needs an event
// loop to observe an effect.
class FakeTransport : public QIODevice {
public:
    explicit FakeTransport(QObject* parent = nullptr) : QIODevice(parent)
    {
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return m_incoming.size() + QIODevice::bytesAvailable();
    }

    void deliver(const QByteArray& frame)
    {
        m_incoming.append(frame);
        emit readyRead();
    }

    QByteArray takeSent()
    {
        const QByteArray sent = m_sent;
        m_sent.clear();
        return sent;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_incoming.size());
        if (n > 0) {
            std::memcpy(data, m_incoming.constData(), static_cast<size_t>(n));
            m_incoming.remove(0, n);
        }
        return n;
    }

    qint64 writeData(const char* data, qint64 len) override
    {
        m_sent.append(data, len);
        return len;
    }

private:
    QByteArray m_incoming;
    QByteArray m_sent;
};

// One `viewer.command` notification frame, exactly as codeharbord writes it.
QByteArray commandFrame(const QJsonObject& params)
{
    QJsonObject frame;
    frame.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    frame.insert(QStringLiteral("method"),
                 QString::fromLatin1(rpc::kNotificationViewerCommand));
    frame.insert(QStringLiteral("params"), params);
    return QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject validParams(const QString& op = QStringLiteral("list"),
                        const QString& commandId = QStringLiteral("vc-1"))
{
    QJsonObject params;
    params.insert(QStringLiteral("commandId"), commandId);
    params.insert(QStringLiteral("devSessionId"), QStringLiteral("dev-1"));
    params.insert(QStringLiteral("terminalId"), QStringLiteral("term-1"));
    params.insert(QStringLiteral("op"), op);
    params.insert(QStringLiteral("args"), QJsonObject{});
    return params;
}

// Every JSON-RPC request the client wrote since the last drain, in order.
QVector<QJsonObject> takeRequests(FakeTransport& transport)
{
    QVector<QJsonObject> requests;
    const QByteArray sent = transport.takeSent();
    for (const QByteArray& line : sent.split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        requests.append(QJsonDocument::fromJson(line).object());
    }
    return requests;
}

} // namespace

class TstViewerCommands : public QObject {
    Q_OBJECT

private slots:
    void aValidCommandBecomesASignal();
    void argumentsSurviveAsAVariantMap();
    void aMalformedNotificationIsDroppedSilently();
    void anUnroutableCommandIsRefusedRatherThanEmitted();
    void anUnknownOperationIsRefusedWithoutEmitting();
    void anotherMethodsNotificationIsIgnored();
    void anAnswerBecomesOneCommandResultRequest();
    void aRefusalCarriesItsCodeAndMessage();
    void aSecondAnswerForTheSameCommandIsDropped();
    void anEmptyErrorCodeStillNamesAFailure();
    void pastTheInFlightBoundACommandIsRefusedAsBusy();
    void answeringWithoutAClientIsHarmless();
};

// ---------------------------------------------------------------------------
// Inbound: a notification off the wire.
// ---------------------------------------------------------------------------

void TstViewerCommands::aValidCommandBecomesASignal()
{
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);
    QSignalSpy spy(&service, &ViewerCommandService::commandRequested);

    transport.deliver(commandFrame(validParams(QStringLiteral("focus"))));

    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("vc-1"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("dev-1"));
    QCOMPARE(spy.at(0).at(2).toString(), QStringLiteral("focus"));
    QCOMPARE(service.inFlightCount(), 1);
    // Nothing is answered until the handler answers it.
    QVERIFY(takeRequests(transport).isEmpty());
}

void TstViewerCommands::argumentsSurviveAsAVariantMap()
{
    // The args object is the op's whole payload — a URL, a pane id, a flag — and
    // QML reads it as a plain map. A lossy conversion here would silently drop
    // the very field the command exists to carry.
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);
    QSignalSpy spy(&service, &ViewerCommandService::commandRequested);

    QJsonObject params = validParams(QStringLiteral("open"));
    QJsonObject args;
    args.insert(QStringLiteral("url"), QStringLiteral("README.md"));
    args.insert(QStringLiteral("newPane"), true);
    args.insert(QStringLiteral("kind"), QStringLiteral("editor"));
    params.insert(QStringLiteral("args"), args);
    transport.deliver(commandFrame(params));

    QCOMPARE(spy.size(), 1);
    const QVariantMap received = spy.at(0).at(3).toMap();
    QCOMPARE(received.value(QStringLiteral("url")).toString(), QStringLiteral("README.md"));
    QCOMPARE(received.value(QStringLiteral("newPane")).toBool(), true);
    QCOMPARE(received.value(QStringLiteral("kind")).toString(), QStringLiteral("editor"));
}

void TstViewerCommands::aMalformedNotificationIsDroppedSilently()
{
    // No commandId means no id to answer AGAINST, so there is nothing to report
    // and nobody to report it to. Dropping is the only honest handling, and it
    // must not put a failure in front of a user who did nothing.
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);
    QSignalSpy spy(&service, &ViewerCommandService::commandRequested);

    QJsonObject noId = validParams();
    noId.remove(QStringLiteral("commandId"));
    transport.deliver(commandFrame(noId));

    QJsonObject blankId = validParams();
    blankId.insert(QStringLiteral("commandId"), QStringLiteral("   "));
    transport.deliver(commandFrame(blankId));

    // params that are not an object at all.
    QJsonObject frame;
    frame.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    frame.insert(QStringLiteral("method"),
                 QString::fromLatin1(rpc::kNotificationViewerCommand));
    frame.insert(QStringLiteral("params"), QStringLiteral("not an object"));
    transport.deliver(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');

    QCOMPARE(spy.size(), 0);
    QCOMPARE(service.inFlightCount(), 0);
    QVERIFY(takeRequests(transport).isEmpty());
}

void TstViewerCommands::anUnroutableCommandIsRefusedRatherThanEmitted()
{
    // A command that names no Dev Session or no terminal is structurally valid and
    // cannot be honoured. It HAS an id, so unlike the cases above it is answered:
    // the agent learns why instead of waiting out the daemon's timeout.
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);
    QSignalSpy spy(&service, &ViewerCommandService::commandRequested);

    QJsonObject noSession = validParams();
    noSession.insert(QStringLiteral("devSessionId"), QString());
    transport.deliver(commandFrame(noSession));

    QCOMPARE(spy.size(), 0);
    const QVector<QJsonObject> requests = takeRequests(transport);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.at(0).value(QStringLiteral("method")).toString(),
             QString::fromLatin1(rpc::kMethodViewerCommandResult));
    const QJsonObject params = requests.at(0).value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(params.value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("bad_request"));
    QCOMPARE(service.inFlightCount(), 0);
}

void TstViewerCommands::anUnknownOperationIsRefusedWithoutEmitting()
{
    // The op list is duplicated on purpose (daemon and client), so version skew
    // must produce a NAMED refusal here rather than an unhandled command the QML
    // side would never answer.
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);
    QSignalSpy spy(&service, &ViewerCommandService::commandRequested);

    transport.deliver(commandFrame(validParams(QStringLiteral("teleport"))));

    QCOMPARE(spy.size(), 0);
    const QVector<QJsonObject> requests = takeRequests(transport);
    QCOMPARE(requests.size(), 1);
    const QJsonObject params = requests.at(0).value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("commandId")).toString(), QStringLiteral("vc-1"));
    QCOMPARE(params.value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("bad_request"));
}

void TstViewerCommands::anotherMethodsNotificationIsIgnored()
{
    // The client's notification signal is shared: file.watchEvent arrives on it
    // too, and consuming or answering one of those would corrupt the editor's
    // watch handling.
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);
    QSignalSpy spy(&service, &ViewerCommandService::commandRequested);

    QJsonObject frame;
    frame.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    frame.insert(QStringLiteral("method"), QString::fromLatin1(rpc::kWatchEventNotification));
    frame.insert(QStringLiteral("params"), QJsonObject{
        {QStringLiteral("subscriptionId"), QStringLiteral("sub-1")},
        {QStringLiteral("path"), QStringLiteral("/srv/a.txt")},
        {QStringLiteral("event"), QStringLiteral("modified")},
    });
    transport.deliver(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');

    QCOMPARE(spy.size(), 0);
    QVERIFY(takeRequests(transport).isEmpty());
}

// ---------------------------------------------------------------------------
// Outbound: the answer.
// ---------------------------------------------------------------------------

void TstViewerCommands::anAnswerBecomesOneCommandResultRequest()
{
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);

    transport.deliver(commandFrame(validParams(QStringLiteral("split"))));
    transport.takeSent();

    QVariantMap data;
    data.insert(QStringLiteral("paneId"), QStringLiteral("viewer-2"));
    service.respond(QStringLiteral("vc-1"), true, QString(), QString(), data);

    const QVector<QJsonObject> requests = takeRequests(transport);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.at(0).value(QStringLiteral("method")).toString(),
             QString::fromLatin1(rpc::kMethodViewerCommandResult));
    // An id is present: this is a REQUEST, not a notification, so the daemon's
    // reply completes it and a lost answer is visible rather than silent.
    QVERIFY(requests.at(0).contains(QStringLiteral("id")));
    const QJsonObject params = requests.at(0).value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("commandId")).toString(), QStringLiteral("vc-1"));
    QCOMPARE(params.value(QStringLiteral("ok")).toBool(), true);
    QVERIFY(!params.contains(QStringLiteral("error")));
    QCOMPARE(params.value(QStringLiteral("data")).toObject()
                 .value(QStringLiteral("paneId")).toString(),
             QStringLiteral("viewer-2"));
    QCOMPARE(service.inFlightCount(), 0);
}

void TstViewerCommands::aRefusalCarriesItsCodeAndMessage()
{
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);

    transport.deliver(commandFrame(validParams(QStringLiteral("reload"))));
    transport.takeSent();

    service.respond(QStringLiteral("vc-1"), false, QStringLiteral("unknown_pane"),
                    QStringLiteral("No viewer pane named viewer-9."), {});

    const QVector<QJsonObject> requests = takeRequests(transport);
    QCOMPARE(requests.size(), 1);
    const QJsonObject params = requests.at(0).value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("ok")).toBool(), false);
    const QJsonObject error = params.value(QStringLiteral("error")).toObject();
    QCOMPARE(error.value(QStringLiteral("code")).toString(), QStringLiteral("unknown_pane"));
    QCOMPARE(error.value(QStringLiteral("message")).toString(),
             QStringLiteral("No viewer pane named viewer-9."));
    // An empty data map is omitted rather than sent as {}.
    QVERIFY(!params.contains(QStringLiteral("data")));
}

void TstViewerCommands::aSecondAnswerForTheSameCommandIsDropped()
{
    // A QML handler that fell through two arms would otherwise report an outcome
    // twice, and the second report could settle a command the daemon has since
    // reused the socket for.
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);

    transport.deliver(commandFrame(validParams()));
    transport.takeSent();

    service.respond(QStringLiteral("vc-1"), true, QString(), QString(), {});
    QCOMPARE(takeRequests(transport).size(), 1);

    service.respond(QStringLiteral("vc-1"), true, QString(), QString(), {});
    QVERIFY(takeRequests(transport).isEmpty());
}

void TstViewerCommands::anEmptyErrorCodeStillNamesAFailure()
{
    // The daemon maps an unrecognized code to "failed"; sending the empty string
    // would be that, said less clearly. A caller that only has a message must
    // still produce a valid refusal.
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);

    transport.deliver(commandFrame(validParams()));
    transport.takeSent();

    service.respond(QStringLiteral("vc-1"), false, QString(), QString(), {});

    const QVector<QJsonObject> requests = takeRequests(transport);
    QCOMPARE(requests.size(), 1);
    const QJsonObject error = requests.at(0).value(QStringLiteral("params")).toObject()
                                  .value(QStringLiteral("error")).toObject();
    QCOMPARE(error.value(QStringLiteral("code")).toString(), QStringLiteral("failed"));
    QVERIFY(!error.value(QStringLiteral("message")).toString().isEmpty());
}

void TstViewerCommands::pastTheInFlightBoundACommandIsRefusedAsBusy()
{
    // Back-pressure, not a queue: an agent firing faster than the UI can settle
    // gets told to wait. Without the bound the set would grow with every
    // unanswered command.
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);
    ViewerCommandService service(&client);
    QSignalSpy spy(&service, &ViewerCommandService::commandRequested);

    for (int i = 0; i < ViewerCommandService::kMaxInFlight; ++i) {
        transport.deliver(commandFrame(
            validParams(QStringLiteral("list"), QStringLiteral("vc-%1").arg(i + 1))));
    }
    QCOMPARE(spy.size(), ViewerCommandService::kMaxInFlight);
    QCOMPARE(service.inFlightCount(), ViewerCommandService::kMaxInFlight);
    transport.takeSent();

    transport.deliver(commandFrame(validParams(QStringLiteral("list"),
                                               QStringLiteral("vc-overflow"))));
    QCOMPARE(spy.size(), ViewerCommandService::kMaxInFlight);
    const QVector<QJsonObject> requests = takeRequests(transport);
    QCOMPARE(requests.size(), 1);
    const QJsonObject params = requests.at(0).value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("commandId")).toString(), QStringLiteral("vc-overflow"));
    QCOMPARE(params.value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("busy"));
    // The commands already in flight are untouched.
    QCOMPARE(service.inFlightCount(), ViewerCommandService::kMaxInFlight);
}

void TstViewerCommands::answeringWithoutAClientIsHarmless()
{
    // A service built without a client (or outliving one) must not crash the shell
    // when QML answers a command it announced before the transport went away.
    ViewerCommandService service(nullptr);
    service.respond(QStringLiteral("vc-1"), true, QString(), QString(), {});
    service.respond(QString(), false, QStringLiteral("failed"), QStringLiteral("x"), {});
    QCOMPARE(service.inFlightCount(), 0);
}

// GUILESS: nothing here touches a window, a scene graph or a font. QTEST_MAIN
// would demand a platform plugin and make this case fail on a headless machine
// for reasons that have nothing to do with the channel it tests.
QTEST_GUILESS_MAIN(TstViewerCommands)
#include "tst_viewercommands.moc"
