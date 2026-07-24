#include "CodeharbordClient.h"

#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

namespace ch {

namespace {

// JSON-RPC 2.0 reserved code for an internal/transport error. Reused for the
// synthetic failure delivered to pending callbacks when the transport dies.
constexpr int kInternalError = -32603;

} // namespace

CodeharbordClient::CodeharbordClient(QObject* parent) : QObject(parent) {}

void CodeharbordClient::setTransport(QIODevice* transport)
{
    if (m_transport == transport)
        return;

    if (m_transport)
        m_transport->disconnect(this);

    m_transport = transport;
    m_readBuffer.clear();
    m_closed = false;

    if (!m_transport)
        return;

    connect(m_transport, &QIODevice::readyRead, this, &CodeharbordClient::onReadyRead);
    // EOF on the read channel: covers QProcess stdout close and socket shutdown.
    connect(m_transport, &QIODevice::readChannelFinished, this,
            &CodeharbordClient::onTransportClosed);
    // Sockets (QLocalSocket/QAbstractSocket) also emit disconnected(); wire it
    // dynamically so the client stays transport-agnostic without a hard link
    // against the socket classes.
    if (m_transport->metaObject()->indexOfSignal("disconnected()") >= 0) {
        connect(m_transport, SIGNAL(disconnected()), this, SLOT(onTransportClosed()));
    }

    // Drain anything already buffered on the transport before we subscribed.
    if (m_transport->bytesAvailable() > 0)
        onReadyRead();
}

int CodeharbordClient::call(const QString& method, const QJsonValue& params,
                            ResponseCallback cb)
{
    const int id = m_nextId++;

    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("id"), id);
    request.insert(QStringLiteral("method"), method);
    if (!params.isUndefined() && !params.isNull())
        request.insert(QStringLiteral("params"), params);

    m_pending.insert(id, std::move(cb));

    if (!m_transport || !m_transport->isWritable()) {
        emit protocolWarning(
            QStringLiteral("call(%1): no writable transport").arg(method));
        // The pending entry will be failed by onTransportClosed(), or the caller
        // may retry after binding a transport; leave it registered.
        return id;
    }

    QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
    line.append('\n');
    m_transport->write(line);
    return id;
}

void CodeharbordClient::onReadyRead()
{
    if (!m_transport)
        return;

    m_readBuffer.append(m_transport->readAll());

    // Consume every complete line; a trailing partial line stays buffered until
    // the rest arrives on a later readyRead (partial-line-mid-JSON handling).
    int newline;
    while ((newline = m_readBuffer.indexOf('\n')) != -1) {
        QByteArray line = m_readBuffer.left(newline);
        m_readBuffer.remove(0, newline + 1);
        if (line.trimmed().isEmpty())
            continue;
        processLine(line);
    }
}

void CodeharbordClient::processLine(const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit protocolWarning(
            QStringLiteral("malformed RPC message: %1").arg(parseError.errorString()));
        return;
    }

    const QJsonObject obj = doc.object();

    // Server -> client notification: has a method, carries no id (SPEC 8.7).
    if (obj.contains(QStringLiteral("method"))) {
        emit notificationReceived(obj.value(QStringLiteral("method")).toString(),
                                  obj.value(QStringLiteral("params")));
        return;
    }

    const QJsonValue idValue = obj.value(QStringLiteral("id"));
    if (!idValue.isDouble()) {
        emit protocolWarning(QStringLiteral("RPC message with no routable id"));
        return;
    }

    const int id = idValue.toInt();
    auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        // Unknown or duplicate (already-dispatched) id — ignore with a warning.
        emit protocolWarning(
            QStringLiteral("response for unknown/duplicate id %1").arg(id));
        return;
    }

    const ResponseCallback cb = it.value();
    m_pending.erase(it);

    if (obj.contains(QStringLiteral("error"))) {
        const QJsonObject errObj = obj.value(QStringLiteral("error")).toObject();
        RpcError error;
        error.code = errObj.value(QStringLiteral("code")).toInt();
        error.message = errObj.value(QStringLiteral("message")).toString();
        error.data = errObj.value(QStringLiteral("data"));
        cb(QJsonValue(), error);
        return;
    }

    cb(obj.value(QStringLiteral("result")), std::nullopt);
}

void CodeharbordClient::onTransportClosed()
{
    if (m_closed)
        return;
    m_closed = true;

    RpcError error;
    error.code = kInternalError;
    error.message = QStringLiteral("transport closed with request pending");
    failAllPending(error);

    emit transportClosed();
}

void CodeharbordClient::failAllPending(const RpcError& error)
{
    // Move out first: a callback may re-enter (e.g. issue a fresh call) and must
    // not observe a half-cleared map.
    const QHash<int, ResponseCallback> pending = std::move(m_pending);
    m_pending.clear();
    for (const ResponseCallback& cb : pending)
        cb(QJsonValue(), error);
}

QString CodeharbordClient::launchCommand()
{
    return QStringLiteral("codeharbord rpc --stdio");
}

} // namespace ch
