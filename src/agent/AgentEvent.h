#pragma once

#include "SessionState.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <QStringView>

#include <optional>

namespace ch {

// Wire schema version for the agent-status event stream (SPEC 6.4). Mirrors
// CH_EVENT_VERSION in remote/src/events.ts; bump both in lockstep.
constexpr int kAgentEventVersion = 1;

// C++ mirror of the AgentEvent interface in remote/src/events.ts. Carries the
// decoded, validated fields of one JSONL agent-status event. `state` is the
// parsed enum; `summary`/`metadata` are optional (empty when the producer
// omitted them).
struct AgentEvent {
    int version = 0;
    QString timestamp;
    AgentState state = AgentState::Unknown;
    QString harness;
    QString devSessionId;
    QString terminalId;
    QString event;
    QString summary;
    QJsonObject metadata;
};

// Map a wire state string (starting/running/waiting_input/idle_unseen/idle/
// error/stopped/unknown) to the ch::AgentState enum. Any unrecognized value —
// including the literal "unknown" — collapses to AgentState::Unknown.
inline AgentState agentStateFromWire(QStringView s)
{
    if (s == u"starting") return AgentState::Starting;
    if (s == u"running") return AgentState::Running;
    if (s == u"waiting_input") return AgentState::WaitingInput;
    if (s == u"idle_unseen") return AgentState::IdleUnseen;
    if (s == u"idle") return AgentState::Idle;
    if (s == u"error") return AgentState::Error;
    if (s == u"stopped") return AgentState::Stopped;
    if (s == u"unknown") return AgentState::Unknown;
    return AgentState::Unknown;
}

namespace detail {

// Membership test mirroring isAgentState() in events.ts: only the eight wire
// tokens are valid. Distinct from agentStateFromWire(), which cannot tell a
// genuine "unknown" from an unrecognized token (both map to Unknown) — parsing
// must reject the latter as malformed.
inline bool isAgentStateWire(QStringView s)
{
    return s == u"starting" || s == u"running" || s == u"waiting_input"
        || s == u"idle_unseen" || s == u"idle" || s == u"error"
        || s == u"stopped" || s == u"unknown";
}

// Membership test mirroring isHarness() in events.ts.
inline bool isHarnessWire(QStringView h)
{
    return h == u"generic" || h == u"oh-my-pi" || h == u"pi"
        || h == u"claude-code";
}

} // namespace detail

// Parse one JSONL line into a validated AgentEvent, or std::nullopt if the line
// is blank, malformed, or fails structural validation. Mirrors
// validateEvent()/parseEventLine() in remote/src/events.ts and, like them,
// NEVER throws: a broken producer must not take down the client (SPEC 6.4).
inline std::optional<AgentEvent> parseAgentEventLine(const QByteArray& line)
{
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return std::nullopt;

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    const QJsonObject o = doc.object();
    const QJsonValue vVersion = o.value(QStringLiteral("version"));
    const QJsonValue vTimestamp = o.value(QStringLiteral("timestamp"));
    const QJsonValue vHarness = o.value(QStringLiteral("harness"));
    const QJsonValue vDev = o.value(QStringLiteral("devSessionId"));
    const QJsonValue vTerm = o.value(QStringLiteral("terminalId"));
    const QJsonValue vState = o.value(QStringLiteral("state"));
    const QJsonValue vEvent = o.value(QStringLiteral("event"));
    const QJsonValue vSummary = o.value(QStringLiteral("summary"));
    const QJsonValue vMeta = o.value(QStringLiteral("metadata"));

    // version === CH_EVENT_VERSION (must be the JSON number 1, not "1"). Compare
    // as a double so a non-integer like 1.5 is rejected the way events.ts's
    // strict `===` rejects it (QJsonValue::toInt() would truncate 1.5 -> 1 and
    // wrongly accept it).
    if (!vVersion.isDouble()
        || vVersion.toDouble() != static_cast<double>(kAgentEventVersion))
        return std::nullopt;
    if (!vTimestamp.isString())
        return std::nullopt;
    if (!vHarness.isString() || !detail::isHarnessWire(vHarness.toString()))
        return std::nullopt;
    if (!vDev.isString())
        return std::nullopt;
    if (!vTerm.isString())
        return std::nullopt;
    if (!vState.isString() || !detail::isAgentStateWire(vState.toString()))
        return std::nullopt;
    if (!vEvent.isString())
        return std::nullopt;
    // summary: absent (undefined) or a string.
    if (!vSummary.isUndefined() && !vSummary.isString())
        return std::nullopt;
    // metadata: absent (undefined) or a JSON object. A JSON null or array is
    // rejected (our struct stores a QJsonObject).
    if (!vMeta.isUndefined() && !vMeta.isObject())
        return std::nullopt;

    AgentEvent ev;
    ev.version = vVersion.toInt();
    ev.timestamp = vTimestamp.toString();
    ev.state = agentStateFromWire(vState.toString());
    ev.harness = vHarness.toString();
    ev.devSessionId = vDev.toString();
    ev.terminalId = vTerm.toString();
    ev.event = vEvent.toString();
    if (vSummary.isString())
        ev.summary = vSummary.toString();
    if (vMeta.isObject())
        ev.metadata = vMeta.toObject();
    return ev;
}

} // namespace ch
