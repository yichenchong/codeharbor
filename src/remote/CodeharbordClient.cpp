#include "CodeharbordClient.h"

#include "RpcTypes.h"

#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

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

CodeharbordClient::CodeharbordClient(QObject* parent)
    : QObject(parent), m_heartbeatTimer(new QTimer(this))
{
    m_heartbeatTimer->setSingleShot(false);
    connect(m_heartbeatTimer, &QTimer::timeout, this,
            &CodeharbordClient::onHeartbeatTick);
}

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
    // Three rules keep dispatching from a destructor sound:
    //  * Latch DESTROYING and closed, and drop the transport FIRST. A callback
    //    whose usual reaction is to retry then takes call()'s failure branch,
    //    which fails it synchronously and registers nothing — a fresh pending
    //    entry on a dying object could never fire. Disconnecting also stops a
    //    late readyRead()/disconnected() from landing in a half-destroyed slot.
    //  * m_destroying, unlike m_closed, is one-way. setTransport() clears
    //    m_closed by design (that is how a reconnect revives the client), so a
    //    callback that reacts to the failure by driving a reconnect — plausible
    //    at shutdown, where AppController's error path can reach
    //    SessionBootstrap — would otherwise un-latch a client already inside its
    //    own destructor, wire fresh signal connections into a half-destroyed
    //    QObject, and start registering pending entries that can never fire.
    //  * Touch no member after the dispatch loop. failAllPending() moves the map
    //    out before iterating, so re-entry cannot observe a half-cleared map, and
    //    there is nothing left to do once it returns.
    m_destroying = true;
    m_closed = true;
    // Stop the probe timer before anything below can dispatch a callback. It is
    // a child QObject and would be deleted with us anyway, but a tick landing
    // in onHeartbeatTick() midway through this destructor would call() on a
    // dying client.
    m_heartbeatTimer->stop();
    // Retire the outstanding probe too: failAllPending() below runs its callback
    // like any other, and it must not touch live state on the way out.
    ++m_heartbeatGeneration;
    m_heartbeatProbeOutstanding = false;
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
    // This object is already inside ~CodeharbordClient, dispatching the failures
    // for the requests it is abandoning. Binding anything now would connect
    // signals into a half-destroyed QObject, clear the destructor's close latch,
    // and let the very callbacks being failed queue fresh requests that nothing
    // will ever service. The one-way m_destroying latch makes teardown final.
    if (m_destroying)
        return;

    // Rebinding the device already bound is normally a no-op. The exception is
    // a client that has latched CLOSED on it: a caller that reopens the same
    // QIODevice object rather than allocating a new one (nothing in the API
    // forbids it) would otherwise be left with a permanently dead client, every
    // later call() failing with "transport closed". Falling through re-runs the
    // full bind, which clears the latch — disconnect()+connect() below makes
    // re-wiring the same device harmless.
    if (m_transport == transport && !m_closed)
        return;

    if (m_transport)
        m_transport->disconnect(this);

    m_transport = transport;
    m_readBuffer.clear();
    m_scanOffset = 0;
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
        // The transport belongs to the CALLER, which may simply delete it while
        // requests are still in flight (a stack-allocated device going out of
        // scope is enough). Nothing else would ever tell us: QIODevice emits no
        // readChannelFinished() from its destructor, so every pending callback
        // would sit in the map until this client itself died. Treat the death
        // as the transport loss it is.
        connect(m_transport, &QObject::destroyed, this,
                &CodeharbordClient::onTransportDestroyed);
    }

    // Before the sweep below: the probe outstanding on the transport we just let
    // go is about to be failed like any other pending entry. restartHeartbeat()
    // retires it by bumping the generation stamp, so when that callback runs —
    // possibly LATE, after a nested event loop inside another swept callback has
    // already let the timer send the next probe — it recognises itself as stale
    // and does nothing. Resetting here also means the miss counter starts from
    // zero against the NEW peer rather than inheriting the silence of the one
    // that died.
    restartHeartbeat();

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

qint64 CodeharbordClient::call(const QString& method, const QJsonValue& params,
                            ResponseCallback cb)
{
    // Assign a monotonically increasing id, wrapping to 1 before the increment
    // would overflow the maximum signed 64-bit value (signed overflow is
    // undefined behaviour). At 64 bits a wrap can only collide with a
    // still-pending id after ~2^63 requests, which no connection reaches; the
    // pending map would exhaust memory long first.
    const qint64 id = m_nextId;
    m_nextId = (m_nextId == std::numeric_limits<qint64>::max()) ? 1 : m_nextId + 1;

    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("id"), QJsonValue(id));
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
    // RC13: never emit a frame the server would answer with a null-id error. An
    // unparseable line or a structurally invalid request carries no id to echo,
    // so this client could not route the resulting error to the caller that
    // provoked it — that caller would hang until the connection died. We mint
    // jsonrpc, id and the frame ourselves, so only the caller-supplied method
    // and params can be malformed; reject them here and fail the call through
    // the SAME protocolWarning + single-callback path as a dead transport,
    // writing nothing. This guarantees the client never provokes an
    // unattributable null-id server error, closing RC13 from the side we
    // control.
    if (request.value(QStringLiteral("jsonrpc")).toString() !=
        QLatin1String("2.0"))
        failReason = QStringLiteral("outgoing jsonrpc is not \"2.0\"");
    else if (!request.value(QStringLiteral("id")).isDouble())
        failReason = QStringLiteral("outgoing id is not an integer");
    else if (method.isEmpty())
        failReason = QStringLiteral("outgoing method is empty");
    else if (request.contains(QStringLiteral("params")) &&
             !request.value(QStringLiteral("params")).isObject() &&
             !request.value(QStringLiteral("params")).isArray())
        failReason =
            QStringLiteral("outgoing params is neither object nor array");
    else if (m_destroying)
        failReason = QStringLiteral("client is being destroyed");
    else if (m_closed)
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

    const qsizetype bufferedBefore = m_readBuffer.size();
    m_readBuffer.append(m_transport->readAll());
    // ANY bytes from the peer are proof of life, not just a ping reply. This is
    // one serialized JSONL stream, so while the peer is midway through writing a
    // multi-megabyte file.readFile frame its reply to our probe physically
    // cannot arrive — waiting for the reply alone would tear down exactly the
    // slow, healthy, large transfer the heartbeat is supposed to protect.
    if (m_readBuffer.size() != bufferedBefore)
        m_heartbeatMisses = 0;

    // processLine() invokes a user callback that may delete this client. Watch
    // for that with a QPointer and stop touching members the instant it fires,
    // otherwise the loop's next m_readBuffer access is a use-after-free.
    QPointer<CodeharbordClient> self(this);

    // Consume every complete line. m_scanOffset remembers how far we have
    // already searched for a '\n' and found none, so a single large message
    // arriving in many small chunks is not re-scanned from the front on every
    // readyRead (an O(n^2) trap otherwise). It indexes INTO m_readBuffer, so it
    // is reset to 0 the moment that buffer is mutated at the front or dropped —
    // see the resets below and in setTransport(). A trailing partial line stays
    // buffered until the rest arrives on a later readyRead.
    //
    // qsizetype, not int: QByteArray indexes are 64-bit and m_readBuffer holds
    // whatever one readAll() handed us, which is only bounded by the cap check
    // BELOW — narrowing the index here would corrupt the split of a buffer that
    // grew past 2 GiB before that check ever ran.
    qsizetype newline;
    while ((newline = m_readBuffer.indexOf('\n', m_scanOffset)) != -1) {
        QByteArray line = m_readBuffer.left(newline);
        m_readBuffer.remove(0, newline + 1);
        // The consumed prefix is gone, so the unscanned remainder starts at the
        // front again.
        m_scanOffset = 0;
        if (line.endsWith('\r'))
            line.chop(1); // tolerate CRLF framing (SPEC 10.3 is newline-delimited)
        if (isBlankLine(line))
            continue;
        processLine(line);
        if (!self)
            return; // a callback deleted us; touch no members
        // A callback (or a slot on one of the signals it provoked) may have
        // torn the transport down mid-chunk — SessionBootstrap really does
        // close the RPC channel from inside a response callback. Once the close
        // is latched every pending caller has already been failed and
        // transportClosed() announced, so the rest of this chunk belongs to a
        // connection that no longer exists: dispatching it would emit
        // notifications AFTER the close and warn about ids we just swept.
        if (m_closed) {
            m_readBuffer.clear();
            m_scanOffset = 0;
            return;
        }
    }
    // No '\n' past m_scanOffset: the whole buffer has now been searched, so the
    // next readyRead resumes from its end instead of from position zero.
    m_scanOffset = m_readBuffer.size();

    // Guard against an unterminated line growing the buffer without bound: a
    // peer streaming megabytes with no '\n' is malformed. Drop the garbage and
    // declare the stream unusable — that fails every pending caller once and
    // emits transportClosed(). The transport object itself is left alone (it
    // belongs to the caller and may still be physically healthy); what is gone
    // is our ability to trust a single byte on it, so nothing more is read.
    if (m_readBuffer.size() > kMaxLineBytes) {
        // Drop the garbage BEFORE announcing anything: a protocolWarning slot
        // may delete this client, and the release must not depend on surviving
        // the emit. Frees up to the cap (16 MiB) as a side effect.
        m_readBuffer.clear();
        m_scanOffset = 0;
        emit protocolWarning(
            QStringLiteral("RPC line exceeded %1 bytes without a newline; "
                           "resetting transport").arg(kMaxLineBytes));
        if (!self)
            return;
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
    // toInteger() yields the id when the JSON number is a whole value in qint64
    // range and its `defaultValue` otherwise, so probing with two distinct
    // sentinels detects a fractional or out-of-range id without a manual
    // float->int cast (UB out of range). They agree only for a genuine integer
    // id; ids are 64-bit now, matching m_nextId (RC14).
    const qint64 id = idValue.toInteger(0);
    if (id != idValue.toInteger(1)) {
        emit protocolWarning(
            QStringLiteral("RPC response with non-integral or out-of-range id"));
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
    // Whatever is left is a half-received frame from a peer that will never
    // finish it, or bytes we have decided not to trust. Release them: keeping
    // them pins up to the 16 MiB cap for the client's whole remaining life, and
    // a later rebind must never splice a dead producer's tail onto the first
    // frame of the new one.
    m_readBuffer.clear();
    m_scanOffset = 0;
    // Nothing left to probe: stop the timer and forget any outstanding probe
    // before failAllPending() dispatches its callback. m_closed is already
    // latched, so restartHeartbeat() cannot restart it here.
    restartHeartbeat();

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

void CodeharbordClient::onTransportDestroyed()
{
    // Runs from inside the device's ~QObject, so its QIODevice subobject is
    // already gone: even bytesAvailable() would be a use-after-free. Drop the
    // pointer FIRST — do not rely on when QPointer clears relative to
    // destroyed() — then run the ordinary loss path, which now skips the drain
    // and simply fails every pending caller once and announces the close.
    m_transport = nullptr;
    onTransportClosed();
}

void CodeharbordClient::failAllPending(const RpcError& error)
{
    // Move out first: a callback may re-enter (e.g. issue a fresh call) and must
    // not observe a half-cleared map. clear() is not redundant — a moved-from Qt
    // container is left in a valid but unspecified state and must be reset
    // before it is used again.
    const QHash<qint64, ResponseCallback> pending = std::move(m_pending);
    m_pending.clear();
    for (const ResponseCallback& cb : pending) {
        if (cb) // empty for a fire-and-forget call() with a null callback
            cb(QJsonValue(), error);
    }
}

int CodeharbordClient::pendingCount() const
{
    // Probes are real m_pending entries — that is what keeps them off the
    // unknown-id warning path and gets them failed exactly once with everybody
    // else — so they are subtracted back out here rather than never added.
    // Usually one, briefly two when a restart retired a probe that has not been
    // answered or swept yet.
    //
    // Intersected with m_pending rather than trusted blindly, which is what
    // keeps this from going NEGATIVE inside failAllPending(): that moves the map
    // out before dispatching, so a caller re-entering here mid-sweep sees an
    // empty m_pending while the probe callbacks that clear these ids have not
    // run yet.
    int outstanding = static_cast<int>(m_pending.size());
    for (const qint64 id : m_heartbeatProbeIds) {
        if (m_pending.contains(id))
            --outstanding;
    }
    return outstanding;
}

void CodeharbordClient::enableHeartbeat(int intervalMs, int missTolerance)
{
    // Refuse nonsense outright instead of clamping it: a zero or negative
    // interval would either spin the event loop or silently mean "off", and a
    // tolerance below one would declare the very first interval fatal.
    if (intervalMs <= 0 || missTolerance < 1) {
        emit protocolWarning(
            QStringLiteral("enableHeartbeat(%1, %2): refused, heartbeat stays off")
                .arg(intervalMs)
                .arg(missTolerance));
        return;
    }
    m_heartbeatIntervalMs = intervalMs;
    m_heartbeatMissTolerance = missTolerance;
    m_heartbeatTimer->setInterval(intervalMs);
    // Live immediately when a transport is already bound, so the order in which
    // a consumer calls enableHeartbeat() and setTransport() does not matter.
    restartHeartbeat();
}

void CodeharbordClient::restartHeartbeat()
{
    m_heartbeatTimer->stop();
    m_heartbeatMisses = 0;
    // Retire the outstanding probe rather than forgetting it. m_heartbeatProbeIds
    // is deliberately NOT cleared: on the enableHeartbeat() path nothing sweeps
    // m_pending, so that probe is still in flight and must keep being excluded
    // from pendingCount() until its callback runs. The generation bump is what
    // makes that callback a no-op if it lands after a successor is live.
    ++m_heartbeatGeneration;
    m_heartbeatProbeOutstanding = false;
    if (m_heartbeatIntervalMs > 0 && m_transport && !m_closed && !m_destroying)
        m_heartbeatTimer->start();
}

void CodeharbordClient::onHeartbeatTick()
{
    // Belt and braces: every state change that invalidates the timer already
    // stops it, but a tick queued before one of them landed would otherwise
    // write into a corpse.
    if (!m_transport || m_closed || m_destroying) {
        m_heartbeatTimer->stop();
        return;
    }

    if (m_heartbeatProbeOutstanding) {
        // The previous probe is still unanswered AND not one byte has arrived
        // from the peer since (onReadyRead() would have zeroed this counter).
        // Only ONE probe is ever in flight: piling on a fresh id every interval
        // would measure the same silence and merely leave more entries to
        // abandon. Consecutive silent intervals are the measurement.
        ++m_heartbeatMisses;
        if (m_heartbeatMisses < m_heartbeatMissTolerance)
            return;

        // Last-gasp drain, for the same reason onTransportClosed() has one: the
        // peer's answer may already be sitting on the transport with its
        // readyRead() still queued behind this timeout. Killing a session over
        // bytes we simply had not picked up yet would be the worst possible
        // false positive. onReadyRead() zeroes the miss counter for any non-empty
        // read, so that is the signal to check.
        QPointer<CodeharbordClient> self(this);
        if (m_transport->bytesAvailable() > 0) {
            onReadyRead();
            if (!self || m_closed)
                return;
            if (m_heartbeatMisses == 0)
                return;
        }
        // The peer is dead or wedged. Take the ordinary transport-loss path
        // rather than inventing a second kind of failure: every pending
        // callback is failed exactly once with the standard synthetic transport
        // error and transportClosed() is emitted, which is byte for byte what a
        // real disconnect does and what SessionBootstrap's reconnect ladder
        // already handles. onTransportClosed() stops this timer on the way
        // through, via restartHeartbeat().
        emit protocolWarning(
            QStringLiteral("heartbeat: no response from peer for %1 consecutive "
                           "intervals of %2 ms; treating transport as dead")
                .arg(m_heartbeatMissTolerance)
                .arg(m_heartbeatIntervalMs));
        onTransportClosed();
        return;
    }

    sendHeartbeatPing();
}

void CodeharbordClient::sendHeartbeatPing()
{
    // Stamp this probe before issuing it. The callback captures the stamp BY
    // VALUE and refuses to touch the live state unless it still matches, which
    // is the only way an out-of-order callback can be recognised: the request id
    // does not exist until call() returns, and the callback must be handed to
    // call() before that.
    const quint64 generation = ++m_heartbeatGeneration;

    // call() can fail the callback SYNCHRONOUSLY (an unwritable transport, a
    // short write that latches the close), that callback may re-enter this
    // client, and it may even delete it. Hence the QPointer, and hence reading
    // the outcome out of m_pending afterwards instead of trusting the returned
    // id: a probe recorded for a request that was never registered would never
    // be cleared, and every later tick would count misses against a probe that
    // is not on the wire.
    QPointer<CodeharbordClient> self(this);
    const qint64 id =
        call(QString::fromLatin1(ch::rpc::kMethodPing), QJsonValue(),
             [this, generation](QJsonValue, std::optional<RpcError>) {
                 m_heartbeatProbeIds.remove(generation);
                 // A retired probe's answer says nothing about the transport we
                 // hold now — it may not even have come from the same peer — so
                 // it neither clears the live probe nor resets the miss counter.
                 if (generation != m_heartbeatGeneration)
                     return;
                 // Any answer at all — including a "method not found" from a
                 // daemon too old to know the probe — proves the peer is reading
                 // and writing.
                 m_heartbeatProbeOutstanding = false;
                 m_heartbeatMisses = 0;
             });
    if (!self)
        return;
    if (!m_pending.contains(id))
        return; // call() already failed it synchronously; nothing is in flight
    // Tracked for pendingCount() unconditionally, but treated as the LIVE probe
    // only if nothing retired us while call() was running.
    m_heartbeatProbeIds.insert(generation, id);
    if (generation == m_heartbeatGeneration)
        m_heartbeatProbeOutstanding = true;
}

} // namespace ch
