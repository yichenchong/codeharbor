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

    // Bind the transport carrying the JSONL RPC stream. Ownership stays with the
    // caller. Passing a new transport rewires the readyRead/close hooks; the
    // read buffer is reset but in-flight pending callbacks are preserved.
    // Binding a non-null transport emits transportBound() once it is usable.
    void setTransport(QIODevice* transport);
    QIODevice* transport() const { return m_transport; }

    // Issue an async request. Returns the monotonically increasing JSON-RPC id
    // assigned to it. The callback fires exactly once: either when the matching
    // response arrives, with a synthetic error if the transport closes while the
    // request is pending, or — if no writable transport is bound (or the write
    // fails) — synchronously with a synthetic transport error before returning.
    // No callback is ever registered that cannot fire.
    int call(const QString& method, const QJsonValue& params, ResponseCallback cb);

    // Number of requests awaiting a response.
    int pendingCount() const { return static_cast<int>(m_pending.size()); }

    // Launch command used to start the service over SSH (SPEC 10.1).
    static QString launchCommand();

signals:
    // Server -> client notification (id-less message), e.g. file.watchEvent.
    void notificationReceived(const QString& method, const QJsonValue& params);
    // A malformed line, unknown/duplicate response id, or otherwise unroutable
    // message was received. Non-fatal; the stream continues.
    void protocolWarning(const QString& message);
    // The transport closed/disconnected. Emitted after all pending callbacks
    // have been failed with a synthetic error and the pending map cleared.
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
