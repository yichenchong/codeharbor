#pragma once

#include "SessionState.h"

#include <QJsonValue>
#include <QObject>
#include <QString>

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
    void checkRecovery(const QString& loadedContent);
    void writeRecovery(const QString& content, bool retryOnMismatch);
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
    // write is create-only (expectedRevision "").
    QString m_recoveryRevision;
};

} // namespace ch
