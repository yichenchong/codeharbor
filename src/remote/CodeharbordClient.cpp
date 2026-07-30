#include "CodeharbordClient.h"

#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>

#include <limits>

namespace ch {

namespace {

// JSON-RPC 2.0 reserved code for an internal/transport error. Reused for the
// synthetic failure delivered to pending callbacks when the transport dies.
constexpr int kInternalError = -32603;

// Hard cap on a single unframed line. A well-behaved server delimits every
// message with '\n'; a peer that streams megabytes without one is malformed and
// must not grow m_readBuffer without bound. Set above the largest legitimate
// frame: the internal scheme handler bounds inline file reads to 8 MiB raw
// (~11 MiB base64), so 16 MiB leaves headroom for that plus JSON overhead.
constexpr int kMaxLineBytes = 16 * 1024 * 1024;

// True when `line` holds nothing but ASCII whitespace, i.e. it is a separator
// rather than a frame. Deliberately allocation free: QByteArray::trimmed() would
// copy the WHOLE line — megabytes for an inline file read — just to answer "is
// this blank?". '\n' cannot appear (the caller split on it) and a trailing '\r'
// has already been chopped, but both are listed for completeness.
bool isBlankLine(const QByteArray& line)
{
    for (const char c : line) {
        if (c != ' ' && c != '\t' && c != '\v' && c != '\f' && c != '\r' &&
            c != '\n')
            return false;
    }
    return true;
}

} // namespace

CodeharbordClient::CodeharbordClient(QObject* parent) : QObject(parent) {}

CodeharbordClient::~CodeharbordClient()
{
    // The transport hooks (write failure, swap, close) already sweep m_pending,
    // but none of them covers the client itself going away with requests still
    // outstanding: the QHash would just be destroyed and those callbacks would
    // never run. A caller that allocated state for its callback to release, or
    // that waits for the callback before finishing an operation, would then leak
    // or hang forever — exactly what call()'s "no callback is ever registered
    // that cannot fire" contract promises cannot happen. So fail them here,
    // mirroring failAllPending()'s synthetic-error path.
    //
    // Two rules keep dispatching from a destructor sound:
    //  * Latch closed and drop the transport FIRST. A callback whose usual
    //    reaction is to retry then takes call()'s "transport closed" branch,
    //    which fails it synchronously and registers nothing — a fresh pending
    //    entry on a dying object could never fire. Disconnecting also stops a
    //    late readyRead()/disconnected() from landing in a half-destroyed slot.
    //  * Touch no member after the dispatch loop. failAllPending() moves the map
    //    out before iterating, so re-entry cannot observe a half-cleared map, and
    //    there is nothing left to do once it returns.
    m_closed = true;
    if (m_transport) {
        m_transport->disconnect(this);
        m_transport = nullptr;
    }

    if (m_pending.isEmpty())
        return;

    RpcError error;
    error.code = kInternalError;
    error.message = QStringLiteral("client destroyed with request pending");
    failAllPending(error);
}

void CodeharbordClient::setTransport(QIODevice* transport)
{
    if (m_transport == transport)
        return;

    if (m_transport)
        m_transport->disconnect(this);

    m_transport = transport;
    m_readBuffer.clear();
    m_closed = false;

    if (m_transport) {
        connect(m_transport, &QIODevice::readyRead, this,
                &CodeharbordClient::onReadyRead);
        // EOF on the read channel: covers QProcess stdout close and socket
        // shutdown.
        connect(m_transport, &QIODevice::readChannelFinished, this,
                &CodeharbordClient::onTransportClosed);
        // Sockets (QLocalSocket/QAbstractSocket) also emit disconnected(); wire
        // it dynamically so the client stays transport-agnostic without a hard
        // link against the socket classes.
        if (m_transport->metaObject()->indexOfSignal("disconnected()") >= 0) {
            connect(m_transport, SIGNAL(disconnected()), this,
                    SLOT(onTransportClosed()));
        }
    }

    // Any callback invoked below may delete this client; stop touching members
    // the instant one does.
    QPointer<CodeharbordClient> self(this);

    // Fail every request still in flight on the transport we just let go of.
    // The ids we minted are meaningful ONLY to that peer: a reconnect (SPEC 5.6)
    // hands us a DIFFERENT `codeharbord` process whose request table has never
    // heard of them, and a detach leaves nobody to answer at all. Carrying them
    // across the swap does not keep them alive, it hangs their callers forever
    // and breaks call()'s exactly-once guarantee.
    //
    // Swept AFTER the rewire and after m_closed was cleared, so a callback that
    // immediately retries — the usual response to a transport error — writes
    // onto the NEW transport instead of being rejected as "transport closed".
    // failAllPending() moves the map out first, so that retry's fresh pending
    // entry survives the sweep.
    if (!m_pending.isEmpty()) {
        RpcError error;
        error.code = kInternalError;
        error.message = QStringLiteral("transport replaced with request pending");
        failAllPending(error);
        if (!self)
            return;
    }

    if (!m_transport)
        return;

    // Drain anything already buffered on the transport before we subscribed.
    if (m_transport->bytesAvailable() > 0) {
        onReadyRead();
        if (!self)
            return;
    }

    // Announce LAST: a consumer re-establishing server-side state (a
    // file.watch subscription, say) must find the client fully bound and
    // drained so its very first call goes out on the new transport.
    emit transportBound();
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

    // Decide whether we can actually transmit BEFORE registering the pending
    // callback. If we cannot, the callback would otherwise be orphaned forever
    // (leak + caller hangs, never learning it failed). Instead we fail it once
    // with a synthetic transport error and register nothing. The single
    // acceptance rule: call() either (a) writes the request and registers the
    // pending callback, or (b) invokes the callback exactly once with an error.
    QString failReason;
    if (m_closed)
        failReason = QStringLiteral("transport closed");
    else if (!m_transport)
        failReason = QStringLiteral("no transport bound");
    else if (!m_transport->isOpen() || !m_transport->isWritable())
        failReason = QStringLiteral("transport not writable");

    // Set when a SHORT — but non-empty — write already put a TRUNCATED frame on
    // the wire. QIODevice::write() is allowed to report fewer bytes than asked
    // for, and SshChannelDevice::writeData() really does return a short count
    // when ssh_channel_write() stops making progress. The peer would then glue
    // our next request onto that fragment, so the byte stream is desynchronised
    // beyond repair and the transport has to go rather than keep emitting
    // garbage frames that the server answers with parse errors.
    bool framingLost = false;

    if (failReason.isEmpty()) {
        QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
        line.append('\n');
        const qint64 written = m_transport->write(line);
        if (written != line.size()) {
            failReason = QStringLiteral("transport write failed");
            framingLost = written > 0;
        }
    }

    if (!failReason.isEmpty()) {
        emit protocolWarning(
            QStringLiteral("call(%1): %2").arg(method, failReason));
        RpcError error;
        error.code = kInternalError;
        error.message = QStringLiteral("call failed: %1").arg(failReason);
        // A half-written frame kills the stream for everyone, so latch the
        // transport closed first: that fails every OTHER pending caller exactly
        // once. Nothing after this point touches a member, so a callback that
        // deletes this client from inside the teardown is safe.
        if (framingLost)
            onTransportClosed();
        // Deliver the failure exactly once and register nothing. The callback
        // may delete this client, so touch no members after invoking it; `id`
        // is a local copy and is safe to return.
        if (cb)
            cb(QJsonValue(), error);
        return id;
    }

    m_pending.insert(id, std::move(cb));
    return id;
}

void CodeharbordClient::onReadyRead()
{
    if (!m_transport || m_closed)
        return;

    m_readBuffer.append(m_transport->readAll());

    // processLine() invokes a user callback that may delete this client. Watch
    // for that with a QPointer and stop touching members the instant it fires,
    // otherwise the loop's next m_readBuffer access is a use-after-free.
    QPointer<CodeharbordClient> self(this);

    // Consume every complete line; a trailing partial line stays buffered until
    // the rest arrives on a later readyRead (partial-line-mid-JSON handling).
    int newline;
    while ((newline = m_readBuffer.indexOf('\n')) != -1) {
        QByteArray line = m_readBuffer.left(newline);
        m_readBuffer.remove(0, newline + 1);
        if (line.endsWith('\r'))
            line.chop(1); // tolerate CRLF framing (SPEC 10.3 is newline-delimited)
        if (isBlankLine(line))
            continue;
        processLine(line);
        if (!self)
            return; // a callback deleted us; touch no members
    }

    // Guard against an unterminated line growing the buffer without bound: a
    // peer streaming megabytes with no '\n' is malformed. Drop the garbage and
    // declare the stream unusable — that fails every pending caller once and
    // emits transportClosed(). The transport object itself is left alone (it
    // belongs to the caller and may still be physically healthy); what is gone
    // is our ability to trust a single byte on it, so nothing more is read.
    if (m_readBuffer.size() > kMaxLineBytes) {
        emit protocolWarning(
            QStringLiteral("RPC line exceeded %1 bytes without a newline; "
                           "resetting transport").arg(kMaxLineBytes));
        m_readBuffer.clear();
        onTransportClosed();
    }
}

void CodeharbordClient::processLine(const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        emit protocolWarning(
            QStringLiteral("malformed RPC message: %1").arg(parseError.errorString()));
        return;
    }
    if (!doc.isObject()) {
        // Well-formed JSON that is not an object: a JSON-RPC 2.0 batch array, or
        // a bare scalar. Unroutable either way. Reported separately because
        // parseError.errorString() would read "no error" here, which is useless.
        emit protocolWarning(QStringLiteral("RPC message is not a JSON object"));
        return;
    }

    const QJsonObject obj = doc.object();
    const QJsonValue idValue = obj.value(QStringLiteral("id"));

    // Server -> client notification: carries a method and NO id (JSON-RPC 2.0
    // section 4.1), e.g. file.watchEvent (SPEC 8.7). Checking the id is what
    // keeps this branch honest. A message carrying BOTH a method and an id is a
    // REQUEST aimed at us, which this client does not implement; and a malformed
    // RESPONSE that echoed `method` back would, without the id check, be
    // swallowed as a notification and leave its caller hanging forever instead
    // of being failed below.
    const QJsonValue methodValue = obj.value(QStringLiteral("method"));
    if (methodValue.isString() && (idValue.isUndefined() || idValue.isNull())) {
        emit notificationReceived(methodValue.toString(),
                                  obj.value(QStringLiteral("params")));
        return;
    }

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
    //
    // `cb` may be EMPTY: call() accepts a null callback for a fire-and-forget
    // request, so every invocation below is guarded. Invoking an empty
    // std::function would throw std::bad_function_call out of a Qt slot.
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
        if (cb)
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
        if (cb)
            cb(QJsonValue(), error);
        return;
    }

    if (cb)
        cb(obj.value(QStringLiteral("result")), std::nullopt);
}

void CodeharbordClient::onTransportClosed()
{
    if (m_closed)
        return;

    // Last-gasp drain, BEFORE latching closed. A peer that wrote complete frames
    // and then shut down leaves those bytes readable on the transport, and Qt
    // may deliver readChannelFinished()/disconnected() with them still
    // unconsumed. Routing them here means a response that physically arrived is
    // DELIVERED rather than reported as "transport closed with request pending".
    QPointer<CodeharbordClient> self(this);
    if (m_transport && m_transport->bytesAvailable() > 0) {
        onReadyRead();
        if (!self)
            return; // a callback deleted us mid-drain
        // A callback may have re-entered here (or the drain may have tripped the
        // unframed-line cap), which already latched and announced the close.
        // Without this second check the announcement would go out twice.
        if (m_closed)
            return;
    }
    m_closed = true;

    RpcError error;
    error.code = kInternalError;
    error.message = QStringLiteral("transport closed with request pending");
    // A failed callback may delete this client during failAllPending(); guard
    // the trailing emit so it never touches a destroyed object. m_closed was
    // latched above, so any reentrant onTransportClosed() already returned.
    failAllPending(error);
    if (!self)
        return;

    emit transportClosed();
}

void CodeharbordClient::failAllPending(const RpcError& error)
{
    // Move out first: a callback may re-enter (e.g. issue a fresh call) and must
    // not observe a half-cleared map. clear() is not redundant — a moved-from Qt
    // container is left in a valid but unspecified state and must be reset
    // before it is used again.
    const QHash<int, ResponseCallback> pending = std::move(m_pending);
    m_pending.clear();
    for (const ResponseCallback& cb : pending) {
        if (cb) // empty for a fire-and-forget call() with a null callback
            cb(QJsonValue(), error);
    }
}

QString CodeharbordClient::launchCommand()
{
    return QStringLiteral("codeharbord rpc --stdio");
}

} // namespace ch
