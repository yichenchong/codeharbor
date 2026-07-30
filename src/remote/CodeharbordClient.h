#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

QT_BEGIN_NAMESPACE
class QIODevice;
QT_END_NAMESPACE

namespace ch {

// A JSON-RPC 2.0 error object returned by the remote peer (SPEC 10.3). `data`
// is the optional application payload and is null when the server omits it.
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
    ~CodeharbordClient() override;

    // Bind the transport carrying the JSONL RPC stream. Ownership stays with the
    // caller. Passing a new transport rewires the readyRead/close hooks and
    // resets the read buffer.
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
    // Binding a non-null transport emits transportBound() once it is usable.
    void setTransport(QIODevice* transport);
    QIODevice* transport() const { return m_transport; }

    // Issue an async request. Returns the monotonically increasing JSON-RPC id
    // assigned to it (also returned, unused, on the failure paths below). The
    // callback fires exactly once: when the matching response arrives; with a
    // synthetic error if the transport closes or is replaced while the request is
    // pending, or if this client is destroyed while it is still pending; or — if
    // no writable transport is bound, or the write fails — synchronously with a
    // synthetic transport error before returning. No callback is ever registered
    // that cannot fire. A null callback is accepted
    // for a fire-and-forget request; the request is still written and matched,
    // there is simply nothing to invoke.
    int call(const QString& method, const QJsonValue& params, ResponseCallback cb);

    // Number of requests awaiting a response.
    int pendingCount() const { return static_cast<int>(m_pending.size()); }

    // Launch command used to start the service over SSH (SPEC 10.1).
    static QString launchCommand();

signals:
    // Server -> client notification: a message with a method name and NO id
    // (JSON-RPC 2.0 section 4.1), e.g. file.watchEvent. A message carrying both
    // a method and an id is a request aimed at us, which this client does not
    // implement; it is reported through protocolWarning() instead.
    void notificationReceived(const QString& method, const QJsonValue& params);
    // A malformed line, unknown/duplicate response id, or otherwise unroutable
    // message was received. Non-fatal; the stream continues.
    void protocolWarning(const QString& message);
    // The transport closed/disconnected, or its byte stream became untrustworthy
    // (an unframed line past the size cap). Emitted after any bytes still
    // readable on the transport have been routed, then all remaining pending
    // callbacks have been failed with a synthetic error and the pending map
    // cleared. Emitted at most once per bound transport.
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

private:
    void processLine(const QByteArray& line);
    void failAllPending(const RpcError& error);

    QIODevice* m_transport = nullptr;
    QByteArray m_readBuffer;
    int m_nextId = 1;
    QHash<int, ResponseCallback> m_pending;
    bool m_closed = false;
};

} // namespace ch
