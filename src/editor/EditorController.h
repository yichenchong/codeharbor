#pragma once

#include "SessionState.h"

#include <QJsonValue>
#include <QObject>
#include <QString>

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
    // Editor read-only toggle (SPEC 8.2).
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

    // Toggle read-only mode (e.g. when a stat/permission check says the file
    // cannot be written). Emits readOnlyChanged on transition. Not Q_INVOKABLE:
    // the JS bridge only observes readOnly via readOnlyChanged.
    void setReadOnly(bool readOnly);

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

private:
    void setFileState(FileState state);
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
    static QString recoveryPathFor(const QString& path);

    CodeharbordClient* m_client = nullptr;

    QString m_path;
    QString m_revision;      // baseline expectedRevision for the main file
    FileState m_fileState = FileState::Disconnected;
    bool m_readOnly = false;
    // True once reportContent has been seen since the last load/save: the buffer
    // holds unsaved edits, so external changes must NOT auto-reload (SPEC 8.7).
    bool m_dirty = false;

    QString m_watchSubscriptionId;

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
