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
    void setTransport(QIODevice* transport);
    QIODevice* transport() const { return m_transport; }

    // Issue an async request. Returns the monotonically increasing JSON-RPC id
    // assigned to it; the callback fires once when the matching response
    // arrives, or with a synthetic error if the transport closes first.
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
