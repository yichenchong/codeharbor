#include "CodeharbordClient.h"

#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#include <limits>

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
    // Assign a monotonically increasing id, wrapping to 1 before the increment
    // would overflow INT_MAX (signed overflow is undefined behaviour). A wrap
    // can only collide with a still-pending id after ~2^31 requests are
    // outstanding at once, which the pending map never realistically reaches.
    const int id = m_nextId;
    m_nextId = (m_nextId == std::numeric_limits<int>::max()) ? 1 : m_nextId + 1;

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
        if (line.endsWith('\r'))
            line.chop(1); // tolerate CRLF framing (SPEC 10.3 is newline-delimited)
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
    // Responses this client can route always carry the integer id it issued;
    // the server echoes it verbatim. A missing/null/string id, or a fractional
    // or out-of-range number, is unroutable (JSON-RPC 2.0 section 5).
    if (!idValue.isDouble()) {
        emit protocolWarning(QStringLiteral("RPC message with no routable id"));
        return;
    }
    const double idNum = idValue.toDouble();
    // Range-check before the float->int cast (out-of-range double->int is UB),
    // then reject fractional ids so a bogus 1.5 can't truncate onto pending #1.
    if (idNum < static_cast<double>(std::numeric_limits<int>::min()) ||
        idNum > static_cast<double>(std::numeric_limits<int>::max())) {
        emit protocolWarning(QStringLiteral("RPC response with out-of-range id"));
        return;
    }
    const int id = static_cast<int>(idNum);
    if (static_cast<double>(id) != idNum) {
        emit protocolWarning(QStringLiteral("RPC response with non-integral id"));
        return;
    }

    auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        // Unknown or duplicate (already-dispatched) id — ignore with a warning.
        emit protocolWarning(
            QStringLiteral("response for unknown/duplicate id %1").arg(id));
        return;
    }

    // Copy the callback out and erase the pending entry BEFORE invoking it, so a
    // callback that re-enters call() (or otherwise mutates m_pending) never sees
    // a half-cleared map or a dangling iterator.
    const ResponseCallback cb = it.value();
    m_pending.erase(it);

    // JSON-RPC 2.0 section 5: a response carries exactly one of result/error.
    // `error` counts as present only when it is a non-null value: servers
    // commonly spell a successful response {"result":…,"error":null}, and
    // treating that null as a failure would mis-report every such success. A
    // null `result`, by contrast, is a legitimate successful value, so `result`
    // is detected with contains().
    const QJsonValue errValue = obj.value(QStringLiteral("error"));
    const bool hasError = !errValue.isUndefined() && !errValue.isNull();
    const bool hasResult = obj.contains(QStringLiteral("result"));

    if (hasError) {
        if (hasResult) {
            emit protocolWarning(
                QStringLiteral("response %1 carries both result and error; "
                               "treating as error").arg(id));
        }
        RpcError error;
        if (errValue.isObject()) {
            const QJsonObject errObj = errValue.toObject();
            // A well-formed error object has an integer code and string message
            // (JSON-RPC 2.0 section 5.1). Surface the violation but still fail
            // the callback with best-effort fields so the caller cannot hang.
            if (!errObj.value(QStringLiteral("code")).isDouble() ||
                !errObj.value(QStringLiteral("message")).isString()) {
                emit protocolWarning(
                    QStringLiteral("response %1 error object missing code/message")
                        .arg(id));
            }
            error.code = errObj.value(QStringLiteral("code")).toInt();
            error.message = errObj.value(QStringLiteral("message")).toString();
            error.data = errObj.value(QStringLiteral("data"));
        } else {
            // `error` present but not an object at all (e.g. a bare string).
            emit protocolWarning(
                QStringLiteral("response %1 error is not an object").arg(id));
            error.code = kInternalError;
            error.message = QStringLiteral("malformed error: not an object");
        }
        cb(QJsonValue(), error);
        return;
    }

    if (!hasResult) {
        // Neither field present: malformed. Fail the pending callback so the
        // caller cannot hang, and flag the violation.
        emit protocolWarning(
            QStringLiteral("response %1 carries neither result nor error").arg(id));
        RpcError error;
        error.code = kInternalError;
        error.message =
            QStringLiteral("malformed response: neither result nor error");
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
