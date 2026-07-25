#include "AgentEvent.h"
#include "AgentStatusMonitor.h"
#include "SessionState.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QString>
#include <QtTest/QtTest>

using ch::AgentState;
using ch::AgentStatusMonitor;

namespace {

// Build one framed (newline-terminated) AgentEvent JSONL line.
QByteArray eventLine(const QString& state, const QString& dev = QStringLiteral("d1"),
                     const QString& term = QStringLiteral("t1"),
                     const QString& harness = QStringLiteral("generic"),
                     const QString& evName = QStringLiteral("tick"),
                     const QString& summary = QString())
{
    QJsonObject o{
        {"version", 1},
        {"timestamp", "2026-07-25T00:00:00.000Z"},
        {"harness", harness},
        {"devSessionId", dev},
        {"terminalId", term},
        {"state", state},
        {"event", evName},
    };
    if (!summary.isNull())
        o.insert("summary", summary);
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

int asInt(AgentState s) { return static_cast<int>(s); }

} // namespace

// Exercises AgentStatusMonitor against a QLocalSocket pair (no SSH): the
// monitor's transport is one socket, the test writes canned JSONL frames on the
// other. Covers the pure parser (AgentEvent.h) and the monitor's state/unseen/
// notify bookkeeping.
class TstAgentMonitor : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void wireMappingIsExhaustive();       // pure parser
    void parseRejectsMalformed();         // pure parser
    void mapsStatesFromWire();
    void runningToIdleUnseenSetsUnseenAndNotify();
    void markSeenClearsUnseen();
    void notifyOnWaitingInput();
    void malformedAndBlankLinesSkipped();
    void lineSplitAcrossReadsIsBuffered();
    void unknownStateStringMapsToUnknown();
    void perTerminalStateIsIndependent();
    void transportDestroyedThenRebindIsSafe();

private:
    void makePair();
    void feed(const QByteArray& bytes);

    QLocalServer* m_server = nullptr;
    QLocalSocket* m_monitorSide = nullptr; // transport bound to the monitor
    QLocalSocket* m_writerSide = nullptr;  // test writes canned frames here
    AgentStatusMonitor* m_monitor = nullptr;
    static int s_seq;
};

int TstAgentMonitor::s_seq = 0;

void TstAgentMonitor::init()
{
    m_monitor = new AgentStatusMonitor;
}

void TstAgentMonitor::cleanup()
{
    delete m_monitor;
    m_monitor = nullptr;
    delete m_writerSide;
    m_writerSide = nullptr;
    delete m_monitorSide;
    m_monitorSide = nullptr;
    delete m_server;
    m_server = nullptr;
}

void TstAgentMonitor::makePair()
{
    const QString name = QStringLiteral("ch_agent_test_%1_%2")
                             .arg(QCoreApplication::applicationPid())
                             .arg(++s_seq);
    QLocalServer::removeServer(name);

    m_server = new QLocalServer;
    QVERIFY(m_server->listen(name));

    m_monitorSide = new QLocalSocket;
    m_monitorSide->connectToServer(name);
    QVERIFY(m_monitorSide->waitForConnected(2000));
    QVERIFY(m_server->waitForNewConnection(2000));
    m_writerSide = m_server->nextPendingConnection();
    QVERIFY(m_writerSide != nullptr);

    m_monitor->setTransport(m_monitorSide);
}

void TstAgentMonitor::feed(const QByteArray& bytes)
{
    m_writerSide->write(bytes);
    m_writerSide->flush();
}

// --- Pure parser (AgentEvent.h) --------------------------------------------

void TstAgentMonitor::wireMappingIsExhaustive()
{
    QCOMPARE(asInt(ch::agentStateFromWire(u"starting")), asInt(AgentState::Starting));
    QCOMPARE(asInt(ch::agentStateFromWire(u"running")), asInt(AgentState::Running));
    QCOMPARE(asInt(ch::agentStateFromWire(u"waiting_input")), asInt(AgentState::WaitingInput));
    QCOMPARE(asInt(ch::agentStateFromWire(u"idle_unseen")), asInt(AgentState::IdleUnseen));
    QCOMPARE(asInt(ch::agentStateFromWire(u"idle")), asInt(AgentState::Idle));
    QCOMPARE(asInt(ch::agentStateFromWire(u"error")), asInt(AgentState::Error));
    QCOMPARE(asInt(ch::agentStateFromWire(u"stopped")), asInt(AgentState::Stopped));
    QCOMPARE(asInt(ch::agentStateFromWire(u"unknown")), asInt(AgentState::Unknown));
    // Unrecognized token collapses to Unknown.
    QCOMPARE(asInt(ch::agentStateFromWire(u"banana")), asInt(AgentState::Unknown));
    QCOMPARE(asInt(ch::agentStateFromWire(u"")), asInt(AgentState::Unknown));
}

void TstAgentMonitor::parseRejectsMalformed()
{
    // Blank / whitespace.
    QVERIFY(!ch::parseAgentEventLine(QByteArray()).has_value());
    QVERIFY(!ch::parseAgentEventLine("   ").has_value());
    // Not JSON.
    QVERIFY(!ch::parseAgentEventLine("not json").has_value());
    // JSON but not an object.
    QVERIFY(!ch::parseAgentEventLine("[1,2,3]").has_value());
    // Wrong version.
    {
        QJsonObject o{{"version", 2},
                      {"timestamp", "x"},
                      {"harness", "generic"},
                      {"devSessionId", "d"},
                      {"terminalId", "t"},
                      {"state", "idle"},
                      {"event", "e"}};
        QVERIFY(!ch::parseAgentEventLine(
                     QJsonDocument(o).toJson(QJsonDocument::Compact))
                     .has_value());
    }
    // Non-integer version: strict equality with CH_EVENT_VERSION must reject a
    // fractional 1.5 (events.ts uses `===`; a truncating toInt() would wrongly
    // accept it).
    {
        QJsonObject o{{"version", 1.5},
                      {"timestamp", "x"},
                      {"harness", "generic"},
                      {"devSessionId", "d"},
                      {"terminalId", "t"},
                      {"state", "idle"},
                      {"event", "e"}};
        QVERIFY(!ch::parseAgentEventLine(
                     QJsonDocument(o).toJson(QJsonDocument::Compact))
                     .has_value());
    }
    // version as a string is not the JSON number 1.
    {
        QJsonObject o{{"version", "1"},
                      {"timestamp", "x"},
                      {"harness", "generic"},
                      {"devSessionId", "d"},
                      {"terminalId", "t"},
                      {"state", "idle"},
                      {"event", "e"}};
        QVERIFY(!ch::parseAgentEventLine(
                     QJsonDocument(o).toJson(QJsonDocument::Compact))
                     .has_value());
    }
    // Invalid harness.
    QVERIFY(!ch::parseAgentEventLine(
                 eventLine("idle", "d", "t", QStringLiteral("bogus")))
                 .has_value());
    // Missing required field (devSessionId).
    {
        QJsonObject o{{"version", 1},
                      {"timestamp", "x"},
                      {"harness", "generic"},
                      {"terminalId", "t"},
                      {"state", "idle"},
                      {"event", "e"}};
        QVERIFY(!ch::parseAgentEventLine(
                     QJsonDocument(o).toJson(QJsonDocument::Compact))
                     .has_value());
    }
    // Invalid state token.
    QVERIFY(!ch::parseAgentEventLine(eventLine("banana")).has_value());
    // metadata must be an object, not null/array.
    {
        QJsonObject o{{"version", 1},
                      {"timestamp", "x"},
                      {"harness", "generic"},
                      {"devSessionId", "d"},
                      {"terminalId", "t"},
                      {"state", "idle"},
                      {"event", "e"},
                      {"metadata", QJsonValue(QJsonValue::Null)}};
        QVERIFY(!ch::parseAgentEventLine(
                     QJsonDocument(o).toJson(QJsonDocument::Compact))
                     .has_value());
    }

    // A well-formed line with optional summary + metadata parses.
    QJsonObject good{{"version", 1},
                     {"timestamp", "2026-07-25T00:00:00.000Z"},
                     {"harness", "oh-my-pi"},
                     {"devSessionId", "d9"},
                     {"terminalId", "t9"},
                     {"state", "waiting_input"},
                     {"event", "ask_started"},
                     {"summary", "need input"},
                     {"metadata", QJsonObject{{"k", "v"}}}};
    auto ev = ch::parseAgentEventLine(
        QJsonDocument(good).toJson(QJsonDocument::Compact));
    QVERIFY(ev.has_value());
    QCOMPARE(asInt(ev->state), asInt(AgentState::WaitingInput));
    QCOMPARE(ev->harness, QStringLiteral("oh-my-pi"));
    QCOMPARE(ev->devSessionId, QStringLiteral("d9"));
    QCOMPARE(ev->summary, QStringLiteral("need input"));
    QCOMPARE(ev->metadata.value("k").toString(), QStringLiteral("v"));
}

// --- Monitor ---------------------------------------------------------------

void TstAgentMonitor::mapsStatesFromWire()
{
    makePair();
    QSignalSpy spy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    feed(eventLine("starting"));
    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("d1"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("t1"));
    QCOMPARE(spy.at(0).at(2).toInt(), asInt(AgentState::Starting));

    feed(eventLine("running"));
    QTRY_COMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(2).toInt(), asInt(AgentState::Running));
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::Running));

    // A repeated same-state event is not a transition: no new signal.
    feed(eventLine("running"));
    QTest::qWait(100);
    QCOMPARE(spy.count(), 2);

    // Unobserved pair reports Unknown.
    QCOMPARE(m_monitor->stateFor("dX", "tX"), asInt(AgentState::Unknown));
}

void TstAgentMonitor::runningToIdleUnseenSetsUnseenAndNotify()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);
    QSignalSpy notifySpy(m_monitor, &AgentStatusMonitor::notify);

    feed(eventLine("running"));
    QTRY_COMPARE(stateSpy.count(), 1);
    QVERIFY(!m_monitor->hasUnseen("d1"));
    QCOMPARE(unseenSpy.count(), 0);
    QCOMPARE(notifySpy.count(), 0);

    feed(eventLine("idle_unseen", "d1", "t1", "generic", "turn_end",
                   "done building"));
    QTRY_COMPARE(unseenSpy.count(), 1);
    QCOMPARE(unseenSpy.at(0).at(0).toString(), QStringLiteral("d1"));
    QCOMPARE(unseenSpy.at(0).at(1).toBool(), true);
    QVERIFY(m_monitor->hasUnseen("d1"));

    QCOMPARE(notifySpy.count(), 1);
    QCOMPARE(notifySpy.at(0).at(1).toString(), QStringLiteral("done building"));
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::IdleUnseen));

    // A second idle_unseen on the same session (already unseen) does not
    // re-emit unseenChanged, and repeating the identical state is no transition.
    feed(eventLine("idle_unseen", "d1", "t1", "generic", "turn_end",
                   "done building"));
    QTest::qWait(100);
    QCOMPARE(unseenSpy.count(), 1);
}

void TstAgentMonitor::markSeenClearsUnseen()
{
    makePair();
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);

    feed(eventLine("idle_unseen"));
    QTRY_VERIFY(m_monitor->hasUnseen("d1"));
    QCOMPARE(unseenSpy.count(), 1);

    m_monitor->markSeen("d1");
    QCOMPARE(unseenSpy.count(), 2);
    QCOMPARE(unseenSpy.at(1).at(1).toBool(), false);
    QVERIFY(!m_monitor->hasUnseen("d1"));

    // Idempotent: marking an already-seen session emits nothing.
    m_monitor->markSeen("d1");
    QCOMPARE(unseenSpy.count(), 2);
}

void TstAgentMonitor::notifyOnWaitingInput()
{
    makePair();
    QSignalSpy notifySpy(m_monitor, &AgentStatusMonitor::notify);
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);

    feed(eventLine("waiting_input", "d1", "t1", "generic", "ask_started",
                   "approve this?"));
    QTRY_COMPARE(notifySpy.count(), 1);
    QCOMPARE(notifySpy.at(0).at(1).toString(), QStringLiteral("approve this?"));
    // waiting_input is not a completion: no unseen flag.
    QVERIFY(!m_monitor->hasUnseen("d1"));
    QCOMPARE(unseenSpy.count(), 0);
}

void TstAgentMonitor::malformedAndBlankLinesSkipped()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    QByteArray chunk;
    chunk += "\n";                          // blank
    chunk += "   \n";                       // whitespace
    chunk += "{ not json\n";                // malformed JSON
    chunk += "{\"version\":2}\n";           // wrong version
    chunk += eventLine("banana");           // invalid state token
    chunk += eventLine("running");          // the one valid event
    feed(chunk);

    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(2).toInt(), asInt(AgentState::Running));
    // No spurious state was recorded.
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::Running));
}

void TstAgentMonitor::lineSplitAcrossReadsIsBuffered()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    const QByteArray full = eventLine("idle");
    const int half = full.size() / 2; // split mid-JSON, before the '\n'

    feed(full.left(half));
    QTest::qWait(150);
    QCOMPARE(stateSpy.count(), 0); // no complete line yet

    feed(full.mid(half));
    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(2).toInt(), asInt(AgentState::Idle));
}

void TstAgentMonitor::unknownStateStringMapsToUnknown()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    // The literal wire token "unknown" is valid and maps to AgentState::Unknown.
    feed(eventLine("unknown"));
    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(2).toInt(), asInt(AgentState::Unknown));
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::Unknown));

    // An unrecognized token is malformed and is dropped (no signal).
    feed(eventLine("banana"));
    QTest::qWait(100);
    QCOMPARE(stateSpy.count(), 1);
}

void TstAgentMonitor::perTerminalStateIsIndependent()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    feed(eventLine("running", "d1", "t1"));
    feed(eventLine("waiting_input", "d1", "t2"));
    QTRY_COMPARE(stateSpy.count(), 2);

    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::Running));
    QCOMPARE(m_monitor->stateFor("d1", "t2"), asInt(AgentState::WaitingInput));

    // idle_unseen on t2 marks the shared Dev Session unseen exactly once.
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);
    feed(eventLine("idle_unseen", "d1", "t2"));
    QTRY_COMPARE(unseenSpy.count(), 1);
    QVERIFY(m_monitor->hasUnseen("d1"));
}

// The transport is caller-owned; if it is destroyed while the monitor lives,
// m_transport must not dangle. A QPointer auto-nulls on the transport's death,
// so a later setTransport() does not call disconnect() on freed memory (a raw
// pointer would UAF here). Reaching the end without a crash is the assertion.
void TstAgentMonitor::transportDestroyedThenRebindIsSafe()
{
    auto* first = new QBuffer;
    QVERIFY(first->open(QIODevice::ReadWrite));
    m_monitor->setTransport(first);
    QCOMPARE(m_monitor->transport(), static_cast<QIODevice*>(first));

    delete first; // caller destroys the transport out from under the monitor

    QBuffer second;
    QVERIFY(second.open(QIODevice::ReadWrite));
    m_monitor->setTransport(&second); // must not disconnect() the destroyed one
    QCOMPARE(m_monitor->transport(), static_cast<QIODevice*>(&second));

    // Clear before `second` goes out of scope so the monitor drops its pointer.
    m_monitor->setTransport(nullptr);
}

QTEST_MAIN(TstAgentMonitor)
#include "tst_agentmonitor.moc"
