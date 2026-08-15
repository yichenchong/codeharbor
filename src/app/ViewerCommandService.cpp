#include "ViewerCommandService.h"

#include "RpcTypes.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QStringList>

namespace ch {
namespace {

// The operations Main.qml implements. Kept here rather than in QML so an op the
// handler has never heard of is refused at the boundary with a NAMED reason,
// instead of falling through a QML switch and leaving the agent to wait out the
// daemon's timeout for an answer nobody was going to send.
//
// It is a second copy of control.ts's CONTROL_OPS on purpose: the daemon may be
// newer than this client, and version skew must produce a refusal here rather
// than an unhandled command.
bool isKnownOp(const QString& op)
{
    return op == QLatin1String("list") || op == QLatin1String("open")
        || op == QLatin1String("close") || op == QLatin1String("split")
        || op == QLatin1String("focus") || op == QLatin1String("reload");
}

bool isUsableId(const QJsonValue& value, QString* out)
{
    if (!value.isString())
        return false;
    const QString text = value.toString();
    if (text.trimmed().isEmpty())
        return false;
    *out = text;
    return true;
}

} // namespace

ViewerCommandService::ViewerCommandService(CodeharbordClient* client, QObject* parent)
    : QObject(parent), m_client(client)
{
    if (m_client) {
        connect(m_client, &CodeharbordClient::notificationReceived, this,
                &ViewerCommandService::onNotification);
    }
}

void ViewerCommandService::onNotification(const QString& method, const QJsonValue& params)
{
    if (method != QLatin1String(rpc::kNotificationViewerCommand))
        return;
    // A malformed notification is DROPPED silently, the same posture
    // parseAgentEventLine takes: the producer is a remote process, and a broken
    // one must not put a failure in front of a user who did nothing wrong. There
    // is also nobody to answer — without a usable commandId there is no id to
    // report against.
    if (!params.isObject())
        return;
    const QJsonObject o = params.toObject();

    QString commandId;
    if (!isUsableId(o.value(QStringLiteral("commandId")), &commandId))
        return;

    QString devSessionId;
    if (!isUsableId(o.value(QStringLiteral("devSessionId")), &devSessionId)) {
        respond(commandId, false, QStringLiteral("bad_request"),
                QStringLiteral("the command named no Dev Session"), {});
        return;
    }
    // terminalId is provenance only — which pane's agent asked — but it is still
    // required, so a producer that cannot name its own pane is refused rather
    // than allowed to drive the layout anonymously.
    QString terminalId;
    if (!isUsableId(o.value(QStringLiteral("terminalId")), &terminalId)) {
        respond(commandId, false, QStringLiteral("bad_request"),
                QStringLiteral("the command named no terminal"), {});
        return;
    }

    const QJsonValue opValue = o.value(QStringLiteral("op"));
    if (!opValue.isString() || !isKnownOp(opValue.toString())) {
        respond(commandId, false, QStringLiteral("bad_request"),
                QStringLiteral("this CodeHarbor does not implement that viewer operation"),
                {});
        return;
    }
    const QJsonValue argsValue = o.value(QStringLiteral("args"));
    if (!argsValue.isUndefined() && !argsValue.isObject()) {
        respond(commandId, false, QStringLiteral("bad_request"),
                QStringLiteral("args was not a JSON object"), {});
        return;
    }

    // The refusals above answer WITHOUT registering the id, which is why respond()
    // tolerates an unknown one: they are the one legitimate case of answering a
    // command that was never announced.
    if (m_inFlight.size() >= kMaxInFlight) {
        respond(commandId, false, QStringLiteral("busy"),
                QStringLiteral("CodeHarbor is already working through %1 viewer commands")
                    .arg(kMaxInFlight),
                {});
        return;
    }

    m_inFlight.insert(commandId);
    emit commandRequested(commandId, devSessionId, opValue.toString(),
                          argsValue.toObject().toVariantMap());
}

void ViewerCommandService::respond(const QString& commandId, bool ok,
                                   const QString& errorCode, const QString& message,
                                   const QVariantMap& data)
{
    if (commandId.trimmed().isEmpty())
        return;
    // remove() answers whether the id was there. An id that was never announced
    // is still answered on the wire — the validation refusals above rely on that
    // — but a SECOND answer to an announced one is dropped, so a QML handler that
    // falls through two arms cannot report an outcome twice.
    if (!m_inFlight.remove(commandId) && ok) {
        // A success for an id nobody is tracking can only be a double-report:
        // every refusal path here is a failure. Dropping it keeps the first,
        // truthful answer.
        return;
    }
    if (!m_client)
        return;

    QJsonObject params;
    params.insert(QStringLiteral("commandId"), commandId);
    params.insert(QStringLiteral("ok"), ok);
    if (!ok) {
        QJsonObject error;
        // The daemon maps an unrecognized code to "failed"; sending the empty
        // string would be that, said less clearly.
        error.insert(QStringLiteral("code"),
                     errorCode.isEmpty() ? QStringLiteral("failed") : errorCode);
        error.insert(QStringLiteral("message"),
                     message.isEmpty() ? QStringLiteral("the viewer command failed")
                                       : message);
        params.insert(QStringLiteral("error"), error);
    }
    if (!data.isEmpty())
        params.insert(QStringLiteral("data"), QJsonObject::fromVariantMap(data));

    // Fire-and-forget: the daemon's answer is `{ok:true}` in every case (an
    // expired id included), so there is nothing for a callback to decide.
    m_client->call(QString::fromLatin1(rpc::kMethodViewerCommandResult), params, nullptr);
}

} // namespace ch
