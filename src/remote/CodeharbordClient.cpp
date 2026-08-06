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

// JSON-RPC 2.0 reserved code for an internal/transport error, sourced from the
// shared mirror header so this client's synthetic failures and ch::rpc's
// contract constants cannot drift apart.
constexpr int kInternalError = rpc::kInternalError;

// Hard cap on ONE framed line, in BOTH directions. Inbound: a well-behaved
// server delimits every message with '\n'; a peer that streams megabytes
// without one is malformed and must not grow m_readBuffer without bound.
// Outbound: call() refuses to write a frame past this, because the daemon drops
// the whole transport on an over-cap line rather than failing one request.
//
// Set above the largest legitimate frame: the internal scheme handler bounds
// inline file reads to 8 MiB raw (~11 MiB base64), so 16 MiB leaves headroom
// for that plus JSON overhead. MAX_LINE_BYTES in remote/src/codeharbord.ts is
// the same number, and remote/test/rpc-mirror.test.ts pins the two together.
constexpr int kMaxLineBytes = 16 * 1024 * 1024;

// Hard cap on how many heartbeat probes may be unanswered at once: the live one
// plus the abandoned-but-still-in-flight ones a re-arm leaves behind. Only
// transport loss reclaims a retired probe, so without a cap a consumer that
// re-arms repeatedly against a silent peer grows m_pending and
// m_heartbeatProbeIds without bound. 4 is generous — reaching it needs three
// configuration changes inside one round trip — and once there, silence is
// already proven, so the tick counts a miss instead of adding a fifth id.
constexpr qsizetype kMaxOutstandingProbes = 4;

// Read-buffer capacity kept across frames. compactReadBuffer() drops the
// consumed prefix but QByteArray::remove() keeps the allocation, so one
// legitimately large frame — an inline file read is megabytes — would pin its
// whole buffer for the rest of the client's life. Anything above this is handed
// back once the buffer has drained; below it, holding on avoids a malloc per
// frame on ordinary traffic.
constexpr qsizetype kIdleReadCapacityBytes = 64 * 1024;

// True when `line` holds nothing but ASCII whitespace, i.e. it is a separator
// rather than a frame. Deliberately allocation free: QByteArray::trimmed() would
// copy the WHOLE line — megabytes for an inline file read — just to answer "is
// this blank?". '\n' cannot appear (the caller split on it) and a trailing '\r'
// has already been chopped, but both are listed for completeness.
bool isBlankLine(const QByteArray& line) noexcept
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
    error.data = QJsonValue(QJsonValue::Null);
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
    releaseReadBuffer();
    m_closed = false;
    m_streamUntrusted = false;

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
        error.data = QJsonValue(QJsonValue::Null);
        failAllPending(error);
        if (!self)
            return;
    }

    // A callback in the sweep above may itself have rebound (a reconnect is the
    // plausible reaction to a transport error) or detached. That nested
    // setTransport() already drained and announced whatever it bound, so the
    // rest of THIS call now belongs to a device we no longer hold: carrying on
    // would drain the wrong transport and emit a SECOND transportBound() for
    // it, making every consumer re-establish its server-side state twice.
    if (m_transport != transport || !m_transport)
        return;

    // Drain anything already buffered on the transport before we subscribed.
    if (m_transport->bytesAvailable() > 0) {
        onReadyRead();
        if (!self)
            return;
        // The drain can end the connection outright — a queued EOF frame, or an
        // unframed line past the size cap — and a callback it dispatched can
        // rebind just like one in the sweep. Announcing a usable transport after
        // either would be a lie.
        if (m_closed || m_transport != transport)
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
    // Keep every id unique while it is in flight, including after the positive
    // qint64 sequence wraps and against heartbeat probes, which use the same
    // pending map. A wrap is practically unreachable, but skipping an occupied
    // slot makes the routing invariant true rather than probabilistic.
    qint64 id = m_nextId;
    while (m_pending.contains(id))
        id = (id == std::numeric_limits<qint64>::max()) ? 1 : id + 1;
    m_nextId = (id == std::numeric_limits<qint64>::max()) ? 1 : id + 1;

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

    // Serialize LAST, and only for a request that is otherwise going out. The
    // size check below needs the bytes, but nothing above does, and building
    // them for a client with no live transport is pure waste: a file.writeFile
    // of an 8 MiB buffer whose C0 bytes escape to 48 MiB of \uXXXX was
    // serialized and thrown away on every attempt while SessionBootstrap's
    // reconnect ladder was still trying to get a transport back.
    QByteArray line;
    if (failReason.isEmpty()) {
        line = QJsonDocument(request).toJson(QJsonDocument::Compact);
        line.append('\n');
        // Both ends of this wire agree that a frame past kMaxLineBytes is a
        // FAULT, not something to buffer: the daemon's framer destroys its stdin
        // on one (createLineFramer in remote/src/codeharbord.ts), which drops the
        // whole SSH RPC channel and with it every editor and terminal in the
        // session. The daemon already refuses to WRITE an over-cap response for
        // exactly that reason; this is the mirror of that check on the request
        // side. The cap measures the PAYLOAD, not the framing newline, which is
        // the same thing the inbound check above measures and what
        // remote/test/rpc-mirror.test.ts pins on the daemon's two directions.
        //
        // It is reachable, not theoretical: file.writeFile carries the buffer's
        // full text, and JSON escaping is not size preserving — 8 MiB of C0
        // control bytes (valid UTF-8, so the daemon returns them as `utf-8` text
        // a pane may edit) becomes 48 MiB of \uXXXX escapes. Refusing the one
        // request keeps the session alive and tells the caller which write was
        // too big, instead of silently taking the connection down.
        if (line.size() - 1 > kMaxLineBytes)
            failReason = QStringLiteral("outgoing frame is %1 bytes, past the "
                                        "%2-byte line cap")
                             .arg(qint64(line.size() - 1))
                             .arg(kMaxLineBytes);
    }

    if (!failReason.isEmpty()) {
        // A protocolWarning slot is free to delete this client, so do not touch
        // any member after the signal. The callback, error and id are locals and
        // remain valid for the required synchronous failure delivery.
        emit protocolWarning(
            QStringLiteral("call(%1): %2").arg(method, failReason));
        RpcError error;
        error.code = kInternalError;
        error.message = QStringLiteral("call failed: %1").arg(failReason);
        error.data = QJsonValue(QJsonValue::Null);
        if (cb)
            cb(QJsonValue(), error);
        return id;
    }

    // Register BEFORE writing. QIODevice subclasses are allowed to emit
    // readyRead/disconnected synchronously from writeData(); registering after
    // write would let such a response or close race past the map and orphan
    // this callback. The entry is removed again below if the write fails.
    const QPointer<QIODevice> writeTransport = m_transport;
    m_pending.insert(id, std::move(cb));
    QPointer<CodeharbordClient> self(this);
    const qint64 written = writeTransport->write(line);
    if (!self)
        return id;

    // A synchronous response or close may already have erased the entry. In
    // that case its callback has fired exactly once and there is nothing left
    // for this call to do. A short write still needs the close path even if a
    // deliberately re-entrant test device answered before returning.
    if (written == line.size())
        return id;

    const bool framingLost = written > 0;
    const QString writeFailure = QStringLiteral("transport write failed");
    emit protocolWarning(
        QStringLiteral("call(%1): %2").arg(method, writeFailure));
    if (!self)
        return id;

    // A partial frame permanently desynchronises the JSONL stream. Close only
    // the transport that was written to: a warning handler may have rebound a
    // new one while the signal was being delivered.
    if (framingLost && m_transport == writeTransport && !m_closed)
        onTransportClosed();
    if (!self)
        return id;

    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return id; // close/rebind already failed it
    ResponseCallback failedCallback = std::move(it.value());
    m_pending.erase(it);
    if (failedCallback) {
        RpcError error;
        error.code = kInternalError;
        error.message = QStringLiteral("call failed: %1").arg(writeFailure);
        error.data = QJsonValue(QJsonValue::Null);
        failedCallback(QJsonValue(), error);
    }
    return id;
}

void CodeharbordClient::onReadyRead()
{
    if (!m_transport || m_closed)
        return;

    const QPointer<QIODevice> sourceTransport = m_transport;
    const qsizetype bufferedBefore = m_readBuffer.size();
    // ONE bounded read per call, sized to what is actually waiting.
    // QIODevice::read(n) resizes its result to n BEFORE reading, so asking for
    // the cap unconditionally requested a 32 MiB allocation (16 MiB rounded up
    // by QByteArray's growth policy) on EVERY readyRead, however few bytes had
    // arrived: a 60-byte ping reply cost a 32 MiB malloc and free.
    // bytesAvailable() is only a lower bound on some devices, so one that
    // reports nothing still gets the full cap, and whatever this read leaves
    // behind is picked up by the queued re-invoke at the bottom of this
    // function.
    const qint64 available = m_transport->bytesAvailable();
    const qint64 chunkSize =
        available > 0 ? qMin<qint64>(available, qint64(kMaxLineBytes) + 1)
                      : qint64(kMaxLineBytes) + 1;
    m_readBuffer.append(m_transport->read(chunkSize));
    // ANY bytes from the peer are proof of life, not just a ping reply. This is
    // one serialized JSONL stream, so while the peer is midway through writing a
    // multi-megabyte file.readFile frame its reply to our probe physically
    // cannot arrive — waiting for the reply alone would tear down exactly the
    // slow, healthy, large transfer the heartbeat is supposed to protect.
    const bool readProgress = m_readBuffer.size() != bufferedBefore;
    if (readProgress)
        m_heartbeatMisses = 0;

    // processLine() invokes a user callback that may delete this client. Watch
    // for that with a QPointer and stop touching members the instant it fires,
    // otherwise the loop's next m_readBuffer access is a use-after-free.
    QPointer<CodeharbordClient> self(this);

    // Consume every complete line through a CURSOR pair held in members.
    // m_readPos is the first unconsumed byte; m_scanOffset is how far past it
    // we have already searched for a '\n' and found none, so a single large
    // message arriving in many small chunks is not re-scanned from the front on
    // every readyRead (an O(n^2) trap otherwise). A trailing partial line stays
    // buffered until the rest arrives on a later readyRead.
    //
    // Nothing derived from the buffer survives a processLine() call: no
    // iterator, no pointer, no cached index. processLine() may spin a nested
    // event loop that re-enters this function, and that nested call appends to
    // m_readBuffer (reallocating it), consumes frames of its own and compacts.
    // Because the consumption point lives in a member, the nested call advances
    // the very cursor this loop re-reads on its next iteration, so a frame is
    // dispatched exactly once no matter which nesting level reaches it. That
    // property is the whole reason the cursor is a member: a consume offset
    // local to this function, or a slice of the buffer held across the
    // dispatch, lets the nested call re-process frames the outer loop had
    // already logically consumed.
    //
    // qsizetype, not int: QByteArray indexes are 64-bit and m_readBuffer holds
    // whatever the bounded read above handed us, which is only bounded by the
    // cap check BELOW — narrowing the index here would corrupt the split of a
    // buffer that grew past 2 GiB before that check ever ran.
    for (;;) {
        const qsizetype newline = m_readBuffer.indexOf('\n', m_scanOffset);
        if (newline == -1)
            break;
        if (newline - m_readPos > kMaxLineBytes) {
            dropUntrustedStream(sourceTransport);
            return;
        }
        // A DEEP copy, not sliced()/left(): those share m_readBuffer's
        // allocation, so a nested reader's append would have to detach the
        // whole buffer — copying every byte still in it, per frame — and the
        // frame handed to processLine() would be a live reference into a buffer
        // that re-entrant code is free to mutate. One frame-sized allocation
        // keeps the two independent.
        QByteArray line(m_readBuffer.constData() + m_readPos,
                        newline - m_readPos);
        // Advance BEFORE dispatching: processLine() can re-enter, and the
        // re-entrant reader must start after the frame we are about to hand out.
        m_readPos = newline + 1;
        m_scanOffset = m_readPos;
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
            releaseReadBuffer();
            return;
        }
        if (m_transport != sourceTransport)
            return;
    }
    // No '\n' past m_scanOffset: the whole buffer has now been searched, so the
    // next readyRead resumes from its end instead of from the cursor.
    m_scanOffset = m_readBuffer.size();
    // Everything before m_readPos has been dispatched; release it in one move.
    // Done BEFORE the cap check so that check measures the unframed remainder
    // rather than a buffer full of frames already delivered.
    compactReadBuffer();

    // Guard against an unterminated line growing the buffer without bound: a
    // peer streaming megabytes with no '\n' is malformed. Drop the garbage and
    // declare the stream unusable — that fails every pending caller once and
    // emits transportClosed(). The transport object itself is left alone (it
    // belongs to the caller and may still be physically healthy); what is gone
    // is our ability to trust a single byte on it, so nothing more is read.
    if (m_readBuffer.size() > kMaxLineBytes) {
        dropUntrustedStream(sourceTransport);
        return;
    }
    // QIODevice::read() intentionally takes only one bounded chunk. A socket
    // may already have more bytes queued, but it will not necessarily emit a
    // second readyRead() just because this call left some unread. Continue in
    // the event queue so a large burst is drained without ever allocating an
    // unbounded readAll() result.
    //
    // `readProgress` is the termination condition, and it is not decoration:
    // bytesAvailable() is only ADVERTISED capacity, so a device that reports
    // readable bytes and then hands back none — a subclass whose count includes
    // a partial record it will not release, say — would have this function
    // re-post itself forever and starve the event loop while never consuming a
    // byte. Rescheduling only after a read that actually delivered something
    // makes progress a precondition for continuing, the same rule
    // onTransportClosed()'s drain loop already applies to its own iteration.
    // m_transport is checked for null in its own right: a callback above may
    // have deleted the caller-owned device, which nulls BOTH QPointers and makes
    // them compare equal.
    if (readProgress && m_transport && m_transport == sourceTransport &&
        !m_closed && m_transport->bytesAvailable() > 0) {
        QMetaObject::invokeMethod(this, &CodeharbordClient::onReadyRead,
                                  Qt::QueuedConnection);
    }
}

void CodeharbordClient::compactReadBuffer()
{
    if (m_readPos > 0) {
        m_readBuffer.remove(0, m_readPos);
        // m_scanOffset is never behind the cursor, so this stays non-negative.
        m_scanOffset -= m_readPos;
        m_readPos = 0;
    }
    // remove() keeps the allocation. Give a big one back once the buffer has
    // drained, so a single multi-megabyte frame does not pin its buffer for the
    // client's whole remaining life.
    if (m_readBuffer.isEmpty() &&
        m_readBuffer.capacity() > kIdleReadCapacityBytes) {
        m_readBuffer.squeeze();
    }
}

void CodeharbordClient::releaseReadBuffer()
{
    // clear(), not remove(0, size()): QByteArray::clear() drops the allocation
    // outright where remove() keeps it, and every caller is abandoning a buffer
    // that may hold up to kMaxLineBytes rather than retiring a dispatched
    // prefix it will refill again in a moment.
    m_readBuffer.clear();
    m_readPos = 0;
    m_scanOffset = 0;
}

void CodeharbordClient::dropUntrustedStream(
    const QPointer<QIODevice>& sourceTransport)
{
    // Release the garbage BEFORE announcing anything: a protocolWarning slot may
    // delete this client, and the release must not depend on surviving the emit.
    releaseReadBuffer();
    // Framing is lost: we no longer know where the next message begins, so
    // nothing still queued on this transport is a message. This is what keeps
    // onTransportClosed()'s last-gasp drain from reading — and routing — those
    // very bytes back, which, because each 16 MiB chunk trips the cap again,
    // re-entered the close path once per chunk of garbage.
    m_streamUntrusted = true;
    QPointer<CodeharbordClient> self(this);
    emit protocolWarning(
        QStringLiteral("RPC line exceeded %1 bytes; resetting transport")
            .arg(kMaxLineBytes));
    if (!self)
        return;
    // Only the transport those bytes came from: a warning slot is free to have
    // rebound a replacement, and this close belongs to the device we gave up on.
    if (m_transport == sourceTransport && !m_closed)
        onTransportClosed();
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
    const QJsonValue methodValue = obj.value(QStringLiteral("method"));
    const QJsonValue versionValue = obj.value(QStringLiteral("jsonrpc"));
    const bool validVersion =
        versionValue.isString() && versionValue.toString() == QLatin1String("2.0");
    const bool noId = idValue.isUndefined() || idValue.isNull();

    // Server -> client notification: carries a method and NO id (JSON-RPC 2.0
    // section 4.1), e.g. file.watchEvent (SPEC 8.7). Checking the id is what
    // keeps this branch honest. A message carrying BOTH a method and an id is a
    // REQUEST aimed at us, which this client does not implement; and a malformed
    // RESPONSE that echoed `method` back would, without the id check, be
    // swallowed as a notification and leave its caller hanging forever instead
    // of being failed below.
    if (methodValue.isString() && noId) {
        if (!validVersion) {
            emit protocolWarning(
                QStringLiteral("RPC notification has invalid jsonrpc member"));
            return;
        }
        if (obj.contains(QStringLiteral("result")) ||
            obj.contains(QStringLiteral("error"))) {
            emit protocolWarning(
                QStringLiteral("RPC notification carries result or error"));
            return;
        }
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
    ResponseCallback cb = std::move(it.value());
    m_pending.erase(it);

    if (!validVersion) {
        emit protocolWarning(
            QStringLiteral("response %1 has invalid jsonrpc member").arg(id));
        RpcError error;
        error.code = kInternalError;
        error.message = QStringLiteral("malformed response: invalid jsonrpc");
        error.data = QJsonValue(QJsonValue::Null);
        if (cb)
            cb(QJsonValue(), error);
        return;
    }

    // JSON-RPC 2.0 section 5: a response object carries jsonrpc/id and exactly
    // one of result/error — never `method`. Reaching here with one means the
    // message is either a REQUEST aimed at us (which this client does not
    // implement) or a corrupted response, so it is not an answer to anything
    // and its `result` must not be handed to the caller as one. Fail the caller
    // instead: that is what keeps the exactly-once contract honest for a
    // message the notification branch above deliberately refused.
    if (!methodValue.isUndefined()) {
        emit protocolWarning(
            QStringLiteral("response %1 carries a method member; treating as "
                           "malformed").arg(id));
        RpcError error;
        error.code = kInternalError;
        error.message =
            QStringLiteral("malformed response: carries a method member");
        error.data = QJsonValue(QJsonValue::Null);
        if (cb)
            cb(QJsonValue(), error);
        return;
    }

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
            const QJsonValue codeValue = errObj.value(QStringLiteral("code"));
            // A well-formed error object has an integer code and string message
            // (JSON-RPC 2.0 section 5.1). Surface the violation but still fail
            // the callback with best-effort fields so the caller cannot hang.
            //
            // `code` is probed with two distinct sentinels because toInt()
            // silently yields its defaultValue for a fractional or
            // out-of-int-range number, so `{"code": 1e12}` would otherwise
            // reach the caller as a perfectly plausible-looking 0 with no hint
            // that anything was lost.
            const bool usableCode =
                codeValue.isDouble() && codeValue.toInt(0) == codeValue.toInt(1);
            if (!usableCode ||
                !errObj.value(QStringLiteral("message")).isString()) {
                emit protocolWarning(
                    QStringLiteral("response %1 error object missing code/message")
                        .arg(id));
            }
            // An unusable code becomes the synthetic internal-error code, NOT
            // 0: 0 is a value a server could legitimately have sent, so a
            // caller comparing against a known application code would silently
            // take the wrong branch. The sibling case just below — an `error`
            // that is not an object at all — already reports this same code for
            // the same class of malformation.
            error.code = usableCode ? codeValue.toInt() : kInternalError;
            error.message = errObj.value(QStringLiteral("message")).toString();
            error.data = errObj.value(QStringLiteral("data"));
        } else {
            // `error` present but not an object at all (e.g. a bare string).
            emit protocolWarning(
                QStringLiteral("response %1 error is not an object").arg(id));
            error.code = kInternalError;
            error.message = QStringLiteral("malformed error: not an object");
            error.data = QJsonValue(QJsonValue::Null);
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
        error.data = QJsonValue(QJsonValue::Null);
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
    const QPointer<QIODevice> closingTransport = m_transport;
    // read() is deliberately bounded, so a close event may find more than one
    // chunk waiting. Drain every chunk that makes progress before failing
    // pending requests; otherwise a valid response just beyond the first
    // chunk would be discarded by the close path.
    //
    // Skipped entirely once the byte stream has been declared UNTRUSTWORTHY (an
    // unframed or over-cap line): there we no longer know where a frame begins,
    // so those bytes are not messages to deliver — and reading them anyway trips
    // the same cap again, which re-enters this function once per 16 MiB of
    // garbage instead of closing the transport.
    while (!m_streamUntrusted && m_transport && m_transport->bytesAvailable() > 0) {
        const qint64 availableBefore = m_transport->bytesAvailable();
        onReadyRead();
        if (!self)
            return; // a callback deleted us mid-drain
        // A callback may have re-entered here (or the drain may have tripped the
        // unframed-line cap), which already latched and announced the close.
        // Without this second check the announcement would go out twice.
        if (m_closed)
            return;
        if (m_transport != closingTransport)
            return;
        // m_transport, not just the comparison above: a callback may have deleted
        // the caller-owned device, which nulls BOTH QPointers and makes them
        // compare equal.
        if (!m_transport || m_transport->bytesAvailable() >= availableBefore)
            break; // protect against a broken device that reports no progress
    }
    m_closed = true;
    // Whatever is left is a half-received frame from a peer that will never
    // finish it, or bytes we have decided not to trust. Release them: keeping
    // them pins up to the 16 MiB cap for the client's whole remaining life, and
    // a later rebind must never splice a dead producer's tail onto the first
    // frame of the new one.
    releaseReadBuffer();
    // Nothing left to probe: stop the timer and forget any outstanding probe
    // before failAllPending() dispatches its callback. m_closed is already
    // latched, so restartHeartbeat() cannot restart it here.
    restartHeartbeat();

    RpcError error;
    error.code = kInternalError;
    error.message = QStringLiteral("transport closed with request pending");
    error.data = QJsonValue(QJsonValue::Null);
    // A failed callback may delete this client during failAllPending(); guard
    // the trailing emit so it never touches a destroyed object. A callback may
    // also rebind or detach the transport, in which case this close belongs to
    // the old device and must not be announced against the new one.
    failAllPending(error);
    if (!self || m_transport != closingTransport || !m_closed)
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

qsizetype CodeharbordClient::pendingCount() const
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
    qsizetype outstanding = m_pending.size();
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

    // Already running exactly this configuration: do NOTHING. restartHeartbeat()
    // below is a re-arm, and a re-arm is not free — it zeroes the miss counter
    // and retires the probe in flight, leaving that probe stranded in m_pending
    // and m_heartbeatProbeIds until the transport dies. A consumer that
    // re-enables periodically (or on every reconnect attempt) would therefore
    // reset the silence measurement forever, so a peer that never answers would
    // never be detected, while accumulating one abandoned probe per call. A
    // configuration CHANGE still falls through: the old cadence has to go.
    if (m_heartbeatTimer->isActive() && intervalMs == m_heartbeatIntervalMs &&
        missTolerance == m_heartbeatMissTolerance) {
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

    QPointer<CodeharbordClient> self(this);

    // "A probe is already on the wire" is the live one, OR enough retired ones
    // that issuing another would just grow m_pending/m_heartbeatProbeIds again.
    // A retired probe is abandoned but still physically in flight and still
    // occupying both collections until the peer answers it or the transport
    // dies; against a peer that never answers, re-arming would otherwise strand
    // one more of them forever, every time. Stopping at the cap loses nothing:
    // kMaxOutstandingProbes unanswered probes are already conclusive evidence
    // of silence, and the interval still counts as a miss below, so detection
    // gets faster rather than slower.
    //
    // Only ONE probe is ever issued at a time: piling on a fresh id every
    // interval would measure the same silence and merely leave more entries to
    // abandon. Consecutive silent intervals are the measurement.
    const bool probeAlreadyInFlight =
        m_heartbeatProbeOutstanding ||
        m_heartbeatProbeIds.size() >= kMaxOutstandingProbes;
    if (!probeAlreadyInFlight) {
        // Nothing outstanding, so probe. A probe that cannot even be WRITTEN is
        // not proof of life either: on a transport that is bound and open but
        // unwritable, call() fails the probe synchronously, and treating that
        // as a completed interval meant the miss counter never moved. The peer
        // was probed forever, never declared dead, and SessionBootstrap's
        // reconnect ladder never ran. Count the interval as the silence it is.
        const bool sent = sendHeartbeatPing();
        if (!self || m_closed || m_destroying)
            return; // the failed write already tore the transport down
        if (sent)
            return;
    }

    // The previous probe is still unanswered (or could not be sent) AND not one
    // byte has arrived from the peer since — onReadyRead() would have zeroed
    // this counter.
    ++m_heartbeatMisses;
    if (m_heartbeatMisses < m_heartbeatMissTolerance)
        return;

    // Last-gasp drain, for the same reason onTransportClosed() has one: the
    // peer's answer may already be sitting on the transport with its
    // readyRead() still queued behind this timeout. Killing a session over
    // bytes we simply had not picked up yet would be the worst possible false
    // positive. onReadyRead() zeroes the miss counter for any non-empty read,
    // so that is the signal to check.
    if (m_transport && m_transport->bytesAvailable() > 0) {
        onReadyRead();
        if (!self || m_closed)
            return;
        if (m_heartbeatMisses == 0)
            return;
    }
    // The peer is dead or wedged. Take the ordinary transport-loss path rather
    // than inventing a second kind of failure: every pending callback is failed
    // exactly once with the standard synthetic transport error and
    // transportClosed() is emitted, which is byte for byte what a real
    // disconnect does and what SessionBootstrap's reconnect ladder already
    // handles. onTransportClosed() stops this timer on the way through, via
    // restartHeartbeat().
    emit protocolWarning(
        QStringLiteral("heartbeat: no response from peer for %1 consecutive "
                       "intervals of %2 ms; treating transport as dead")
            .arg(m_heartbeatMissTolerance)
            .arg(m_heartbeatIntervalMs));
    if (!self)
        return; // the warning slot deleted us; there is nothing left to tear down
    onTransportClosed();
}

bool CodeharbordClient::sendHeartbeatPing()
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
             [self, generation](QJsonValue, std::optional<RpcError>) {
                 if (!self)
                     return;
                 self->m_heartbeatProbeIds.remove(generation);
                 // A retired probe's answer says nothing about the transport we
                 // hold now — it may not even have come from the same peer — so
                 // it does not clear the live probe.
                 if (generation != self->m_heartbeatGeneration)
                     return;
                 // Release the slot so the next tick may probe again. The miss
                 // counter is deliberately NOT touched here: this callback also
                 // runs for a probe this client failed itself (an unwritable
                 // transport, a rebind sweep), which is not evidence of a live
                 // peer. Real proof of life is inbound BYTES, and
                 // onReadyRead() has already zeroed the counter for those —
                 // including for this very reply, which had to be read before
                 // it could be dispatched here.
                 self->m_heartbeatProbeOutstanding = false;
             });
    if (!self)
        return false;
    if (!m_pending.contains(id))
        return false; // call() already failed it synchronously; nothing in flight
    // Tracked for pendingCount() unconditionally, but treated as the LIVE probe
    // only if nothing retired us while call() was running.
    m_heartbeatProbeIds.insert(generation, id);
    if (generation == m_heartbeatGeneration)
        m_heartbeatProbeOutstanding = true;
    return true;
}

} // namespace ch
