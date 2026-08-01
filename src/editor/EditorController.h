#pragma once

#include "SessionState.h"

// Full definition (not a forward declaration): m_client is a QPointer, whose
// QObject static_cast needs the complete CodeharbordClient (a QObject) type.
#include "CodeharbordClient.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

namespace ch {

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
// Lifetime: the client is BORROWED, not owned, and is held through a QPointer
// so it may be destroyed first — a controller parented to a QML pane outlives
// the client whenever the QML engine is torn down last, and the destructor
// below still wants to release its file.watch. A raw pointer would be a
// use-after-free there; the QPointer self-clears and every remote operation
// simply becomes a no-op. Pending RPC callbacks are likewise guarded by a
// QPointer to the controller, so a callback that fires after the controller is
// destroyed is a safe no-op.
class EditorController : public QObject {
    Q_OBJECT
    // SPEC 8.2 file lifecycle as a ch::FileState string (see toString(FileState)).
    Q_PROPERTY(QString fileState READ fileState NOTIFY fileStateChanged)
    // Editor read-only state (SPEC 8.2). DERIVED, never set from outside: see
    // refreshPermissions() / applyStatPermissions() below.
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY readOnlyChanged)
public:
    explicit EditorController(CodeharbordClient* client, QString recoveryId,
                              QObject* parent = nullptr);
    // Releases the active file.watch subscription (SPEC 8.7) so closing the
    // viewer pane this controller serves never leaks a server-side watcher.
    ~EditorController() override;

    QString fileState() const { return toString(m_fileState); }
    bool readOnly() const { return m_readOnly; }

    // Currently open remote path and the revision the buffer is guarded at.
    // Exposed for host/tests; not part of the JS bridge surface.
    QString path() const { return m_path; }
    QString revision() const { return m_revision; }

    // The server-reported recovery directory (server.info.recoveryDir), pushed
    // in by EditorFactory once server.info answers; may arrive after the pane
    // already exists. Base for recoveryPath(); empty until set (recovery
    // disabled), so an older server that reports none degrades cleanly.
    void setRecoveryDir(const QString& dir);

    // This pane's stable per-pane recovery key (the layout paneId), the other
    // half of recoveryPath(). Writable, not a fixed constructor value, because
    // the QML pane's paneId is assigned by the region's layout logic and can
    // settle AFTER this controller exists; EditorPaneView pushes it here whenever
    // it changes so the per-pane snapshot is never keyed by an empty id (which
    // would make two panes on one file share a snapshot). recoveryId() is for
    // hosts/tests; not part of the JS bridge surface.
    Q_INVOKABLE void setRecoveryId(const QString& id);
    QString recoveryId() const { return m_recoveryId; }

    // Byte ceiling on every file.readFile this controller issues (SPEC 8.3).
    //
    // A file.readFile that carries no `length` makes the server read the WHOLE
    // file into memory (the un-ranged branch of read() in remote/src/files.ts),
    // which is then held again in this process and once more as a JavaScript
    // string in the Monaco page — three copies of a file that may be gigabytes.
    // Worse, remote/src/files.ts derives its `truncated` flag FROM the requested
    // length, so an unbounded read cannot even report that the file was too big.
    //
    // 8 MiB matches InternalUrlSchemeHandler::kMaxInlineReadBytes, the ceiling
    // the inline file viewer already applies, so one file is never editable but
    // unviewable (or the reverse). The value is restated here rather than shared
    // because ch_editor does not link ch_viewers and must not start to.
    static constexpr int kMaxEditableReadBytes = 8 * 1024 * 1024;

public slots:
    // ---- JS -> C++ bridge slots (fire-and-forget; results via signals) ----

    // Open a remote file: FileState Loading -> read -> contentLoaded + Clean,
    // subscribe a watch, and check for a crash-recovery snapshot (SPEC 11.3).
    Q_INVOKABLE void open(QString path);
    // Persist the buffer guarded by the revision originally loaded
    // (SPEC 8.4/8.6). Result arrives via saved / saveConflict / saveError.
    Q_INVOKABLE void save(QString content, QString expectedRevision);
    // Snapshot the unsaved buffer to this pane's recovery file (SPEC 11.3) and
    // mark the buffer dirty. Call debounced from the editor. Ignored while a
    // load is in flight: the page is still showing the PREVIOUS file's buffer
    // (the debounce outlives an open()), so honouring it would tag one file's
    // edits with the file now opening and mark a buffer that nobody has edited
    // dirty.
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
    // The state signals are one-shot too, so fileStateChanged and
    // readOnlyChanged are re-emitted here for the current file: a page that
    // connected after the load would otherwise render an empty status bar and,
    // worse, an editable surface over an unwritable file.
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
    // silently stopped. Leave FileState::Disconnected, re-subscribe, and
    // reconcile whatever changed while the session was down. Never touches the
    // buffer's dirty state.
    void onTransportBound();
    // The old transport hit EOF: that codeharbord is unreachable forever, so
    // the subscription id it minted is dead. Forget it WITHOUT an unwatch RPC —
    // the only peer that could receive one is its replacement, which never
    // created it. An open file also enters FileState::Disconnected (SPEC 8.2):
    // nothing can be read or written until a transport is bound again, and a
    // pane that keeps advertising "clean" through an outage is lying about it.
    void onTransportClosed();

private:
    void setFileState(FileState state);
    // Publish m_pathReadOnly || m_bufferReadOnly, emitting readOnlyChanged on a
    // transition. The ONLY writer of m_readOnly.
    void setReadOnly(bool readOnly);
    void updateReadOnly() { setReadOnly(m_pathReadOnly || m_bufferReadOnly); }
    // True when a file.watchEventsLost payload names this pane's own live
    // subscription id. Every malformed shape answers false; see the definition.
    bool watchLossNamesThisPane(const QJsonValue& params) const;
    // Shared reaction to "the file may have changed under us", used by both
    // watch notifications: re-read a clean buffer, only FLAG a dirty one.
    void applyExternalChange();
    // Ask the server what the current path looks like and re-derive
    // m_pathReadOnly from the answer, then run `then` — whether the stat
    // succeeded or not, so a caller can chain work behind it. The one case that
    // does NOT run `then` is a file switch landing while the stat is in flight:
    // the answer describes the previous file and everything chained behind it
    // would too. Read-only-ness is DERIVED on every load and every reconnect,
    // never latched from the first open: a chmod is an ordinary external change.
    void refreshPermissions(std::function<void()> then = {});
    // Fold a file.stat result into m_pathReadOnly.
    void applyStatPermissions(const QJsonObject& stat);
    // Re-fetch the file, parking the pane in `transitional` for the round trip.
    //
    // `discardLocalEdits` says what to do when the buffer moved WHILE the read
    // was in flight — the page debounces its reportContent by 500 ms, so a
    // keystroke can easily land inside the round trip. False (the default, used
    // by every reload the SYSTEM starts: a watch event, a reconnect
    // reconciliation) refuses to overwrite those keystrokes: the fetched bytes
    // are dropped and the pane is left dirty and flagged ExternallyModified,
    // exactly as if the change had been noticed one moment later. True is for
    // reloads the USER asked for (the page's "Reload" affordance, a page that
    // reloaded and lost its buffer), where replacing the buffer IS the request.
    void reload(FileState transitional, bool discardLocalEdits = false);
    // Put ONE file.writeFile on the wire and own its reply. Split out of save()
    // so the reply can re-enter it for a save that was queued behind this one
    // (see save(), and m_saveInFlight below). Callers guarantee m_client,
    // a non-empty m_path and a writable buffer.
    void issueSave(QString content, QString expectedRevision);
    // Emit contentLoaded, or hold it until the page reports ready() (see the
    // ready() slot). The held buffer is overwritten by a newer load, so a
    // reconnecting page always sees the LATEST content exactly once.
    void deliverContent(const QString& content, const QString& revision);
    // Look for a crash-recovery snapshot for the file `generation` loaded. The
    // generation is carried through both round trips so a snapshot belonging to
    // a superseded load can never be offered against the file now open, and the
    // snapshot's own recorded path is matched against the open file so this
    // pane's snapshot of a PREVIOUS file is never offered as the current one
    // (the recovery file is keyed per pane, not per path).
    void checkRecovery(const QString& loadedContent, quint64 generation);
    void writeRecovery(const QString& content, bool retryOnMismatch);
    // Discard this pane's recovery snapshot after a successful save
    // (SPEC 11.3): the saved file IS the buffer now, so a later reopen must not
    // offer a stale "unsaved changes" copy. No-op when no snapshot holds content.
    void clearRecovery();
    // Run a truncate that clearRecovery() had to defer, once the LAST outstanding
    // snapshot write has answered. A no-op unless one was deferred and the count
    // has reached zero; clearRecovery() itself decides whether there is anything
    // to truncate. Called from every terminal path of a snapshot write's reply.
    void honourDeferredRecoveryClear();
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
    // Absolute REMOTE path of THIS pane's single recovery snapshot: a file named
    // by the pane's stable id (m_recoveryId) inside the recovery directory the
    // SERVER reports (m_recoveryDir, from server.info.recoveryDir), NOT a sibling
    // of the edited file (SPEC 11.3). One file per pane, so two panes editing the
    // same path keep independent snapshots; the snapshot records the path it
    // belongs to so a pane that has switched files never offers the wrong one.
    // Empty when there is no stable id or the server reported no recovery dir,
    // which disables recovery rather than sharing one unkeyed file between panes.
    QString recoveryPath() const;

    // QPointer: see the lifetime note at the top of this file — the client may
    // be destroyed before this controller, and the destructor still touches it.
    QPointer<CodeharbordClient> m_client = nullptr;
    // Stable per-pane id (the persisted layout paneId) this pane's recovery
    // snapshot is keyed by; see recoveryPath() and setRecoveryId(). Writable: the
    // pane's paneId can be assigned after this controller is constructed.
    QString m_recoveryId;
    // The server-reported recovery base directory, set via setRecoveryDir().
    // Mutable: it can be pushed in after this controller exists (server.info is
    // asynchronous). Empty => recovery disabled.
    QString m_recoveryDir;

    QString m_path;
    QString m_revision;      // baseline expectedRevision for the main file
    FileState m_fileState = FileState::Disconnected;
    bool m_readOnly = false;
    // Two independent reasons a buffer cannot be written back, OR-ed into
    // m_readOnly by updateReadOnly():
    //   m_pathReadOnly   — file.stat says the file itself is not writable.
    //   m_bufferReadOnly — the bytes we hold are not the file's bytes, so
    //                      saving them would corrupt the file. Two ways that
    //                      happens: a base64 (binary) read, and a TRUNCATED
    //                      read — a file.readFile result carrying truncated=true is a
    //                      PREFIX of the file, and writing a prefix back is
    //                      silent data loss for everything past it. A truncated
    //                      load additionally parks FileState at ReadOnly instead
    //                      of Clean: "clean" asserts the buffer equals the file,
    //                      which for a prefix is simply false.
    bool m_pathReadOnly = false;
    bool m_bufferReadOnly = false;
    // True once reportContent has been seen since the last load/save, or once a
    // save has been issued and not yet confirmed: the buffer holds bytes that
    // are not on the server, so external changes must NOT auto-reload
    // (SPEC 8.7).
    bool m_dirty = false;
    // Bumped by every reportContent(). save() captures it and the write's reply
    // compares: a report that arrived DURING the save means the buffer moved on
    // after the bytes now on the server, so the buffer is still dirty and its
    // recovery snapshot must survive. Without this an edit typed during a save
    // round trip is treated as saved and the next external change silently
    // auto-reloads over it.
    quint64 m_editSerial = 0;
    // Generation of the load this controller WANTS, bumped by every open() and
    // every reload(). A file.readFile reply naming a superseded generation is
    // DROPPED: the responses to two overlapping loads can arrive in either
    // order, and applying the older one would show the wrong bytes and, worse,
    // adopt the wrong revision as the save guard.
    quint64 m_loadGeneration = 0;
    // save() captures m_loadGeneration too, and its reply is DROPPED if that has
    // moved: a write outcome must not re-baseline, mark clean, clear the recovery
    // snapshot of, or report `saved` for a buffer some later load replaced. The
    // m_path check next to it cannot see this, because open() on the SAME path
    // and every reload() leave m_path equal. See the comment at that check for
    // why comparing loads STARTED is sufficient here.

    // Saves are SERIALISED, never raced. The save key beats an SSH round trip
    // easily, and two writes carrying the same expectedRevision end with the
    // second refused for a change this very client made — a "file changed on
    // disk" conflict over nobody else's edit. So at most one write is on the
    // wire; a save() arriving during it either recognises its own bytes and
    // rides the outcome of the write already sent, or parks its bytes in
    // m_queuedSaveContent to be issued by that write's reply, guarded by the
    // revision it produced. Latest buffer wins: an intermediate one the user
    // has already typed past does not deserve a round trip.
    //
    // This lives here rather than in the page because CodeharbordClient
    // GUARANTEES every pending callback fires — on a reply, on transport
    // closure, on transport replacement and in its destructor — so
    // m_saveInFlight cannot latch true and make the save key dead. A flag in
    // the page has no such guarantee.
    bool m_saveInFlight = false;
    // Bumped by every write issued. A reply carrying a superseded generation
    // (open() bumps it too) must not clear the flag or consume the queue: they
    // belong to a later chain, possibly for another file.
    quint64 m_saveGeneration = 0;
    // The bytes of the write on the wire, so a second save of an UNCHANGED
    // buffer is recognised as the same save and costs no second write (which
    // would only move the file's mtime and wake every other watcher). QString is
    // implicitly shared, so this holds a reference, not a copy.
    QString m_inFlightSaveContent;
    // Bytes a save() handed over while a write was on the wire. Dropped when
    // that write FAILS: the pane reports the failure, stays dirty, and the
    // page's Retry / Overwrite re-sends the buffer as it is at the click.
    std::optional<QString> m_queuedSaveContent;

    // Where to go when a transport is bound again after a drop (see
    // onTransportClosed). Conflict / ExternallyModified / ReadOnly survive an
    // outage — the first two because the file on the server still diverges from
    // the buffer and only the user can resolve that, ReadOnly because it
    // describes the bytes this pane is holding (a truncated read) and a drop
    // does not make them whole — while an empty optional means "re-derive from
    // m_dirty", because the load or save the outage killed has no outcome left
    // to report.
    std::optional<FileState> m_resumeState;

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
    // whether the snapshot currently holds a buffer worth offering FOR THE OPEN
    // FILE, so a save truncates it at most once and an already-empty snapshot is
    // never rewritten. The snapshot is a small JSON envelope carrying the remote
    // path alongside the buffer, so checkRecovery() can refuse to offer a
    // snapshot this pane wrote for a different file.
    QString m_recoveryRevision;
    bool m_recoveryHasContent = false;
    // How many snapshot writes issued by writeRecovery() are on the wire. m_recoveryRevision
    // and m_recoveryHasContent are only adopted when its reply lands, so between
    // the two there is a window in which the controller does not yet know a
    // snapshot exists — and clearRecovery() would take its "nothing was ever
    // snapshotted" early exit and truncate nothing.
    //
    // A successful save inside that window therefore used to leave the snapshot
    // behind. If the user typed again between the page's report and the save, that
    // snapshot holds OLDER text than the file now on the server, so the next open
    // offers to restore work the save had already superseded — and since the
    // recovery offer is now a real dialog (see the crash-recovery restore path in
    // the viewer pane) accepting it walks the buffer backwards.
    //
    // clearRecovery() therefore records the intent instead of dropping it, and the
    // LAST outstanding snapshot write's reply carries it out once it knows what to
    // truncate.
    // A COUNT, not a flag: nothing serialises snapshot writes, so two reports in
    // quick succession put two on the wire, and a bool would read "idle" the moment
    // the first answered while the second was still outstanding — the deferred
    // truncate would then be guarded against a revision the second write is about
    // to replace, resurrecting the snapshot after the save.
    int m_recoveryWritesInFlight = 0;
    // Which file's recovery slot the bookkeeping above describes, bumped by every
    // open(). Every snapshot-write callback captures it and returns before touching
    // anything if it has moved: the count and the deferred-truncate intent are
    // shared state, so a reply belonging to the PREVIOUS file must not decrement the
    // new file's count (which would fire its deferred truncate early, or make a
    // later legitimate decrement clamp at zero and lose it). Same idiom as
    // m_loadGeneration and m_watchGeneration.
    quint64 m_recoveryGeneration = 0;
    // Set by clearRecovery() when it cannot act yet; carried out by the last
    // outstanding snapshot write's reply. CLEARED by a fresh content write, because
    // bytes reported after the save are unsaved work the save did not cover and
    // must not be truncated.
    bool m_recoveryClearPending = false;

    // Ready handshake (see the ready() slot). m_pendingContent holds a load that
    // completed before the page connected; std::nullopt means nothing is held.
    bool m_ready = false;
    std::optional<QString> m_pendingContent;
    QString m_pendingRevision;
};

} // namespace ch
