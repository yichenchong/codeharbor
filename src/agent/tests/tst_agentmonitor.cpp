#include "AgentEvent.h"
#include "AgentStatusMonitor.h"
#include "SessionState.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QString>
#include <QStringView>
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
    void strictWireMappingSeparatesUnknownFromGarbage(); // pure parser
    void parseRejectsMalformed();         // pure parser
    void parseRejectsBlankIdentifiers();  // pure parser
    void mapsStatesFromWire();
    void runningToIdleUnseenSetsUnseenAndNotify();
    void markSeenClearsUnseen();
    void notifyOnWaitingInput();
    void malformedAndBlankLinesSkipped();
    void lineSplitAcrossReadsIsBuffered();
    void unknownStateStringMapsToUnknown();
    void perTerminalStateIsIndependent();
    void unseenIsPerDevSession();
    void retainDevSessionsEvictsStaleSubtrees();
    void completionAfterMarkSeenReArmsUnseen();
    void crlfFramedLinesAreAccepted();
    void transportDestroyedThenRebindIsSafe();
    void accumulatedStateSurvivesAReconnect();
    void blankIdentifierEventsNeverReachTheState();
    void bufferedBytesAreDrainedWhenTheTransportIsBound();
    void rebindingTheSameTransportDoesNotDuplicateDelivery();
    void oversizedUnterminatedLineIsDroppedAndStreamResyncs();
    void oversizedFrameTailIsNeverParsedAsItsOwnEvent();
    void overCapCompleteFrameIsDroppedWhateverTheChunking();
    void errorStateIsNotSticky();
    void onlyAttentionStatesNotify();
    void notificationBodyFallsBackToIdentifiers();
    void repeatedAndReorderedEventsAreAppliedInArrivalOrder();
    void retainDevSessionsIsSilentAndTotalWhenNothingIsLive();
    void markSeenForAnUnknownDevSessionIsSilent();
    void monitorWithoutATransportIsInert();

private:
    void makePair();
    // Swap the transport the way SessionBootstrap does on a SPEC 5.6 reconnect:
    // detach the dead channel, then bind one onto the REPLACEMENT bridge.
    void swapPair();
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

void TstAgentMonitor::swapPair()
{
    m_monitor->setTransport(nullptr);
    delete m_writerSide;
    m_writerSide = nullptr;
    delete m_monitorSide;
    m_monitorSide = nullptr;
    delete m_server;
    m_server = nullptr;
    makePair();
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

// The strict mapping is the single token table both the lenient mapping and the
// parser's validation are derived from, and the ONLY thing that can tell the
// two ways of arriving at AgentState::Unknown apart: the wire token "unknown"
// is a legitimate state a producer may report, while any other token means the
// producer is speaking a schema this client does not know and the whole event
// must be dropped. Collapse the two and an unrecognized token would be recorded
// as a real "unknown" state, overwriting whatever the terminal was actually
// doing.
void TstAgentMonitor::strictWireMappingSeparatesUnknownFromGarbage()
{
    const auto genuine = ch::agentStateFromWireStrict(u"unknown");
    QVERIFY(genuine.has_value());
    QCOMPARE(asInt(*genuine), asInt(AgentState::Unknown));

    QVERIFY(!ch::agentStateFromWireStrict(u"banana").has_value());
    QVERIFY(!ch::agentStateFromWireStrict(u"").has_value());
    // Tokens are exact: no case folding, no trimming, no prefix matching.
    QVERIFY(!ch::agentStateFromWireStrict(u"Idle").has_value());
    QVERIFY(!ch::agentStateFromWireStrict(u" idle").has_value());
    QVERIFY(!ch::agentStateFromWireStrict(u"idle_").has_value());
    // Every state the lenient mapping resolves is also a valid wire token.
    for (QStringView token : {QStringView(u"starting"), QStringView(u"running"),
                              QStringView(u"waiting_input"),
                              QStringView(u"idle_unseen"), QStringView(u"idle"),
                              QStringView(u"error"), QStringView(u"stopped"),
                              QStringView(u"unknown")}) {
        QVERIFY2(ch::agentStateFromWireStrict(token).has_value(),
                 qPrintable(token.toString()));
        QCOMPARE(asInt(*ch::agentStateFromWireStrict(token)),
                 asInt(ch::agentStateFromWire(token)));
    }
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
    // metadata must be an object, not an array either.
    {
        QJsonObject o{{"version", 1},
                      {"timestamp", "x"},
                      {"harness", "generic"},
                      {"devSessionId", "d"},
                      {"terminalId", "t"},
                      {"state", "idle"},
                      {"event", "e"},
                      {"metadata", QJsonArray{1, 2}}};
        QVERIFY(!ch::parseAgentEventLine(
                     QJsonDocument(o).toJson(QJsonDocument::Compact))
                     .has_value());
    }
    // Every remaining field is type-checked too, matching validateEvent() in
    // remote/src/events.ts field for field. Without these an event carrying a
    // number where a string belongs would be silently coerced (QJsonValue's
    // toString() yields an empty QString for a non-string), so a terminal would
    // be filed under the empty id instead of its own.
    {
        struct WrongType {
            const char* field;
            QJsonValue bad;
        };
        const WrongType wrongTypes[] = {
            {"timestamp", QJsonValue(7)},   {"harness", QJsonValue(7)},
            {"devSessionId", QJsonValue(7)}, {"terminalId", QJsonValue(7)},
            {"state", QJsonValue(7)},       {"event", QJsonValue(7)},
            {"summary", QJsonValue(7)},
        };
        for (const WrongType& wt : wrongTypes) {
            QJsonObject o{{"version", 1},
                          {"timestamp", "x"},
                          {"harness", "generic"},
                          {"devSessionId", "d"},
                          {"terminalId", "t"},
                          {"state", "idle"},
                          {"event", "e"}};
            o.insert(QString::fromLatin1(wt.field), wt.bad);
            QVERIFY2(!ch::parseAgentEventLine(
                          QJsonDocument(o).toJson(QJsonDocument::Compact))
                          .has_value(),
                     wt.field);
        }
    }
    // Unknown extra fields are ignored, not rejected: events.ts's validator is
    // structural, so a newer producer adding a field must not break an older
    // client.
    {
        QJsonObject o{{"version", 1},
                      {"timestamp", "x"},
                      {"harness", "generic"},
                      {"devSessionId", "d"},
                      {"terminalId", "t"},
                      {"state", "idle"},
                      {"event", "e"},
                      {"somethingNew", "ignored"}};
        QVERIFY(ch::parseAgentEventLine(
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

// A devSessionId or terminalId that is present and a string but BLANK is not a
// harmless event: it is structurally valid, so a parser that only checks the
// type accepts it and files the terminal under a Dev Session that does not
// exist and no sidebar row can ever show. Worse, for the two states that
// notify, the notification body falls back to "<devSessionId> / <terminalId>",
// so the user gets a desktop bubble that reads " / " and points nowhere.
//
// Every producer in the chain already refuses to emit one — missingCoordinates()
// in remote/src/hooks/oh-my-pi-hook.ts, the relay guard in remote/src/bridge.ts,
// and isEventIdentifier() inside validateEvent() in remote/src/events.ts — and
// the client is the last edge, so it must refuse it too instead of trusting
// them. This is the case that pins the C++ validator to validateEvent() field
// for field rather than merely field-type for field-type.
void TstAgentMonitor::parseRejectsBlankIdentifiers()
{
    const char* const blanks[] = {"", " ", "\t", "\n", "   \t  "};
    for (const char* blank : blanks) {
        for (const char* field : {"devSessionId", "terminalId"}) {
            QJsonObject o{{"version", 1},
                          {"timestamp", "x"},
                          {"harness", "generic"},
                          {"devSessionId", "d"},
                          {"terminalId", "t"},
                          {"state", "idle"},
                          {"event", "e"}};
            o.insert(QString::fromLatin1(field),
                     QString::fromUtf8(blank));
            QVERIFY2(!ch::parseAgentEventLine(
                          QJsonDocument(o).toJson(QJsonDocument::Compact))
                          .has_value(),
                     qPrintable(QStringLiteral("%1=0x%2 was accepted")
                                    .arg(QString::fromLatin1(field),
                                         QString::fromLatin1(
                                             QByteArray(blank).toHex()))));
        }
    }

    // An id with surrounding whitespace but real content is NOT blank and is
    // kept verbatim: trimming is the emptiness test, never a normalisation
    // step. The server mints these ids and the client must echo back exactly
    // what it was given.
    QJsonObject padded{{"version", 1},
                       {"timestamp", "x"},
                       {"harness", "generic"},
                       {"devSessionId", " d "},
                       {"terminalId", "\tt"},
                       {"state", "idle"},
                       {"event", "e"}};
    const auto ev = ch::parseAgentEventLine(
        QJsonDocument(padded).toJson(QJsonDocument::Compact));
    QVERIFY(ev.has_value());
    QCOMPARE(ev->devSessionId, QStringLiteral(" d "));
    QCOMPARE(ev->terminalId, QStringLiteral("\tt"));
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

    // A second idle_unseen while the session is STILL flagged unseen adds
    // nothing: no unseenChanged (the flag did not flip), no agentStateChanged
    // (identical state is not a transition), and no notification (the user is
    // already being told about work they have not seen).
    feed(eventLine("idle_unseen", "d1", "t1", "generic", "turn_end",
                   "done building"));
    QTest::qWait(100);
    QCOMPARE(unseenSpy.count(), 1);
    QCOMPARE(stateSpy.count(), 2);
    QCOMPARE(notifySpy.count(), 1);
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

// The unseen-completion flag is scoped to ONE Dev Session, not to the whole
// client and not to a single terminal (SPEC 4.2: the sidebar folds a session's
// terminals into one row state, so "finished with unseen output" is a property
// of the row). One session finishing must not light up its neighbour's badge,
// and clearing one must not clear the other's.
void TstAgentMonitor::unseenIsPerDevSession()
{
    makePair();
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);

    feed(eventLine("idle_unseen", "dA", "tA"));
    QTRY_VERIFY(m_monitor->hasUnseen("dA"));
    QVERIFY(!m_monitor->hasUnseen("dB"));

    feed(eventLine("idle_unseen", "dB", "tB"));
    QTRY_VERIFY(m_monitor->hasUnseen("dB"));
    QCOMPARE(unseenSpy.count(), 2);

    // Marking one seen leaves the other pending.
    m_monitor->markSeen(QStringLiteral("dA"));
    QVERIFY(!m_monitor->hasUnseen("dA"));
    QVERIFY(m_monitor->hasUnseen("dB"));
    QCOMPARE(unseenSpy.count(), 3);

    // ...and the per-terminal raw states stayed in their own sub-maps.
    QCOMPARE(m_monitor->stateFor("dA", "tA"), asInt(AgentState::IdleUnseen));
    QCOMPARE(m_monitor->stateFor("dB", "tB"), asInt(AgentState::IdleUnseen));
    // A terminal id is only meaningful inside its own Dev Session.
    QCOMPARE(m_monitor->stateFor("dA", "tB"), asInt(AgentState::Unknown));
}

// AG7: retainDevSessions() evicts whole Dev Session subtrees for ids the server
// no longer lists, and only those — the surviving Dev Session keeps every
// terminal state and its unseen flag, while the removed one loses both at once.
void TstAgentMonitor::retainDevSessionsEvictsStaleSubtrees()
{
    makePair();

    // Seed two Dev Sessions: A with two terminals (one finished unseen), B with
    // one finished-unseen terminal. Both carry per-terminal state and an unseen
    // flag.
    feed(eventLine("idle_unseen", "A", "ta1"));
    feed(eventLine("running", "A", "ta2"));
    feed(eventLine("idle_unseen", "B", "tb1"));
    QTRY_COMPARE(m_monitor->stateFor("B", "tb1"), asInt(AgentState::IdleUnseen));
    QVERIFY(m_monitor->hasUnseen("A"));
    QVERIFY(m_monitor->hasUnseen("B"));

    // Rebuild lists only A: B's subtree and unseen flag are dropped whole.
    m_monitor->retainDevSessions({QStringLiteral("A")});

    // A survives intact.
    QCOMPARE(m_monitor->stateFor("A", "ta1"), asInt(AgentState::IdleUnseen));
    QCOMPARE(m_monitor->stateFor("A", "ta2"), asInt(AgentState::Running));
    QVERIFY(m_monitor->hasUnseen("A"));

    // B is gone: both its terminal state and its unseen flag.
    QCOMPARE(m_monitor->stateFor("B", "tb1"), asInt(AgentState::Unknown));
    QVERIFY(!m_monitor->hasUnseen("B"));
}

// REGRESSION: a completion that arrives after the user cleared the previous one
// must raise the badge again even though the terminal's raw state never left
// IdleUnseen.
//
// markSeen() clears the per-session flag but deliberately leaves the terminal at
// IdleUnseen (AppController::rebuildRows downgrades it for display). So the next
// idle_unseen event carries the SAME state the monitor already has, and gating
// the unseen bookkeeping on the state transition — as the monitor originally did
// — discarded it as a no-op: the badge never returned and no notification was
// raised for finished work the user had genuinely not seen. The flag must
// therefore be re-armed from the EVENT, not from the transition.
void TstAgentMonitor::completionAfterMarkSeenReArmsUnseen()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);
    QSignalSpy notifySpy(m_monitor, &AgentStatusMonitor::notify);

    feed(eventLine("idle_unseen", "d1", "t1", "generic", "turn_end", "first"));
    QTRY_COMPARE(unseenSpy.count(), 1);
    QCOMPARE(notifySpy.count(), 1);
    QCOMPARE(stateSpy.count(), 1);

    m_monitor->markSeen(QStringLiteral("d1"));
    QCOMPARE(unseenSpy.count(), 2);
    QVERIFY(!m_monitor->hasUnseen("d1"));
    // The raw state is intentionally still IdleUnseen.
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::IdleUnseen));

    // A brand-new completion, with no intervening running/idle event.
    feed(eventLine("idle_unseen", "d1", "t1", "generic", "turn_end", "second"));
    QTRY_COMPARE(unseenSpy.count(), 3);
    QCOMPARE(unseenSpy.at(2).at(0).toString(), QStringLiteral("d1"));
    QCOMPARE(unseenSpy.at(2).at(1).toBool(), true);
    QVERIFY(m_monitor->hasUnseen("d1"));

    // The user is told about it, with THIS completion's summary.
    QCOMPARE(notifySpy.count(), 2);
    QCOMPARE(notifySpy.at(1).at(0).toString(), QStringLiteral("Agent finished"));
    QCOMPARE(notifySpy.at(1).at(1).toString(), QStringLiteral("second"));

    // Still not a state transition, so no redundant agentStateChanged.
    QCOMPARE(stateSpy.count(), 1);
}

// The producer frames with '\n'; a peer that emits CRLF (a Windows-side relay,
// or a shell wrapper that translates line endings) must still be understood.
// The carriage return has to be stripped before the JSON payload is handed to
// the parser, and a bare CRLF must count as a blank line rather than a
// malformed event.
void TstAgentMonitor::crlfFramedLinesAreAccepted()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    QByteArray chunk = QByteArray("\r\n"); // blank CRLF line
    QByteArray framed = eventLine("running");
    framed.chop(1); // drop the '\n' so we can re-frame it as CRLF
    chunk += framed + "\r\n";
    feed(chunk);

    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(2).toInt(), asInt(AgentState::Running));
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::Running));
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

// SPEC 5.6 reconnect. A dropped session is re-wired onto a BRAND NEW
// codeharbor-bridge process, and SessionBootstrap hands the monitor a channel
// onto it (setTransport(nullptr), then setTransport(newDevice)).
//
// The question this case answers is "does the monitor need anything REPLAYED
// across that swap?", and the answer is no — but only because of two
// properties that would otherwise be nobody's job to keep:
//
//   * The monitor holds NO server-side state. Unlike EditorController, which
//     mints a file.watch subscription that lives in the codeharbord process
//     and dies with it, the monitor never subscribes to anything: the bridge
//     pushes AgentEvent JSONL at whoever is on the socket. There is no token
//     to re-establish, so there is nothing to re-request.
//   * Everything it accumulated is the USER's, not the wire's — which
//     terminals are running, which Dev Sessions have an unseen completion.
//     setTransport() must therefore preserve it. A swap that reset it would
//     silently clear an unseen-completion badge for a result the user never
//     saw, which is precisely the notification this subsystem exists to keep.
//
// And the byte stream itself must not be spliced: a frame the dying bridge only
// got halfway through is an unrelated byte sequence from the new one's first
// frame, so the partial MUST be discarded rather than concatenated. Fusing them
// destroys BOTH — the join is not valid JSON, so the real event that follows is
// dropped with it.
void TstAgentMonitor::accumulatedStateSurvivesAReconnect()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);

    feed(eventLine("running", "d1", "t1"));
    feed(eventLine("idle_unseen", "d1", "t2"));
    QTRY_COMPARE(stateSpy.count(), 2);
    QTRY_VERIFY(m_monitor->hasUnseen("d1"));

    // The bridge dies mid-frame. lineSplitAcrossReadsIsBuffered() pins that a
    // partial line really is held in the read buffer rather than dropped, so
    // after this wait the monitor is genuinely carrying these bytes.
    feed(eventLine("error", "d1", "t1").left(20));
    QTest::qWait(150);
    QCOMPARE(stateSpy.count(), 2);

    swapPair();

    // Nothing was replayed, and nothing was lost.
    QCOMPARE(stateSpy.count(), 2);
    QCOMPARE(unseenSpy.count(), 1);
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::Running));
    QCOMPARE(m_monitor->stateFor("d1", "t2"), asInt(AgentState::IdleUnseen));
    QVERIFY(m_monitor->hasUnseen("d1"));

    // The replacement's stream is live AND is read as itself: had the orphaned
    // half-frame survived the swap it would have been glued to the front of
    // this event, and the fused line — invalid JSON — would have taken the real
    // event down with it, leaving the terminal stuck on Running forever.
    feed(eventLine("waiting_input", "d1", "t1"));
    QTRY_COMPARE(stateSpy.count(), 3);
    QCOMPARE(stateSpy.at(2).at(2).toInt(), asInt(AgentState::WaitingInput));
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::WaitingInput));

    // The user's pending completion is still pending, and still clearable.
    QVERIFY(m_monitor->hasUnseen("d1"));
    m_monitor->markSeen(QStringLiteral("d1"));
    QCOMPARE(unseenSpy.count(), 2);
    QCOMPARE(unseenSpy.at(1).at(1).toBool(), false);
}

// The wire-level counterpart of parseRejectsBlankIdentifiers(): an event whose
// devSessionId or terminalId is blank must be dropped by the monitor, leaving
// no state, no unseen flag and no notification behind. The one well-formed
// event at the end proves the monitor really was reading the stream, so a
// silent transport cannot make this case pass by accident.
void TstAgentMonitor::blankIdentifierEventsNeverReachTheState()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);
    QSignalSpy notifySpy(m_monitor, &AgentStatusMonitor::notify);

    // idle_unseen and waiting_input are the two states that would otherwise
    // raise a desktop notification whose body is little more than a slash.
    feed(eventLine("idle_unseen", QString(), QStringLiteral("t1")));
    feed(eventLine("waiting_input", QStringLiteral("d1"), QStringLiteral("   ")));
    feed(eventLine("running", QStringLiteral("\t"), QStringLiteral("\t")));
    feed(eventLine("running", QStringLiteral("d1"), QStringLiteral("t1")));

    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).toString(), QStringLiteral("d1"));
    QCOMPARE(stateSpy.at(0).at(1).toString(), QStringLiteral("t1"));
    QCOMPARE(stateSpy.at(0).at(2).toInt(), asInt(AgentState::Running));

    QTest::qWait(100);
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(unseenSpy.count(), 0);
    QCOMPARE(notifySpy.count(), 0);
    QVERIFY(!m_monitor->hasUnseen(QString()));
    QVERIFY(!m_monitor->hasUnseen(QStringLiteral("d1")));
    QCOMPARE(m_monitor->stateFor(QString(), QStringLiteral("t1")),
             asInt(AgentState::Unknown));
    QCOMPARE(m_monitor->stateFor(QStringLiteral("d1"), QStringLiteral("   ")),
             asInt(AgentState::Unknown));
}

// setTransport() drains whatever is ALREADY buffered on the device, and it does
// so synchronously, before it returns. Two consequences are asserted here
// because production depends on both: the first frames of a freshly opened SSH
// channel are never lost to the gap between opening it and subscribing to
// readyRead, and a caller that wants to observe those frames must connect its
// own slots BEFORE binding the transport (this case does exactly that, and the
// counts are checked with no event loop spun in between).
void TstAgentMonitor::bufferedBytesAreDrainedWhenTheTransportIsBound()
{
    QBuffer preloaded;
    preloaded.setData(eventLine("running", QStringLiteral("dB"),
                                QStringLiteral("tB"))
                      + eventLine("idle_unseen", QStringLiteral("dB"),
                                  QStringLiteral("tB"), QStringLiteral("generic"),
                                  QStringLiteral("turn_end"),
                                  QStringLiteral("done")));
    QVERIFY(preloaded.open(QIODevice::ReadOnly));

    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);
    QSignalSpy notifySpy(m_monitor, &AgentStatusMonitor::notify);

    m_monitor->setTransport(&preloaded);

    QCOMPARE(stateSpy.count(), 2);
    QCOMPARE(stateSpy.at(0).at(2).toInt(), asInt(AgentState::Running));
    QCOMPARE(stateSpy.at(1).at(2).toInt(), asInt(AgentState::IdleUnseen));
    QCOMPARE(unseenSpy.count(), 1);
    QCOMPARE(notifySpy.count(), 1);
    QCOMPARE(notifySpy.at(0).at(1).toString(), QStringLiteral("done"));
    QVERIFY(m_monitor->hasUnseen(QStringLiteral("dB")));

    // Drop the pointer before the local buffer dies.
    m_monitor->setTransport(nullptr);
}

// Binding the device that is already bound is documented as a no-op "buffer
// included", and both halves of that matter. If it instead re-ran the wiring it
// would add a SECOND readyRead connection and every chunk would be handled
// twice; if it instead cleared the read buffer it would throw away a frame that
// is only half-received, and the half that eventually arrives would be parsed
// as a line of its own and dropped, losing a real state change.
void TstAgentMonitor::rebindingTheSameTransportDoesNotDuplicateDelivery()
{
    makePair();
    m_monitor->setTransport(m_monitorSide);
    m_monitor->setTransport(m_monitorSide);

    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    feed(eventLine("running"));
    QTRY_COMPARE(stateSpy.count(), 1);
    QTest::qWait(100);
    QCOMPARE(stateSpy.count(), 1);

    // Half a frame in flight, then a redundant rebind, then the other half.
    const QByteArray full = eventLine("idle");
    const int half = full.size() / 2;
    feed(full.left(half));
    QTest::qWait(150);
    m_monitor->setTransport(m_monitorSide);
    feed(full.mid(half));

    QTRY_COMPARE(stateSpy.count(), 2);
    QCOMPARE(stateSpy.at(1).at(2).toInt(), asInt(AgentState::Idle));
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::Idle));
}

// A producer that streams without ever emitting '\n' must not be able to grow
// the client's read buffer without bound. The monitor caps an unframed frame at
// 1 MiB and drops it — and, crucially, remembers that it did: the bytes between
// the cut and the next newline are the discarded frame's TAIL, an arbitrary
// fragment, and handing that to the JSON parser is asking to be lucky. It is
// skipped, and framing resumes at the following line, which is a real event.
void TstAgentMonitor::oversizedUnterminatedLineIsDroppedAndStreamResyncs()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    // 1.5 MiB, no newline anywhere: comfortably past the 1 MiB cap.
    feed(QByteArray(3 * 512 * 1024, 'x'));
    QTRY_COMPARE_WITH_TIMEOUT(m_writerSide->bytesToWrite(), qint64(0), 15000);
    QTest::qWait(200);
    QCOMPARE(stateSpy.count(), 0);

    // The newline that finally arrives closes the oversized frame; the event on
    // the line after it must still be understood.
    feed(QByteArray("\n")
         + eventLine("running", QStringLiteral("dOS"), QStringLiteral("tOS")));
    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).toString(), QStringLiteral("dOS"));
    QCOMPARE(stateSpy.at(0).at(2).toInt(), asInt(AgentState::Running));
    QCOMPARE(m_monitor->stateFor("dOS", "tOS"), asInt(AgentState::Running));
}

// The discriminating case for the oversize guard's discard bookkeeping, and the
// reason it exists at all.
//
// Here the producer emits 1 MiB + 1 bytes of unframed junk and then, WITHOUT
// ever writing the newline that would have closed it, a perfectly well-formed
// event line. On the wire that is ONE frame — junk and event glued together —
// and it is malformed. The junk alone blows the size cap, so the monitor throws
// the accumulated bytes away; what is left in the socket is the second half of
// a frame the client has already mutilated.
//
// If the monitor simply cleared its buffer and carried on, that leftover would
// look exactly like a complete, valid event and would be applied — the client
// would have invented a frame boundary the producer never wrote and recorded a
// state from a corrupted stream. It must instead skip everything up to the next
// newline and resume framing there.
//
// (The junk is written and fully consumed before the event is written, so the
// clear is guaranteed to have happened first: without that ordering the two
// halves could arrive in one read and be framed as a single line anyway, which
// would prove nothing.)
void TstAgentMonitor::oversizedFrameTailIsNeverParsedAsItsOwnEvent()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    feed(QByteArray(1024 * 1024 + 1, 'x'));
    QTRY_COMPARE_WITH_TIMEOUT(m_writerSide->bytesToWrite(), qint64(0), 15000);
    QTest::qWait(200);
    QCOMPARE(stateSpy.count(), 0);

    // Valid on its own, but it is the tail of the mutilated frame above.
    feed(eventLine("running", QStringLiteral("dTail"), QStringLiteral("tTail")));
    QTest::qWait(200);
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(m_monitor->stateFor("dTail", "tTail"), asInt(AgentState::Unknown));

    // Framing has resynchronised: the NEXT line is a frame of its own and is
    // applied normally.
    feed(eventLine("idle", QStringLiteral("dTail"), QStringLiteral("tTail")));
    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(2).toInt(), asInt(AgentState::Idle));
    QCOMPARE(m_monitor->stateFor("dTail", "tTail"), asInt(AgentState::Idle));
}

// The size cap is a property of the FRAME, not of how the producer's bytes
// happened to be chopped up by the socket.
//
// oversizedUnterminatedLineIsDroppedAndStreamResyncs() covers the fragmented
// case: bytes accumulate past 1 MiB with no newline in sight, the buffer is
// thrown away and the frame's tail skipped. But a line that is over the cap AND
// arrives newline-and-all in a single read never touches that guard — the
// framing loop sees a complete line and used to parse and apply it. The same
// event was therefore accepted or rejected purely on read granularity, which is
// not a property any producer or test can control.
//
// The window is reachable rather than theoretical: codeharbor-bridge caps the
// message it accepts at MAX_BRIDGE_LINE_BYTES (1 MiB, the same number) and then
// emits a strictly LARGER event line — it adds version, timestamp and harness
// fields — so a harness whose summary is close to a megabyte produces exactly
// this frame.
//
// A QBuffer is used instead of the socket pair on purpose: setTransport()
// drains it in one synchronous read, so the whole over-cap line is guaranteed to
// be framed as one complete line. That is the case the socket pair cannot
// reliably reproduce.
void TstAgentMonitor::overCapCompleteFrameIsDroppedWhateverTheChunking()
{
    QBuffer preloaded;
    // 1 MiB of summary alone puts the framed line past the 1 MiB cap.
    preloaded.setData(eventLine("running", QStringLiteral("dCap"),
                                QStringLiteral("tCap"),
                                QStringLiteral("generic"),
                                QStringLiteral("turn_end"),
                                QString(1024 * 1024, QLatin1Char('a')))
                      + eventLine("idle", QStringLiteral("dCap"),
                                  QStringLiteral("tCap")));
    QVERIFY(preloaded.data().size() > 1024 * 1024);
    QVERIFY(preloaded.open(QIODevice::ReadOnly));

    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);
    m_monitor->setTransport(&preloaded);

    // Only the small line was applied; the over-cap one left no trace, and
    // framing resumed at its newline rather than swallowing what followed.
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(2).toInt(), asInt(AgentState::Idle));
    QCOMPARE(m_monitor->stateFor("dCap", "tCap"), asInt(AgentState::Idle));

    m_monitor->setTransport(nullptr);
}

// Error is a state, not a latch. Nothing in the monitor keeps a Dev Session red
// once the agent reports it is working again, and no explicit reset is needed
// to leave Error — the next event simply replaces it. Error also never notifies:
// the notification hook fires for waiting_input and idle_unseen only, the state
// names SPEC 6.4 "Internal Event Schema" defines (the notification display layer
// itself has no section of its own and is a Phase 4 deliverable in SPEC 16).
// Error is not a completion either, so it must not arm the unseen badge.
void TstAgentMonitor::errorStateIsNotSticky()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);
    QSignalSpy notifySpy(m_monitor, &AgentStatusMonitor::notify);

    feed(eventLine("running"));
    QTRY_COMPARE(stateSpy.count(), 1);

    feed(eventLine("error", QStringLiteral("d1"), QStringLiteral("t1"),
                   QStringLiteral("generic"), QStringLiteral("hook_failed"),
                   QStringLiteral("boom")));
    QTRY_COMPARE(stateSpy.count(), 2);
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::Error));
    QVERIFY(!m_monitor->hasUnseen("d1"));
    QCOMPARE(notifySpy.count(), 0);

    feed(eventLine("running"));
    QTRY_COMPARE(stateSpy.count(), 3);
    QCOMPARE(m_monitor->stateFor("d1", "t1"), asInt(AgentState::Running));

    // ...and the way OUT of Error is reachable for every following state too,
    // including the completion that arms the badge.
    feed(eventLine("idle_unseen"));
    QTRY_VERIFY(m_monitor->hasUnseen("d1"));
    QCOMPARE(notifySpy.count(), 1);
}

// Only the two attention-worthy states raise the desktop-notification hook. The
// six quiet states all record a transition and all stay silent: a coding agent
// that emits a dozen lifecycle events per turn must not turn into a dozen
// desktop bubbles.
void TstAgentMonitor::onlyAttentionStatesNotify()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);
    QSignalSpy notifySpy(m_monitor, &AgentStatusMonitor::notify);

    for (const char* quiet : {"starting", "running", "idle", "error", "stopped",
                              "unknown"})
        feed(eventLine(QString::fromLatin1(quiet)));

    QTRY_COMPARE(stateSpy.count(), 6);
    QTest::qWait(100);
    QCOMPARE(notifySpy.count(), 0);

    feed(eventLine("waiting_input"));
    QTRY_COMPARE(notifySpy.count(), 1);
    QCOMPARE(notifySpy.at(0).at(0).toString(),
             QStringLiteral("Agent waiting for input"));

    feed(eventLine("idle_unseen"));
    QTRY_COMPARE(notifySpy.count(), 2);
    QCOMPARE(notifySpy.at(1).at(0).toString(), QStringLiteral("Agent finished"));
}

// The notification body prefers the producer's summary, but a harness that
// supplies none — or supplies an empty one — must still yield a body that names
// the terminal rather than an empty bubble.
void TstAgentMonitor::notificationBodyFallsBackToIdentifiers()
{
    makePair();
    QSignalSpy notifySpy(m_monitor, &AgentStatusMonitor::notify);

    // No `summary` key at all.
    feed(eventLine("waiting_input", QStringLiteral("dN"), QStringLiteral("tN")));
    QTRY_COMPARE(notifySpy.count(), 1);
    QCOMPARE(notifySpy.at(0).at(1).toString(), QStringLiteral("dN / tN"));

    // Present but empty: same fallback.
    feed(eventLine("idle_unseen", QStringLiteral("dN"), QStringLiteral("tN"),
                   QStringLiteral("generic"), QStringLiteral("turn_end"),
                   QStringLiteral("")));
    QTRY_COMPARE(notifySpy.count(), 2);
    QCOMPARE(notifySpy.at(1).at(1).toString(), QStringLiteral("dN / tN"));
}

// Sequencing comes from the single ordered byte stream, never from the
// timestamp field: those are wall-clock readings taken in separate short-lived
// hook processes, so they are neither monotonic nor guaranteed distinct, and a
// clock step backwards would discard live state if it were used to reorder or
// drop. An event with an OLDER timestamp arriving later therefore wins, and a
// burst of identical events collapses to nothing extra.
void TstAgentMonitor::repeatedAndReorderedEventsAreAppliedInArrivalOrder()
{
    makePair();
    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);

    const auto stamped = [](const QString& state, const QString& ts) {
        QJsonObject o{{"version", 1},   {"timestamp", ts},
                      {"harness", "generic"}, {"devSessionId", "dO"},
                      {"terminalId", "tO"},   {"state", state},
                      {"event", "tick"}};
        return QByteArray(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
    };

    feed(stamped(QStringLiteral("running"),
                 QStringLiteral("2026-07-25T00:00:03.000Z")));
    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(m_monitor->stateFor("dO", "tO"), asInt(AgentState::Running));

    // Two seconds OLDER, but it arrived second, so it is the current state.
    feed(stamped(QStringLiteral("idle"),
                 QStringLiteral("2026-07-25T00:00:01.000Z")));
    QTRY_COMPARE(stateSpy.count(), 2);
    QCOMPARE(stateSpy.at(1).at(2).toInt(), asInt(AgentState::Idle));
    QCOMPARE(m_monitor->stateFor("dO", "tO"), asInt(AgentState::Idle));

    // Three duplicates delivered as ONE chunk: every line is framed and parsed,
    // and none of them is a transition, so nothing further is emitted.
    feed(stamped(QStringLiteral("idle"), QStringLiteral("a"))
         + stamped(QStringLiteral("idle"), QStringLiteral("b"))
         + stamped(QStringLiteral("idle"), QStringLiteral("c")));
    QTest::qWait(150);
    QCOMPARE(stateSpy.count(), 2);
    QCOMPARE(m_monitor->stateFor("dO", "tO"), asInt(AgentState::Idle));
}

// retainDevSessions() is the only eviction path, and it is deliberately silent:
// it runs right after the sidebar list has been rebuilt from the authoritative
// server tree, so every Dev Session it drops has already stopped having a row,
// and a change signal would name something nobody can display. Retaining
// exactly what is live must not disturb anything, an empty live set must clear
// everything, and eviction must leave no tombstone behind.
void TstAgentMonitor::retainDevSessionsIsSilentAndTotalWhenNothingIsLive()
{
    makePair();
    feed(eventLine("idle_unseen", QStringLiteral("dR"), QStringLiteral("tR")));
    QTRY_VERIFY(m_monitor->hasUnseen("dR"));

    QSignalSpy stateSpy(m_monitor, &AgentStatusMonitor::agentStateChanged);
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);

    m_monitor->retainDevSessions({QStringLiteral("dR")});
    QCOMPARE(m_monitor->stateFor("dR", "tR"), asInt(AgentState::IdleUnseen));
    QVERIFY(m_monitor->hasUnseen("dR"));

    m_monitor->retainDevSessions({});
    QCOMPARE(m_monitor->stateFor("dR", "tR"), asInt(AgentState::Unknown));
    QVERIFY(!m_monitor->hasUnseen("dR"));
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(unseenSpy.count(), 0);

    // No tombstone: the same id reporting again is tracked from scratch and can
    // re-arm its badge.
    feed(eventLine("idle_unseen", QStringLiteral("dR"), QStringLiteral("tR")));
    QTRY_COMPARE(unseenSpy.count(), 1);
    QCOMPARE(stateSpy.count(), 1);
    QVERIFY(m_monitor->hasUnseen("dR"));
}

// markSeen() reports the unseen flag FLIPPING, not the user looking. A Dev
// Session the monitor has never heard of, and one it knows but that has no
// pending completion, must both be silent no-ops — otherwise every sidebar
// selection change would emit a signal and re-derive every row.
void TstAgentMonitor::markSeenForAnUnknownDevSessionIsSilent()
{
    makePair();
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);

    m_monitor->markSeen(QStringLiteral("never-heard-of-it"));
    m_monitor->markSeen(QString());
    QCOMPARE(unseenSpy.count(), 0);
    QVERIFY(!m_monitor->hasUnseen("never-heard-of-it"));

    feed(eventLine("running", QStringLiteral("dM"), QStringLiteral("tM")));
    QTRY_COMPARE(m_monitor->stateFor("dM", "tM"), asInt(AgentState::Running));
    m_monitor->markSeen(QStringLiteral("dM"));
    QCOMPARE(unseenSpy.count(), 0);
    QCOMPARE(m_monitor->stateFor("dM", "tM"), asInt(AgentState::Running));
}

// main.cpp constructs the monitor and hands it to AppController and the
// Notifier long before the SSH channel that feeds it exists, and SessionBootstrap
// unbinds it again on every disconnect. Every query and command must therefore
// be safe with no transport at all.
void TstAgentMonitor::monitorWithoutATransportIsInert()
{
    QCOMPARE(m_monitor->transport(), static_cast<QIODevice*>(nullptr));
    QSignalSpy unseenSpy(m_monitor, &AgentStatusMonitor::unseenChanged);

    QCOMPARE(m_monitor->stateFor("d", "t"), asInt(AgentState::Unknown));
    QVERIFY(!m_monitor->hasUnseen("d"));
    m_monitor->markSeen(QStringLiteral("d"));
    m_monitor->retainDevSessions({});
    QCOMPARE(unseenSpy.count(), 0);

    // Unbinding what was never bound is a no-op, not a crash.
    m_monitor->setTransport(nullptr);
    QCOMPARE(m_monitor->transport(), static_cast<QIODevice*>(nullptr));
}

QTEST_MAIN(TstAgentMonitor)
#include "tst_agentmonitor.moc"
