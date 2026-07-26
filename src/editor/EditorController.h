#pragma once

#include "SessionState.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace ch {

class CodeharbordClient;

// C++ half of the frozen C3 editor bridge (src/web/editor/src/index.ts,
// EditorBridge) plus the SPEC 8.2 remote-file state machine. A single instance
// is exposed to the trusted Monaco editor page over Qt WebChannel under the
// object name "editor" (see src/qml/EditorPaneView.qml). The editor never
// touches a filesystem directly: all reads/writes/watches flow through the
// injected CodeharbordClient to the remote `codeharbord` service.
//
// WebChannel is asynchronous — C++ can NEVER return a value synchronously to
// JS — so every result is delivered as a SIGNAL. The SLOTS below are
// fire-and-forget bridge actions invoked from JS. The signal/slot signatures
// MUST match EditorBridge exactly.
//
// Lifetime: the client is borrowed (not owned) and MUST outlive the
// controller. Pending RPC callbacks are guarded by a QPointer so a callback
// that fires after the controller is destroyed is a safe no-op.
class EditorController : public QObject {
    Q_OBJECT
    // SPEC 8.2 file lifecycle as a ch::FileState string (see toString(FileState)).
    Q_PROPERTY(QString fileState READ fileState NOTIFY fileStateChanged)
    // Editor read-only state (SPEC 8.2). DERIVED, never set from outside: see
    // refreshPermissions() / applyStatPermissions() below.
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY readOnlyChanged)
public:
    explicit EditorController(CodeharbordClient* client, QObject* parent = nullptr);
    // Releases the active file.watch subscription (SPEC 8.7) so closing an
    // editor pane never leaks a server-side watcher.
    ~EditorController() override;

    QString fileState() const { return toString(m_fileState); }
    bool readOnly() const { return m_readOnly; }

    // Currently open remote path and the revision the buffer is guarded at.
    // Exposed for host/tests; not part of the JS bridge surface.
    QString path() const { return m_path; }
    QString revision() const { return m_revision; }

public slots:
    // ---- JS -> C++ bridge slots (fire-and-forget; results via signals) ----

    // Open a remote file: FileState Loading -> read -> contentLoaded + Clean,
    // subscribe a watch, and check for a crash-recovery snapshot (SPEC 11.3).
    Q_INVOKABLE void open(QString path);
    // Persist the buffer guarded by the revision originally loaded
    // (SPEC 8.4/8.6). Result arrives via saved / saveConflict / saveError.
    Q_INVOKABLE void save(QString content, QString expectedRevision);
    // Snapshot the unsaved buffer to a server-side recovery path (SPEC 11.3)
    // and mark the buffer dirty. Call debounced from the editor.
    Q_INVOKABLE void reportContent(QString content);
    // Re-fetch the file from the server (SPEC 8.7).
    Q_INVOKABLE void requestReload();
    // The WebChannel editor page is live and has attached its signal handlers.
    //
    // WebChannel connects asynchronously and long AFTER this controller exists,
    // so a load that finishes first would emit contentLoaded into the void and
    // leave the pane empty. contentLoaded is therefore HELD until this arrives
    // and replayed exactly once. Additive to the frozen C3 contract (the JS
    // half declares it optional), so an older bundle that never calls it still
    // works — it simply never receives content, as before.
    //
    // A second ready() means the page RELOADED and lost its buffer: there is
    // nothing held to replay, so the file is re-fetched instead of re-emitting
    // a stale copy.
    Q_INVOKABLE void ready();

signals:
    // ---- C++ -> JS bridge signals (MUST match EditorBridge exactly) ----
    void contentLoaded(QString content, QString revision);
    void fileStateChanged(QString state);
    void readOnlyChanged(bool readOnly);
    void saved(QString revision);
    void saveConflict(QString currentRevision);
    void saveError(QString message);

    // A crash-recovery snapshot newer than / differing from the on-disk file
    // was found on open (SPEC 11.3); the UI may offer to restore it. Not part
    // of the frozen EditorBridge JS interface — an additive host->UI signal.
    void recoveryAvailable(QString recoveredContent);

private slots:
    void onNotification(const QString& method, const QJsonValue& params);
    // A NEW codeharbord is on the other end of the RPC client (SPEC 5.6
    // reconnect). Its FileWatchService registry is empty — subscriptions live
    // in the remote PROCESS, not in the wire (remote/src/files.ts) — so the
    // watch this controller established is gone and external-change reload has
    // silently stopped. Re-subscribe and reconcile whatever changed while the
    // session was down. Never touches the buffer's dirty state.
    void onTransportBound();
    // The old transport hit EOF: that codeharbord is unreachable forever, so
    // the subscription id it minted is dead. Forget it WITHOUT an unwatch RPC —
    // the only peer that could receive one is its replacement, which never
    // created it.
    void onTransportClosed();

private:
    void setFileState(FileState state);
    // Publish m_pathReadOnly || m_bufferReadOnly, emitting readOnlyChanged on a
    // transition. The ONLY writer of m_readOnly.
    void setReadOnly(bool readOnly);
    void updateReadOnly() { setReadOnly(m_pathReadOnly || m_bufferReadOnly); }
    // Ask the server what the current path looks like and re-derive
    // m_pathReadOnly from the answer, then run `then` (whether the stat
    // succeeded or not, so a caller can chain work behind it). Read-only-ness is
    // DERIVED on every load and every reconnect, never latched from the first
    // open: a chmod is an ordinary external change.
    void refreshPermissions(std::function<void()> then = {});
    // Fold a file.stat result (RpcTypes StatResult) into m_pathReadOnly.
    void applyStatPermissions(const QJsonObject& stat);
    void reload(FileState transitional);
    // Emit contentLoaded, or hold it until the page reports ready() (see the
    // ready() slot). The held buffer is overwritten by a newer load, so a
    // reconnecting page always sees the LATEST content exactly once.
    void deliverContent(const QString& content, const QString& revision);
    void checkRecovery(const QString& loadedContent);
    void writeRecovery(const QString& content, bool retryOnMismatch);
    // Discard the server-side recovery snapshot after a successful save
    // (SPEC 11.3): the saved file IS the buffer now, so a later reopen must not
    // offer a stale "unsaved changes" copy. No-op when no snapshot holds content.
    void clearRecovery();
    // Release the active file.watch subscription (if any) and forget it, so a
    // pane close / file switch never leaks or duplicates a server-side watcher.
    void unwatchCurrent();
    // Establish the file.watch subscription for the current path (SPEC 8.7).
    // Idempotent: a watch already in flight wins, and an id we still hold is
    // released first so the same path can never end up with two watchers
    // double-delivering every change.
    void subscribeWatch();
    // Close the hole a reconnect leaves: changes made while the transport was
    // down produced no watchEvent anywhere (the old codeharbord was dead, and
    // the replacement's fresh subscription baselines at whatever is on disk
    // when it is created). One file.stat decides whether the buffer is stale;
    // a clean buffer is reloaded, a dirty one is only FLAGGED, exactly as
    // onNotification() treats a live watch event.
    void reconcileAfterReconnect();
    static QString recoveryPathFor(const QString& path);

    CodeharbordClient* m_client = nullptr;

    QString m_path;
    QString m_revision;      // baseline expectedRevision for the main file
    FileState m_fileState = FileState::Disconnected;
    bool m_readOnly = false;
    // Two independent reasons a buffer cannot be written back, OR-ed into
    // m_readOnly by updateReadOnly():
    //   m_pathReadOnly   — file.stat says the file itself is not writable.
    //   m_bufferReadOnly — the bytes we hold are not the file's bytes (a base64
    //                      binary read), so saving them would corrupt it.
    bool m_pathReadOnly = false;
    bool m_bufferReadOnly = false;
    // True once reportContent has been seen since the last load/save: the buffer
    // holds unsaved edits, so external changes must NOT auto-reload (SPEC 8.7).
    bool m_dirty = false;

    QString m_watchSubscriptionId;
    // Generation of the watch this controller WANTS, bumped by every open() and
    // every transport swap. A file.watch response that names a superseded
    // generation must not install its (now meaningless) subscription id, nor
    // clear the in-flight guard belonging to a newer attempt — the transport can
    // be replaced with an open() or a save() still in flight.
    quint64 m_watchGeneration = 0;
    // A file.watch request is outstanding. Guards against double-subscribing
    // when a reconnect lands on top of a subscribe that has not answered yet.
    bool m_watchPending = false;

    // Recovery snapshot bookkeeping (SPEC 11.3). Empty revision => the next
    // write is create-only (expectedRevision ""). m_recoveryHasContent tracks
    // whether the snapshot currently holds a buffer worth offering, so a save
    // truncates it at most once and an already-empty snapshot is never rewritten.
    QString m_recoveryRevision;
    bool m_recoveryHasContent = false;

    // Ready handshake (see the ready() slot). m_pendingContent holds a load that
    // completed before the page connected; std::nullopt means nothing is held.
    bool m_ready = false;
    std::optional<QString> m_pendingContent;
    QString m_pendingRevision;
};

} // namespace ch
