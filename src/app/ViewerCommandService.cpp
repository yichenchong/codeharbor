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
        // Commands belong to the daemon on this transport. Retire them before
        // a replacement can deliver commands, not on post-drain transportBound.
        connect(m_client, &CodeharbordClient::transportChanging, this,
                [this](quint64 generation) {
                    // An earlier slot may rebind and drain a newer peer before
                    // this older signal invocation reaches us.
                    if (m_client
                        && generation == m_client->transportGeneration())
                        m_inFlight.clear();
                });
        connect(m_client, &CodeharbordClient::transportClosed, this,
                [this](quint64 generation) {
                    if (m_client
                        && generation == m_client->transportGeneration())
                        m_inFlight.clear();
                });
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
    // A repeated notification must not drive the layout twice or settle the
    // original request with a validation/busy refusal while it is still running.
    if (m_inFlight.contains(commandId))
        return;

    QString devSessionId;
    if (!isUsableId(o.value(QStringLiteral("devSessionId")), &devSessionId)) {
        sendResponse(commandId, false, QStringLiteral("bad_request"),
                     QStringLiteral("the command named no Dev Session"), {});
        return;
    }
    // terminalId is provenance only — which pane's agent asked — but it is still
    // required, so a producer that cannot name its own pane is refused rather
    // than allowed to drive the layout anonymously.
    QString terminalId;
    if (!isUsableId(o.value(QStringLiteral("terminalId")), &terminalId)) {
        sendResponse(commandId, false, QStringLiteral("bad_request"),
                     QStringLiteral("the command named no terminal"), {});
        return;
    }

    const QJsonValue opValue = o.value(QStringLiteral("op"));
    if (!opValue.isString() || !isKnownOp(opValue.toString())) {
        sendResponse(commandId, false, QStringLiteral("bad_request"),
                     QStringLiteral("this CodeHarbor does not implement that viewer operation"),
                     {});
        return;
    }
    const QJsonValue argsValue = o.value(QStringLiteral("args"));
    if (!argsValue.isUndefined() && !argsValue.isObject()) {
        sendResponse(commandId, false, QStringLiteral("bad_request"),
                     QStringLiteral("args was not a JSON object"), {});
        return;
    }

    // Boundary refusals do not announce an in-flight command. Send them
    // directly; respond() is reserved for the handler's first answer.
    if (m_inFlight.size() >= kMaxInFlight) {
        sendResponse(commandId, false, QStringLiteral("busy"),
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
    if (!m_inFlight.remove(commandId))
        return;
    sendResponse(commandId, ok, errorCode, message, data);
}

void ViewerCommandService::sendResponse(const QString& commandId, bool ok,
                                        const QString& errorCode,
                                        const QString& message,
                                        const QVariantMap& data)
{
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
