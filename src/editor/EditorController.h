#pragma once

#include "SessionState.h"

// Full definition (not a forward declaration): m_client is a QPointer, whose
// QObject static_cast needs the complete CodeharbordClient (a QObject) type.
#include "CodeharbordClient.h"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

namespace ch {

class EditorPageBridge;

// C++ file lifecycle (SPEC 8.2) behind the C3 editor bridge. Native callers use
// this controller directly; each trusted Monaco document gets a separate
// EditorPageBridge with the same C3 signals/slots and a unique WebChannel id.
// No document proxy is reused across navigation. Reads/writes/watches always
// flow through the injected CodeharbordClient to the remote service.
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
    // Unsaved work survives Error/Disconnected states; lifecycle labels alone
    // cannot decide whether closing or navigating away would discard edits.
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool recoveryEnabled READ recoveryEnabled NOTIFY recoveryEnabledChanged)
    Q_PROPERTY(quint64 recoveryContextRevision READ recoveryContextRevision
               NOTIFY recoveryContextRevisionChanged)
public:
    explicit EditorController(CodeharbordClient* client, QString serverId,
                              QString devSessionId, QString paneId,
                              QObject* parent = nullptr);
    // Releases the active file.watch subscription (SPEC 8.7) so closing the
    // viewer pane this controller serves never leaks a server-side watcher.
    ~EditorController() override;

    QString fileState() const { return toString(m_fileState); }
    bool readOnly() const { return m_readOnly; }
    bool dirty() const { return m_dirty; }
    bool recoveryEnabled() const { return !m_recoveryPath.isEmpty(); }
    quint64 recoveryContextRevision() const { return m_recoveryGeneration; }

    // Retire the previous document BEFORE open/navigation, then register this
    // adapter under its unique objectName and pass that id in the bundle URL.
    // The host deregisters/deletes the old adapter; this controller parents all
    // adapters as a teardown safety net. Native callers need no page adapter.
    Q_INVOKABLE QObject* createPageBridge();
    // QML cannot call QObject::deleteLater directly. Explicit host disposal is
    // accepted only for adapters parented to this controller.
    Q_INVOKABLE void releasePageBridge(QObject* pageBridge);
    // Host-initiated reload: capture before retiring the old page, retaining
    // its RAM text while the controller's baseline/save chain stays authoritative.
    // The context revision must be captured before the asynchronous DOM read.
    Q_INVOKABLE bool preparePageReload(QString content, quint64 expectedRecoveryContext);
    // Retry a failed replacement only from the current document's retained RAM.
    Q_INVOKABLE bool hasPageReloadContent(QObject* pageBridge) const;

    // Currently open remote path and the revision the buffer is guarded at.
    // Exposed for host/tests; not part of the JS bridge surface.
    QString path() const { return m_path; }
    QString revision() const { return m_revision; }

    // The server-reported recovery directory (server.info.recoveryDir), pushed
    // in by EditorFactory once server.info answers; may arrive after the pane
    // already exists. Base for recoveryPath(); empty until set (recovery
    // disabled), so an older server that reports none degrades cleanly.
    void setRecoveryDir(const QString& dir);

    // All three host identities are required. A missing identity deliberately
    // disables recovery; the host must make that unavailable state visible.
    // The opened file path is the fourth component, supplied only by open().
    // Initial incomplete identity may settle once. A different complete scope
    // leaves this document unavailable until open(); temporary gaps retain RAM.
    Q_INVOKABLE void setRecoveryScope(const QString& serverId,
                                     const QString& devSessionId,
                                     const QString& paneId);

    // Byte ceiling on main-file reads (SPEC 8.3).
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
    // A persisted recovery envelope cannot exceed the 16 MiB RPC write frame.
    // Read it in small ranges: JSON escaping/base64 can otherwise put even a
    // within-limit envelope over the daemon's 15 MiB encoded-response ceiling.
    static constexpr int kMaxRecoveryReadBytes = 16 * 1024 * 1024;

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
    // Lightweight edit observation, sent immediately while reportContent stays
    // debounced. The page's baseline prevents an undo against stale bytes from
    // clearing unsaved work after a host-side reload or save.
    Q_INVOKABLE void reportDirty(bool dirty, QString baselineRevision);
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
    // a stale copy. With no transport bound the re-fetch is DEFERRED to the
    // next bind rather than attempted and lost: it would fail synchronously
    // after having already cleared the dirty flag and the recovery slot, which
    // would leave the pane blank and its unsaved work unreachable.
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
    void dirtyChanged();
    void recoveryEnabledChanged();
    void recoveryContextRevisionChanged();

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
    void onTransportClosed(quint64 generation);

private:
    void setFileState(FileState state);
    void setDirty(bool dirty);
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
    // succeeded or not, so a caller can chain work behind it. A superseding
    // load (including a same-path reopen) or transport replacement drops the
    // answer and continuation: they describe a buffer no longer current.
    // Read-only-ness is DERIVED on every load and every reconnect,
    // never latched from the first open: a chmod is an ordinary external change.
    void refreshPermissions(std::function<void()> then = {});
    // Fold a file.stat result into m_pathReadOnly.
    void applyStatPermissions(const QJsonObject& stat);
    // Re-fetch the file, parking the pane in `transitional` for the round trip.
    //
    // `discardLocalEdits` says what to do when the buffer moved on WHILE the
    // read was in flight — the page debounces its reportContent by 500 ms, so a
    // keystroke can easily land inside the round trip, and a save can settle
    // inside it too. False (the default, used by every reload the SYSTEM starts:
    // a watch event, a reconnect reconciliation) refuses to overwrite either:
    // the fetched bytes are dropped, and the pane is flagged ExternallyModified
    // for unsaved edits (exactly as if the change had been noticed one moment
    // later) or left alone for a save that has already reported its own outcome.
    // True is for reloads the USER asked for (the page's "Reload" affordance, a
    // page that reloaded and lost its buffer), where replacing the buffer IS the
    // request.
    void reload(FileState transitional, bool discardLocalEdits = false);
    // Put ONE file.writeFile on the wire and own its reply. Split out of save()
    // so the reply can re-enter it for a save that was queued behind this one
    // (see save(), and m_saveInFlight below). Callers guarantee m_client,
    // a non-empty m_path and a writable buffer.
    void issueSave(QString content, QString expectedRevision, quint64 editSerial);
    // Emit contentLoaded, or hold it until the page reports ready() (see the
    // ready() slot). The held buffer is overwritten by a newer load, so a
    // reconnecting page always sees the LATEST content exactly once.
    void deliverContent(const QString& content, const QString& revision);
    // Fetch the file again for a page that reloaded and lost its buffer: an
    // open() when the buffer was dirty (so this pane's crash-recovery snapshot
    // is probed and offered back), a plain discarding reload otherwise. Called
    // from ready(), or from onTransportBound() when the page reloaded during an
    // outage and the fetch had to wait for a transport.
    void refetchForReloadedPage();
    // Look for a crash-recovery snapshot for the file `generation` loaded. The
    // generation is carried through every round trip so a snapshot belonging
    // to a superseded load can never be offered against the file now open, and
    // the snapshot's own recovery-context generation is carried too so a late
    // server/session/pane or directory update cannot install another slot's
    // revision. Versioned envelope ownership must match the complete context.
    void checkRecovery(const QString& loadedContent, quint64 generation);
    // Assemble bounded raw-byte ranges before UTF-8 decoding, preserving a
    // codepoint split across ranges. Every range and the final stat must agree
    // on revision; superseded probes stop before requesting another range.
    void readRecoverySnapshot(
        QString path, quint64 generation, quint64 recoveryGeneration,
        QByteArray bytes, QString revision,
        std::function<void(QString, QString)> then);
    // Forget everything known about this pane's recovery slot and retire every
    // reply still in flight for it (by bumping m_recoveryGeneration). Shared by
    // open() and by both recovery-key setters, which face the same problem: the
    // slot they described is not the slot the next write will touch.
    void resetRecoverySlot();
    // Probe the recovery slot for the file already open, used by the two
    // recovery-key setters. A no-op unless a clean, writable, fully loaded
    // buffer is on screen — the same conditions open() checks before it probes.
    void probeRecoveryForOpenFile();
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
    // Reconcile after an outage or watch event deferred by a save. A stat
    // distinguishes our own save echo from a later external write, refreshing
    // permissions too; reload a clean buffer, only flag a dirty one.
    void reconcileCurrentFile();
    // Cached server-directory path for the length-framed hash of the accepted
    // server, Dev Session, logical pane and opened file. Legacy raw pane-id
    // records are ambiguous and are never probed, migrated or removed.
    QString recoveryPath() const { return m_recoveryPath; }
    void updateRecoveryPath();

    // QPointer: see the lifetime note at the top of this file — the client may
    // be destroyed before this controller, and the destructor still touches it.
    QPointer<CodeharbordClient> m_client = nullptr;
    QString m_recoveryServerId;
    QString m_recoverySessionId;
    QString m_recoveryPaneId;
    QString m_recoveryPath;
    struct RecoveryScope {
        QString serverId;
        QString sessionId;
        QString paneId;
    };
    // Incomplete initial identity may settle once. A genuinely different
    // complete scope cannot claim this buffer until an explicit open().
    std::optional<RecoveryScope> m_documentRecoveryScope;
    // The server-reported recovery base directory, set via setRecoveryDir().
    // Mutable: it can be pushed in after this controller exists (server.info is
    // asynchronous). Empty => recovery disabled.
    QString m_recoveryDir;

    QString m_path;
    // Last complete content loaded from the server. The controller normally
    // does not own the editor buffer, but keeping this bounded-to-read-window
    // baseline lets a recovery directory or pane id that arrives after open()
    // still probe an existing snapshot without asking the page for a value.
    QString m_loadedContent;
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
    // Permission refreshes can overlap for dirty watch changes. Only the newest
    // stat may update the verdict, even if older replies arrive last.
    quint64 m_permissionGeneration = 0;
    // True once reportDirty/reportContent observes unsaved text, or once a save
    // has been issued and not yet confirmed: the buffer holds bytes that are not
    // on the server, so external changes must NOT auto-reload (SPEC 8.7).
    bool m_dirty = false;
    // Bumped by every reportDirty()/reportContent(). save() captures it and the
    // write's reply compares: an observation DURING the save means the buffer moved on
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
    // rides the outcome of the write already sent, or parks its bytes and edit
    // observation in m_queuedSave to be issued by that write's reply, guarded by the
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
    // must not clear the flag or consume the queue: they belong to a later
    // chain, possibly for another file. Also bumped by every load that COMMITS
    // to the buffer over a running chain — open(), and the explicit reload that
    // is allowed to replace a buffer mid-save — because the bytes that chain is
    // carrying are then no longer the ones on screen.
    quint64 m_saveGeneration = 0;
    // The bytes of the write on the wire, so a second save of an UNCHANGED
    // buffer is recognised as the same save and costs no second write (which
    // would only move the file's mtime and wake every other watcher). QString is
    // implicitly shared, so this holds a reference, not a copy.
    QString m_inFlightSaveContent;
    // Keep the edit observation paired with the queued bytes: edits made while
    // they wait are not covered by that save, even before it reaches the wire.
    struct QueuedSave {
        QString content;
        quint64 editSerial;
    };
    // Dropped on failure; Retry / Overwrite re-sends the page's current buffer.
    std::optional<QueuedSave> m_queuedSave;
    // A watch event can arrive after the server wrote our bytes but before its
    // reply. Reconcile after the save chain instead of losing that notification.
    bool m_externalChangeDuringSave = false;

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
    // whether the slot currently holds a live envelope FOR THE OPEN FILE, so a
    // save truncates it at most once and an already-truncated snapshot is never
    // rewritten. An envelope whose buffer is EMPTY still counts: deleting every
    // line is unsaved work like any other, and the snapshot must be offered on
    // reopen and cleared by the next successful save. Only the zero-length
    // tombstone clearRecovery() writes means "nothing held".
    // The versioned envelope carries complete logical ownership alongside the
    // buffer. A misplaced foreign or legacy record is never adopted or cleared.
    QString m_recoveryRevision;
    bool m_recoveryHasContent = false;
    bool m_recoverySlotBlocked = false;
    // Bumped every time a reply INSTALLS a revision for this pane's slot (a
    // snapshot write, its validated stale-guard retry, a truncate). checkRecovery()
    // captures it before probing and refuses to write either field above if it
    // has moved: those round trips describe the slot as it was
    // before that write, so the older revision would guard the next write
    // against something the server has already replaced — and a truncate the
    // server then refuses leaves a stale snapshot standing behind a successful
    // save, which is precisely the "restore text the save superseded" offer the
    // deferral machinery below exists to prevent. Reachable because a debounced
    // report can complete a snapshot write while the probe is in flight.
    quint64 m_recoverySlotSerial = 0;
    // A recovery clear is itself an asynchronous write. Keep it separate from
    // m_recoveryWritesInFlight (which counts snapshot writes) so two successful
    // saves cannot issue duplicate truncates while the first one is still on
    // the wire.
    bool m_recoveryClearInFlight = false;
    // A recovery probe is one logical operation. This guard prevents a
    // recovery-key update from racing the permission-refresh callback into a
    // duplicate stat, while its generation capture keeps an old probe from
    // clearing the guard belonging to a newer slot.
    bool m_recoveryCheckInFlight = false;
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
    // Which file and recovery key's slot the bookkeeping above describes,
    // bumped by every open(), setRecoveryDir(), and setRecoveryScope(). Every
    // snapshot-write callback captures it and returns before touching anything
    // if it has moved: a reply belonging to the previous file or key must not
    // decrement the new file's count (which would fire its deferred truncate
    // early, or make a later legitimate decrement clamp at zero and lose it).
    // Same idiom as m_loadGeneration and m_watchGeneration.
    quint64 m_recoveryGeneration = 0;
    // Set by clearRecovery() when it cannot act yet; carried out by the last
    // outstanding snapshot write's reply. Cleared by a fresh content write,
    // because bytes reported after the save are unsaved work the save did not
    // cover and must not be truncated.
    bool m_recoveryClearPending = false;

    // Ready handshake (see the ready() slot). m_pendingContent holds a load that
    // completed before the page connected; std::nullopt means nothing is held.
    bool m_ready = false;
    bool m_hasReadyPage = false;
    QPointer<EditorPageBridge> m_pageBridge;
    std::optional<QString> m_pendingContent;
    QString m_pendingRevision;
    // One latest full RAM report, retained across failed replacement loads and
    // ready delivery until exact clean acknowledgment or an explicit reload.
    std::optional<QString> m_pageReloadContent;
    // A page reloaded (a second ready()) while there was no transport to fetch
    // its buffer over, so the fetch is owed and onTransportBound() performs it.
    // Cleared by open(): a host-driven load serves the page on its own.
    bool m_pageNeedsContent = false;
};

// One WebChannel document's capability to the controller. Retirement severs
// signals and actions immediately; delayed pagehide/ready/save messages can
// never operate on the file a replacement document is opening.
class EditorPageBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString fileState READ fileState NOTIFY fileStateChanged)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY readOnlyChanged)
public:
    QString fileState() const;
    bool readOnly() const;

public slots:
    void save(QString content, QString expectedRevision);
    void reportContent(QString content);
    void reportDirty(bool dirty, QString baselineRevision);
    void requestReload();
    void ready();

signals:
    void contentLoaded(QString content, QString revision);
    void fileStateChanged(QString state);
    void readOnlyChanged(bool readOnly);
    void saved(QString revision);
    void saveConflict(QString currentRevision);
    void saveError(QString message);
    // Additive document rehydration; includes the pending save's real bytes so
    // a save finishing after ready cannot baseline against newer unsaved text.
    void bufferRestored(QString content, QString baselineContent, QString revision,
                        QString pendingSaveContent, bool savePending);

private:
    friend class EditorController;
    explicit EditorPageBridge(EditorController* controller);
    void retire();
    QPointer<EditorController> m_controller;
};

} // namespace ch
