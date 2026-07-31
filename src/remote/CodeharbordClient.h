#pragma once

#include <QByteArray>
#include <QHash>
#include <QIODevice>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

namespace ch {

// A JSON-RPC 2.0 error object returned by the remote peer (SPEC 10.3). `data`
// is the optional application payload, copied verbatim from the server's error
// object: it is UNDEFINED (QJsonValue::isUndefined()) when that object omits
// the member, null when the server sends an explicit null, and null on the
// synthetic errors this client mints itself. Read it with toObject()/toString()
// and friends, which fold undefined and null together, rather than branching on
// isNull().
// Application error codes (e.g. ch::rpc::kRevisionMismatch, -32001) are surfaced
// verbatim here like any transport/reserved code — the client does not special
// case them.
struct RpcError {
    int code = 0;
    QString message;
    QJsonValue data;
};

// Client-side RPC peer for the remote `codeharbord` service (SPEC 10). Speaks
// newline-delimited JSON-RPC 2.0 (SPEC 10.3) over a QIODevice transport so it is
// exercisable without SSH: production wires a dedicated SSH RPC channel from the
// connection pool, tests wire a QBuffer or QLocalSocket pair. Distinct from the
// server-side implementation in remote/.
class CodeharbordClient : public QObject {
    Q_OBJECT
public:
    // Callback for an async request: exactly one of (result, error) is
    // meaningful. On success `error` is std::nullopt and `result` holds the
    // decoded JSON-RPC result; on failure `error` holds the RpcError and
    // `result` is null.
    using ResponseCallback =
        std::function<void(QJsonValue result, std::optional<RpcError> error)>;

    explicit CodeharbordClient(QObject* parent = nullptr);

    // Fails every request still in flight with a synthetic error, so call()'s
    // exactly-once guarantee holds on this path too: destroying the client is
    // the one way a pending callback could otherwise be dropped silently,
    // because nothing is left that could ever answer the ids we minted.
    //
    // Teardown is FINAL. Those failure callbacks run inside this destructor and
    // may re-enter the client; both call() and setTransport() refuse while it is
    // running, so a callback that reacts by retrying is failed synchronously
    // (registering nothing) and one that reacts by reconnecting cannot resurrect
    // an object that is already going away.
    ~CodeharbordClient() override;

    // Bind the transport carrying the JSONL RPC stream. Ownership stays with the
    // caller. Passing a new transport rewires the readyRead/close hooks and
    // resets the read buffer.
    //
    // Passing the device ALREADY bound is a no-op — unless this client has
    // latched closed on it (see transportClosed()), in which case the bind is
    // performed in full and revives the client. A caller that reopens the very
    // same QIODevice object instead of allocating a new one would otherwise be
    // stuck with a permanently dead client whose every call() fails.
    //
    // Every request still in flight is FAILED with a synthetic transport error,
    // because the ids we issued mean nothing to anyone but the peer being
    // dropped: a reconnect (SPEC 5.6) hands us a different `codeharbord` process
    // whose request table never heard of them, and a detach
    // (setTransport(nullptr)) leaves nobody to answer at all. Keeping them would
    // hang their callers forever. Those failures run synchronously, inside this
    // call, and may themselves re-enter call() — a retry issued there goes out on
    // the NEW transport.
    //
    // Called while ~CodeharbordClient is running — from one of the failure
    // callbacks it dispatches — this does NOTHING: teardown is final and must
    // not be undone by a consumer reacting to the failure with a reconnect.
    //
    // Binding a non-null transport emits transportBound() once it is usable.
    void setTransport(QIODevice* transport);
    QIODevice* transport() const { return m_transport; }

    // Issue an async request. Returns the monotonically increasing JSON-RPC id
    // assigned to it (also returned, unused, on the failure paths below). The
    // callback fires exactly once: when the matching response arrives; with a
    // synthetic error if the transport closes, is replaced, or is DESTROYED
    // while the request is pending, or if this client is destroyed while it is
    // still pending; or — if no writable transport is bound, or the write fails
    // — synchronously with a synthetic transport error before returning. No
    // callback is ever registered that cannot fire. A null callback is accepted
    // for a fire-and-forget request; the request is still written and matched,
    // there is simply nothing to invoke.
    //
    // There is deliberately NO per-request timeout: on a transport that stays
    // healthy the callback waits for the peer for as long as it takes, which is
    // what a multi-megabyte file.readFile over a slow link needs. The flip side
    // is that a server which accepts a request and never answers it keeps the
    // callback pending until the transport goes away, so a caller that needs a
    // deadline must impose its own.
    qint64 call(const QString& method, const QJsonValue& params, ResponseCallback cb);

    // Number of requests awaiting a response.
    int pendingCount() const { return static_cast<int>(m_pending.size()); }

signals:
    // Server -> client notification: a message with a method name and NO id
    // (JSON-RPC 2.0 section 4.1), e.g. file.watchEvent. A message carrying a
    // method AND an id is not a notification and is never dispatched here: it is
    // routed on its id like any response, which fails the matching caller (a
    // response is not allowed to carry `method`, so the message is malformed
    // whichever way it was meant) and always emits a protocolWarning().
    void notificationReceived(const QString& method, const QJsonValue& params);
    // A malformed line, unknown/duplicate response id, or otherwise unroutable
    // message was received. Non-fatal; the stream continues.
    void protocolWarning(const QString& message);
    // The transport closed/disconnected, was DESTROYED by its owner while still
    // bound, or its byte stream became untrustworthy (an unframed line past the
    // size cap). Emitted after any bytes still readable on the transport have
    // been routed, then all remaining pending callbacks have been failed with a
    // synthetic error, the pending map cleared and the read buffer released.
    // Emitted at most once per bound transport. Once it has fired, no further
    // frame from that transport is routed — not even one already sitting in the
    // read buffer — and every call() fails synchronously until a transport is
    // (re)bound.
    //
    // Detaching or replacing the transport with setTransport() does NOT emit
    // this; it fails pending callbacks without announcing a close, because the
    // caller doing the detaching already knows.
    void transportClosed();
    // A NEW, non-null transport is bound and ready to carry requests.
    //
    // Consumers that hold state living inside the PROCESS on the other end
    // MUST re-establish it here. A file.watch subscription is the case that
    // forced this signal: remote/src/files.ts keeps FileWatchService's
    // subscriptions in a plain per-process Map, so a transport swap (SPEC 5.6
    // reconnect) hands us a different `codeharbord` whose registry is empty and
    // whose replacement never heard of the ids we are holding.
    //
    // Fires ONLY for a non-null transport, and only after the new one is fully
    // wired and drained, so a handler may issue calls immediately. Detaching —
    // setTransport(nullptr) during teardown — deliberately emits nothing: a
    // consumer that re-established there would only write into a client with
    // nothing bound. The matching "the old one went away" edge is
    // transportClosed().
    void transportBound();

private slots:
    void onReadyRead();
    void onTransportClosed();
    // The caller-owned transport was DESTROYED while still bound. Nobody will
    // ever answer the ids in flight on it, so this is a transport loss like any
    // other; it exists separately from onTransportClosed() only to drop the
    // (already half-destroyed) device pointer before the close path can touch
    // it.
    void onTransportDestroyed();

private:
    void processLine(const QByteArray& line);
    void failAllPending(const RpcError& error);

    // QPointer, not a raw pointer: the transport is owned by the CALLER and can
    // be destroyed while still bound. A raw pointer would dangle, and the next
    // setTransport()/destructor `m_transport->disconnect(this)` would be a
    // use-after-free. QPointer auto-nulls instead, and Qt has already dropped
    // the connections for us.
    QPointer<QIODevice> m_transport = nullptr;
    QByteArray m_readBuffer;
    // How far into m_readBuffer onReadyRead() has already scanned for a '\n'
    // without finding one, so a large message arriving in many chunks is not
    // re-scanned from the front every readyRead. It indexes INTO m_readBuffer,
    // so it MUST be reset to 0 wherever that buffer is cleared or the transport
    // is rebound (setTransport, close, the oversize-line drop); a stale offset
    // would otherwise skip past real bytes.
    qsizetype m_scanOffset = 0;
    qint64 m_nextId = 1;
    QHash<qint64, ResponseCallback> m_pending;
    // Set once the transport is gone or untrustworthy; CLEARED again by
    // setTransport(), which is how a reconnect revives the client.
    bool m_closed = false;
    // Set at the top of ~CodeharbordClient and never cleared. m_closed alone is
    // not enough during teardown precisely because setTransport() clears it: a
    // callback being failed by the destructor that reacts with a reconnect would
    // otherwise revive a client that is already being destroyed. Both call() and
    // setTransport() refuse outright while this is set, so every callback the
    // destructor dispatches ends the interaction instead of starting a new one.
    bool m_destroying = false;
};

} // namespace ch
