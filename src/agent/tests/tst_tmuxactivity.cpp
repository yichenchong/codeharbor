// Unit gate for ch::TmuxActivityPoller: the client half of `tmux.paneActivity`,
// which is how a Dev Session the user is NOT looking at still reports whether
// its agents are working. No SSH and no daemon — a real ch::CodeharbordClient
// over a QLocalSocket pair, with the TEST playing the server, so every answer's
// content and TIMING is chosen here. Timing matters for the overlap rule: the
// only way to observe "a request is still in flight" is to hold one open.

#include "CodeharbordClient.h"
#include "TmuxActivityPoller.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QString>
#include <QtTest/QtTest>

using ch::CodeharbordClient;
using ch::TmuxActivityPoller;

namespace {

// One pane entry of a tmux.paneActivity result. `lastActivityMs` is a
// QJsonValue rather than a number because NULL is a first-class answer here:
// tmux renders a format name it does not know as an empty field in an otherwise
// SUCCESSFUL listing, so "the daemon could not date this pane" is the shape a
// mismatched tmux produces and the one behaviour that must not be guessed at.
QJsonObject paneEntry(const QString& devSessionId, const QString& terminalId,
                      const QJsonValue& lastActivityMs, bool alive = true)
{
    return QJsonObject{
        {QStringLiteral("devSessionId"), devSessionId},
        {QStringLiteral("terminalId"), terminalId},
        {QStringLiteral("target"), devSessionId + QLatin1Char('-') + terminalId},
        {QStringLiteral("lastActivityMs"), lastActivityMs},
        {QStringLiteral("attached"), false},
        {QStringLiteral("alive"), alive},
    };
}

} // namespace

class TstTmuxActivity : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    // The age is the SERVER's, computed from the `nowMs` it stamped its own
    // listing with, never from this machine's clock.
    void ageIsComputedFromTheServersNowMs();
    // A pane the daemon could not date is reported as nothing at all.
    void aNullLastActivityEmitsNothing();
    // A slow or wedged daemon must not accumulate identical requests.
    void anOverlappingTickIssuesNoSecondRequest();

private:
    // Every JSON-RPC request the poller has written since the last call, oldest
    // first. `timeoutMs` is short for the assertions that nothing was asked.
    QList<QJsonObject> takeRequests(int timeoutMs = 2000);
    // Answer the request with id `id` with `result`.
    void respond(qint64 id, const QJsonObject& result);

    QLocalServer* m_server = nullptr;
    QLocalSocket* m_clientSide = nullptr;  // bound to the CodeharbordClient
    QLocalSocket* m_serverSide = nullptr;  // the test plays the daemon here
    CodeharbordClient* m_client = nullptr;
    TmuxActivityPoller* m_poller = nullptr;
    QByteArray m_serverBuffer;
    static int s_seq;
};

int TstTmuxActivity::s_seq = 0;

void TstTmuxActivity::init()
{
    const QString name = QStringLiteral("ch_tmuxact_%1_%2")
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

    m_client = new CodeharbordClient;
    m_client->setTransport(m_clientSide);
    m_poller = new TmuxActivityPoller;
    m_poller->setRpcClient(m_client);
    m_serverBuffer.clear();
}

void TstTmuxActivity::cleanup()
{
    delete m_poller;
    m_poller = nullptr;
    delete m_client;
    m_client = nullptr;
    delete m_serverSide;
    m_serverSide = nullptr;
    delete m_clientSide;
    m_clientSide = nullptr;
    delete m_server;
    m_server = nullptr;
}

QList<QJsonObject> TstTmuxActivity::takeRequests(int timeoutMs)
{
    // QLocalSocket buffers a write until the event loop turns, and these tests
    // deliberately never turn it between issuing a poll and reading the wire.
    // Without this flush the request is still sitting in the client's own
    // buffer and every assertion below would read as "nothing was asked" — the
    // exact claim the overlap test makes, so it must not be an artefact of
    // reading too early.
    m_clientSide->flush();
    QDeadlineTimer deadline(timeoutMs);
    while (!m_serverBuffer.contains('\n') && !deadline.hasExpired()) {
        if (m_serverSide->bytesAvailable() > 0 || m_serverSide->waitForReadyRead(20))
            m_serverBuffer.append(m_serverSide->readAll());
    }

    QList<QJsonObject> out;
    qsizetype newline;
    while ((newline = m_serverBuffer.indexOf('\n')) != -1) {
        const QByteArray line = m_serverBuffer.left(newline);
        m_serverBuffer.remove(0, newline + 1);
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        if (!obj.isEmpty())
            out.append(obj);
    }
    return out;
}

void TstTmuxActivity::respond(qint64 id, const QJsonObject& result)
{
    const QJsonObject reply{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), result},
    };
    m_serverSide->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
    m_serverSide->flush();
}

// The two machines' clocks need not agree, and on a host a few minutes out an
// age differenced against the LOCAL clock would report every pane as either
// permanently busy or permanently idle — silently, and in a way no user could
// diagnose. So the daemon stamps each listing with its own wall clock and the
// subtraction happens entirely inside that frame of reference.
//
// The `nowMs` here is deliberately absurd (1970), so a poller that reached for
// QDateTime::currentMSecsSinceEpoch() instead would produce an age of about
// fifty-six years rather than the 700 ms asserted.
void TstTmuxActivity::ageIsComputedFromTheServersNowMs()
{
    QSignalSpy spy(m_poller, &TmuxActivityPoller::activityObserved);

    // Arming polls at once: a Dev Session switch must not wait a whole interval
    // for its first answer.
    m_poller->setDevSessionIds({QStringLiteral("d1")});

    const QList<QJsonObject> requests = takeRequests();
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().value(QStringLiteral("method")).toString(),
             QStringLiteral("tmux.paneActivity"));
    const QJsonArray asked = requests.first()
                                 .value(QStringLiteral("params"))
                                 .toObject()
                                 .value(QStringLiteral("devSessionIds"))
                                 .toArray();
    QCOMPARE(asked.size(), 1);
    QCOMPARE(asked.first().toString(), QStringLiteral("d1"));

    respond(requests.first().value(QStringLiteral("id")).toInteger(),
            QJsonObject{
                {QStringLiteral("nowMs"), 1000000},
                {QStringLiteral("panes"),
                 QJsonArray{paneEntry(QStringLiteral("d1"), QStringLiteral("t1"),
                                      999300),
                            paneEntry(QStringLiteral("d1"), QStringLiteral("t2"),
                                      1000000, false)}},
            });

    QTRY_COMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("d1"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("t1"));
    QCOMPARE(spy.at(0).at(2).toLongLong(), 700LL);
    QCOMPARE(spy.at(0).at(3).toBool(), true);
    // Dated to the listing's own instant: an age of zero, not of the epoch.
    QCOMPARE(spy.at(1).at(2).toLongLong(), 0LL);
    QCOMPARE(spy.at(1).at(3).toBool(), false);
}

// The trap this whole design avoids: an unrecognised `#{...}` does not make
// tmux fail, it renders as an EMPTY field in a successful listing. The daemon
// therefore answers null for a pane it could not date, and the only safe
// reading of that is "we do not know". Fabricating an age would be a silent lie
// in one direction or the other — 0 dates the pane to 1970 and makes it look
// permanently idle, `nowMs` makes it look permanently busy — and it would be a
// lie on exactly the tmux whose format names differ from the one this was
// written against.
void TstTmuxActivity::aNullLastActivityEmitsNothing()
{
    QSignalSpy spy(m_poller, &TmuxActivityPoller::activityObserved);
    m_poller->setDevSessionIds({QStringLiteral("d1")});

    const QList<QJsonObject> requests = takeRequests();
    QCOMPARE(requests.size(), 1);
    respond(requests.first().value(QStringLiteral("id")).toInteger(),
            QJsonObject{
                {QStringLiteral("nowMs"), 5000},
                {QStringLiteral("panes"),
                 QJsonArray{
                     // Explicitly null: alive, but undatable.
                     paneEntry(QStringLiteral("d1"), QStringLiteral("tNull"),
                               QJsonValue(QJsonValue::Null)),
                     // The member missing altogether reads the same way.
                     QJsonObject{
                         {QStringLiteral("devSessionId"), QStringLiteral("d1")},
                         {QStringLiteral("terminalId"), QStringLiteral("tGone")},
                         {QStringLiteral("alive"), false},
                     },
                     // Non-numeric — an empty tmux field relayed verbatim.
                     paneEntry(QStringLiteral("d1"), QStringLiteral("tBlank"),
                               QStringLiteral("")),
                     // ...and one the daemon COULD date, so the listing is not
                     // being dropped wholesale.
                     paneEntry(QStringLiteral("d1"), QStringLiteral("tOk"), 4000),
                 }},
            });

    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("tOk"));
    QCOMPARE(spy.at(0).at(2).toLongLong(), 1000LL);

    // A listing with no server clock at all cannot be measured against
    // anything, so none of it is reported.
    m_poller->pollNow();
    const QList<QJsonObject> second = takeRequests();
    QCOMPARE(second.size(), 1);
    respond(second.first().value(QStringLiteral("id")).toInteger(),
            QJsonObject{{QStringLiteral("panes"),
                         QJsonArray{paneEntry(QStringLiteral("d1"),
                                              QStringLiteral("tOk"), 4000)}}});
    QTest::qWait(100);
    QCOMPARE(spy.count(), 1);
}

// A daemon that has gone slow or wedged must not accumulate an unbounded queue
// of identical questions: one per tick, forever, every one of them answered in
// a burst if it ever recovers. Skipping costs nothing, because the next tick
// asks the same question and gets a FRESHER answer than the one that was
// dropped.
void TstTmuxActivity::anOverlappingTickIssuesNoSecondRequest()
{
    m_poller->setPollIntervalMs(20);
    m_poller->setDevSessionIds({QStringLiteral("d1")});

    // The first request goes out and is deliberately never answered.
    const QList<QJsonObject> first = takeRequests();
    QCOMPARE(first.size(), 1);

    // Many ticks elapse against a silent server. Short deadlines here: the
    // claim is that the wire stays empty, so waiting the full default would
    // only make the suite slow.
    QTest::qWait(300);
    QCOMPARE(takeRequests(100).size(), 0);
    // An explicit poll is refused by the same guard.
    m_poller->pollNow();
    QCOMPARE(takeRequests(100).size(), 0);

    // The answer lands: polling resumes on the very next tick.
    respond(first.first().value(QStringLiteral("id")).toInteger(),
            QJsonObject{{QStringLiteral("nowMs"), 5000},
                        {QStringLiteral("panes"), QJsonArray{}}});
    // The real event loop, so the client can route the reply and the poll timer
    // can fire: waitForReadyRead() inside takeRequests() drives the socket but
    // not QTimer.
    QTest::qWait(200);
    QVERIFY(!takeRequests().isEmpty());
}

QTEST_MAIN(TstTmuxActivity)
#include "tst_tmuxactivity.moc"
