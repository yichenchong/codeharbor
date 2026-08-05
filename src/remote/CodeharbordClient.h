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

class QTimer;

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
//
// Liveness: there are no per-request deadlines, but an optional transport-level
// HEARTBEAT (enableHeartbeat()) detects a peer that has stopped answering at
// all. See enableHeartbeat() for exactly what that does and does not cover.
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
    // what a multi-megabyte file.readFile over a slow link needs. A single
    // deadline cannot serve both that and a sub-millisecond workspace.list, so
    // a caller that genuinely needs one must still impose its own.
    //
    // What DOES bound the wait is enableHeartbeat(): with it on, a peer that has
    // stopped answering anything is detected within a bounded time and every
    // pending callback is failed through the transport-loss path. That covers
    // "the daemon is dead or wedged"; it does NOT cover "this one reply was lost
    // while the connection stayed healthy" — see enableHeartbeat().
    qint64 call(const QString& method, const QJsonValue& params, ResponseCallback cb);

    // Number of CALLER requests awaiting a response. An outstanding heartbeat
    // probe is deliberately excluded: it is this client's own traffic rather
    // than anyone's call(), and counting it would make the number jitter with
    // the timer under every consumer that reasons about it.
    //
    // qsizetype, not int: this is derived from a QHash::size(), which is
    // 64-bit. Narrowing it here would be a silent truncation of a container
    // size — the one arithmetic in this class that a caller reads directly.
    qsizetype pendingCount() const;

    // Default heartbeat interval, in milliseconds. 15 s is chosen from both
    // ends: it is more than an order of magnitude above the round trip of even
    // a bad link (a satellite or mobile hop answers a 50-byte probe in well
    // under a second) and above the multi-second stalls a busy daemon can take
    // on one large synchronous filesystem call, so a healthy-but-slow peer is
    // never mistaken for a dead one; and it is small enough that the whole
    // detection budget below still fits inside the time a user will sit staring
    // at "saving…" before deciding the app is broken. The probe itself costs
    // ~50 bytes each way, which is free on any link worth using.
    static constexpr int kDefaultHeartbeatIntervalMs = 15000;
    // Default number of CONSECUTIVE silent intervals tolerated before the
    // transport is declared dead. 4 puts detection at roughly 60 s of total
    // silence. One or two would turn a single dropped probe, a garbage-collect
    // pause, or a moment of head-of-line blocking behind a large frame into a
    // spurious session teardown — and a teardown is expensive here, because it
    // fails every in-flight call and forces SessionBootstrap's reconnect ladder.
    // 60 s is still two orders of magnitude better than the ~2 h a default TCP
    // keepalive would take to notice the same half-open connection.
    static constexpr int kDefaultHeartbeatMisses = 4;

    // Turn on the transport-level heartbeat. While a transport is bound, a
    // `ping` request (ch::rpc::kMethodPing) is sent every `intervalMs`; after
    // `missTolerance` consecutive intervals in which NOTHING at all was read
    // from the peer, the transport is treated as lost — every pending callback
    // is failed with the same synthetic transport error a real disconnect
    // produces and transportClosed() is emitted — which is exactly the path
    // SessionBootstrap's reconnect ladder already handles.
    //
    // ALWAYS on, never gated on the connection being idle: a request in flight
    // is not evidence the peer is alive, it is precisely the case being
    // detected. Conversely, ANY bytes arriving from the peer reset the miss
    // counter, not just ping replies — the frames of a slow multi-megabyte
    // file.readFile are proof of life, and because this is one serialized JSONL
    // stream the ping reply cannot be interleaved into the middle of that frame
    // anyway. A working large transfer therefore cannot be killed by the
    // heartbeat. The probe is this client's own traffic: it never appears in
    // pendingCount(), and it can never be reported as an orphaned or unknown
    // response.
    //
    // An interval whose probe could not even be WRITTEN — a transport that is
    // bound and open but not writable, so call() refuses it before a byte
    // reaches the wire — counts as a silent interval too. A failed write is not
    // evidence of a live peer, and treating it as one meant such a transport
    // was probed forever and never declared dead.
    //
    // WHAT THIS DOES NOT GUARANTEE. It bounds "the peer is dead or wedged". It
    // does NOT bound one lost reply: if the daemon drops or forgets a single
    // response while its event loop keeps answering pings, that one caller still
    // waits forever. Nothing here turns a per-connection liveness check into a
    // per-request deadline.
    //
    // The defaults above are the production setting; the parameters exist so
    // tests can compress a 60 s budget into milliseconds. Non-positive values
    // are refused (the heartbeat stays off) rather than silently corrected.
    // Called while a transport is already bound, the heartbeat starts at once.
    //
    // Calling this AGAIN with the configuration already running is a true
    // no-op: it neither retires the probe in flight nor resets the miss
    // counter. Re-arming does both, and a consumer that re-enabled on a timer
    // or on every reconnect attempt would otherwise reset the silence
    // measurement forever (a silent peer would never be detected) while piling
    // an abandoned-but-still-pending probe onto the wire each time. A
    // configuration CHANGE still re-arms, because the old cadence has to go.
    void enableHeartbeat(int intervalMs = kDefaultHeartbeatIntervalMs,
                         int missTolerance = kDefaultHeartbeatMisses);

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
    // The heartbeat interval elapsed: send the next probe, or — if the previous
    // one is still unanswered and nothing at all has been read since — count a
    // miss and, at the tolerance, declare the transport lost.
    void onHeartbeatTick();

private:
    void processLine(const QByteArray& line);
    void failAllPending(const RpcError& error);
    // Issue the next probe. Returns true when a probe is genuinely in flight
    // afterwards, false when call() failed it synchronously (an unwritable
    // transport) so nothing reached the wire — which onHeartbeatTick() counts
    // as a silent interval rather than as a completed probe.
    bool sendHeartbeatPing();
    // Start the heartbeat timer if it is configured and a live transport is
    // bound, stop it otherwise, and RETIRE whatever probe was outstanding —
    // retire, not forget: the probe may still be sitting in m_pending, and its
    // callback must stay harmless rather than be mistaken for an answer about
    // the transport we just moved to. Called from every place the bound/closed
    // state changes, so the timer can never outlive its transport.
    void restartHeartbeat();
    // Drop the bytes onReadyRead() has already consumed (everything before
    // m_readPos) and rebase the two offsets onto the shortened buffer. Called
    // ONCE per readyRead rather than once per frame.
    //
    // This is NOT a performance fix, and the record it replaces claimed it was.
    // The per-frame remove() was never quadratic: Qt 6's
    // QArrayDataPointer::erase has a front-erase fast path that only advances
    // the data pointer, and QByteArray::left() deep-copies rather than sharing,
    // so nothing forced a detach either. Isolating the two disciplines
    // (80 000 frames, no JSON) puts this one ahead by a constant factor — both
    // shapes are linear; see aBurstOfFramesInOneReadIsRoutedOnce in
    // tests/tst_rpcclient.cpp, which records the measurement and deliberately
    // asserts routing rather than wall clock. End to end the difference is
    // invisible next to parsing: 20 000 real frames through
    // one read measure the same either way. What this shape buys is that the
    // consumption point is a member, so a callback that re-enters the reader
    // advances the same cursor instead of re-dispatching consumed frames.
    //
    // remove() keeps the allocation, so this also hands a LARGE buffer back
    // once it has drained: without that, one legitimately multi-megabyte frame
    // pinned its whole buffer for the client's remaining life.
    void compactReadBuffer();

    // QPointer, not a raw pointer: the transport is owned by the CALLER and can
    // be destroyed while still bound. A raw pointer would dangle, and the next
    // setTransport()/destructor `m_transport->disconnect(this)` would be a
    // use-after-free. QPointer auto-nulls instead, and Qt has already dropped
    // the connections for us.
    QPointer<QIODevice> m_transport = nullptr;
    QByteArray m_readBuffer;
    // m_readPos is the first byte onReadyRead() has NOT yet consumed;
    // m_scanOffset is how far past it the search for the next '\n' has already
    // looked and found none, so a large message arriving in many chunks is not
    // re-scanned from the front every readyRead. Invariant:
    // 0 <= m_readPos <= m_scanOffset <= m_readBuffer.size().
    //
    // Both index INTO m_readBuffer, so both MUST be reset wherever that buffer
    // is cleared or the transport is rebound (setTransport, close, the
    // oversize-line drop); a stale offset would otherwise skip past real bytes.
    //
    // Consumption is a CURSOR, not a prefix removal, and it is deliberately
    // kept in members rather than in onReadyRead()'s frame: a response callback
    // may spin a nested event loop and re-enter the reader, and the nested call
    // must see — and advance — the same consumption point. Holding it in a
    // local (or holding a pointer into the buffer, which an append can
    // reallocate) is what would let a re-entrant reader re-dispatch frames the
    // outer loop had already logically consumed.
    qsizetype m_readPos = 0;
    qsizetype m_scanOffset = 0;
    qint64 m_nextId = 1;
    QHash<qint64, ResponseCallback> m_pending;
    // Set once the transport is gone or untrustworthy; CLEARED again by
    // setTransport(), which is how a reconnect revives the client.
    bool m_closed = false;
    // Set when the BYTE STREAM has been declared untrustworthy: an unframed or
    // over-cap line means we can no longer tell where the next frame begins, so
    // whatever else the peer has queued is not a message any more. It exists
    // only to suppress onTransportClosed()'s last-gasp drain, which otherwise
    // reads and routes those very bytes back — and, because each 16 MiB chunk
    // trips the cap again, re-enters onTransportClosed() once per chunk. Cleared
    // by setTransport(), like m_closed.
    bool m_streamUntrusted = false;
    // Set at the top of ~CodeharbordClient and never cleared. m_closed alone is
    // not enough during teardown precisely because setTransport() clears it: a
    // callback being failed by the destructor that reacts with a reconnect would
    // otherwise revive a client that is already being destroyed. Both call() and
    // setTransport() refuse outright while this is set, so every callback the
    // destructor dispatches ends the interaction instead of starting a new one.
    bool m_destroying = false;
    // Owned via the QObject parent (allocated in the constructor, like
    // SessionBootstrap's reconnect timer). Null-interval means "never
    // configured": the heartbeat is opt-in, and SessionBootstrap::wireChannels()
    // is what opts production in.
    QTimer* m_heartbeatTimer = nullptr;
    int m_heartbeatIntervalMs = 0;
    int m_heartbeatMissTolerance = kDefaultHeartbeatMisses;
    // Consecutive intervals in which no proof of life was seen. Reset by
    // onReadyRead() on any non-empty chunk, not merely by a ping reply, so a
    // slow large transfer counts as the proof of life it is. An interval in
    // which the probe could not even be written counts as a miss, because a
    // failed write is not evidence of a live peer either.
    int m_heartbeatMisses = 0;
    // Monotonic stamp identifying the probe currently being awaited. Bumped for
    // every probe sent AND by restartHeartbeat(). This is what makes an OLD
    // probe's callback harmless: the callback captures its own generation by
    // value and does nothing unless it still matches.
    //
    // It has to be a stamp rather than the request id, because the id does not
    // exist until call() returns and the callback has to be handed TO call().
    // And it has to exist at all because a probe's callback can genuinely run
    // while a NEWER probe is live: setTransport() retires the old probe, then
    // sweeps the old pending map, and any callback in that sweep may spin a
    // nested event loop (Qt allows it) in which the heartbeat timer fires and
    // sends the next probe — so the sweep can reach the retired probe's
    // callback after its successor is already on the wire. Clearing the live
    // probe there would break the one-probe-in-flight invariant, corrupt
    // pendingCount(), and leave the miss counter measuring the wrong thing.
    quint64 m_heartbeatGeneration = 0;
    // True while the CURRENT-generation probe is unanswered. A retired probe's
    // answer never clears this: it is evidence about a transport we have
    // already let go. Liveness on the transport we actually hold is covered by
    // onReadyRead()'s miss-counter reset for any inbound bytes.
    bool m_heartbeatProbeOutstanding = false;
    // Generation -> request id, for every probe still sitting in m_pending: the
    // live one plus any retired by a restart that has not been answered or
    // swept yet. Exists ONLY so pendingCount() can report caller requests — the
    // probe deliberately goes through the ordinary call() path, which is what
    // keeps it off the "response for unknown/duplicate id" warning and gets it
    // failed exactly once with everybody else. Entries are removed by the
    // probe's own callback, which call() guarantees fires exactly once, so this
    // can neither leak nor go stale.
    //
    // BOUNDED: onHeartbeatTick() refuses to issue a probe once
    // kMaxOutstandingProbes of them are unanswered, and counts the interval as
    // a miss instead. Without that, re-arming against a peer that never answers
    // strands one more entry here — and one more in m_pending — every time,
    // with nothing but transport loss to reclaim them.
    QHash<quint64, qint64> m_heartbeatProbeIds;
};

} // namespace ch
