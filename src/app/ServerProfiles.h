#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class QSettings;

namespace ch {

// Client-side SSH *connection profiles* (SPEC 4.1, 12.1): the "which machine do
// I talk to" records a user must be able to create BEFORE anything remote can be
// reached. They are deliberately client-local — a profile is the prerequisite
// for opening the connection that a server-side store would have to travel
// over — so they live in QSettings next to the other UI state, never in the
// workspace database.
//
// Storage keys are `servers/<id>/<field>` plus `servers/active`. QSettings'
// IniFormat puts the first path component in the section header and escapes the
// remaining '/' as '\', so on disk that reads:
//
//   [servers]
//   active=<id>                  ; empty, or the id of an existing profile
//   <id>\name=Prod box
//   <id>\host=10.0.0.4
//   <id>\port=22                 ; int, 1..65535
//   <id>\user=yichen
//   <id>\identityFile=~/.ssh/id_ed25519 ; optional local private key
//   <id>\nodePath=/usr/bin/node  ; absolute path to the remote node binary
//   <id>\repoRoot=/srv/codeharbor
//   <id>\ordinal=0               ; internal: preserves insertion order across
//                                ; reloads (childGroups() is sorted by id, which
//                                ; would shuffle the list on every add)
//
// `<id>` is QUuid::createUuid().toString(QUuid::WithoutBraces) — plain hex and
// dashes, safe as an INI key fragment.
//
// Validation happens at the write boundary, never on read: a profile that could
// not possibly connect is refused rather than stored. addProfile() returns an
// empty id and updateProfile() is a no-op when, after normalization, the host or
// the user is empty, the port is not an integral number, or the port falls
// outside 1..65535. Everything else is normalized: strings are trimmed, a
// missing/blank port becomes 22, and a blank name becomes the host.
//
// NO SECRET IS EVER STORED. The seven fields above are a whitelist, applied to
// every addProfile()/updateProfile() call: any other key in the caller's map is
// discarded before anything is written, so a "password" or "passphrase" that a
// credential prompt happened to leave in the same map cannot reach the disk
// even by accident. Authentication is ssh-agent and key files; this store only
// ever names the key FILE. The same whitelist is applied a second time on the
// merge path below, so a key outside it cannot survive in the file even if some
// other writer put it there.
//
// CONCURRENT WRITERS. The store is read once, at construction, and held in
// memory; single-instance use is the assumed model. A second writer is still
// possible — a second copy of the app, or the user editing the file by hand
// while the app runs — and a write must never silently destroy configuration
// this instance never saw. So every write re-reads the file first and MERGES:
//
//   * profiles this instance does not know about are kept, appended after the
//     ones it does know, in their stored order;
//   * profiles this instance deliberately removed stay removed. removeProfile()
//     records the id for the lifetime of this object, so finding the profile
//     still in the file at merge time (another writer's copy predating our
//     removal) does not resurrect it. This is why a plain union is wrong;
//   * a genuine conflict — the same id present here and in the file, edited in
//     both places — is LAST WRITE WINS, and this write is the later one: our
//     in-memory fields replace the stored ones wholesale. Same for
//     `servers/active`. Deliberate: the alternative is per-field timestamps in
//     a hand-editable ini for a scenario the product does not support, and the
//     user's own most recent action is the best available tiebreak.
//
// Merging is computed from a snapshot, and read-merge-write is three steps, so
// two processes can interleave inside it. Qt softens that more than you would
// expect — QSettings::sync() re-reads the file and merges at KEY level, and
// remove("servers") only records the keys that existed when it was called, so a
// profile a peer added in the window survives even unlocked (measured: half a
// thousand interleaved unlocked writes from two processes lost nothing). What
// does NOT survive is anything computed from the snapshot: the ordinals that
// carry the user's ordering, `servers/active`, and the key SET of a profile the
// peer rewrote differently in the window — which can leave a half-erased entry
// behind. And resting a promise of "your configuration is not destroyed" on an
// implementation detail of QSettings' merge is not resting it on anything.
//
// So the WHOLE sequence — re-read, merge, write, flush — is serialised across
// processes by a QLockFile, taken on `<settings file>.merge-lock` (the plain
// `.lock` suffix beside it belongs to QSettings' own sync and taking it here
// would deadlock against Qt). It sits next to the settings file, derived from
// that file's own path, so it is always on the same filesystem — a lock on a
// different mount, or on NFS with a broken flock, is not a lock.
//
// Acquisition is bounded (a couple of seconds): this runs on the UI thread and
// a wedged peer must not freeze the window. A holder that died mid-write leaves
// its lock file behind, and QLockFile's own staleness handling deals with it —
// it reads the recorded pid and hostname, sees the process is gone, and removes
// the file — with a stale time as the backstop for a holder that is alive but
// hung. No home-grown scheme, and nothing that can wedge the store forever.
//
// WHEN THE LOCK CANNOT BE TAKEN — the timeout expires, or the directory does not
// allow creating the lock file at all — the write goes ahead UNLOCKED and
// saveDegraded() carries the reason. Refusing to save would throw away the
// profile the user just typed, which is a certain loss to avoid an unlikely one;
// the merge still runs, so the exposure is the millisecond window above rather
// than the whole lifetime of the object. What is NOT acceptable is doing it
// quietly: AppController turns that signal into the shell's toast, worded as a
// saved-but-unprotected notice, so the guarantee below degrades to best effort
// only in a case the user is actually told about.
//
// So, precisely: as long as every writer is a ServerProfiles that gets the lock,
// no write can destroy a profile it did not know about, and LAST WRITE WINS
// means the process whose locked sequence completed last — a real ordering, not
// an accident of interleaving. A writer that does not participate in the lock at
// all (a text editor, an older build) can still be clobbered inside that
// millisecond window; nothing short of the editor cooperating can fix that, and
// the merge already covers the case that actually happens, which is an edit made
// while the app was not writing.
//
// This is emphatically NOT cross-instance synchronisation: another writer's
// profiles are not adopted into this instance's in-memory list and no signal is
// emitted for them; they simply survive in the file and appear on the next
// load. Keeping two open windows in step is a much larger feature and there is
// deliberately no file watcher here.
class ServerProfiles : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QString activeId READ activeId WRITE setActiveId NOTIFY activeIdChanged)

public:
    // Empty iniPath -> native per-user store (org "CodeHarbor", app
    // "CodeHarbor"), the same scope UiStateStore uses. Non-empty iniPath -> an
    // explicit .ini file in IniFormat, used by tests for a deterministic,
    // isolated store that never touches the developer's real settings.
    explicit ServerProfiles(QString iniPath = QString(), QObject* parent = nullptr);
    ~ServerProfiles() override;

    // Insertion-ordered; each entry is {id, name, host, port (int), user,
    // identityFile, nodePath, repoRoot}.
    QVariantList profiles() const { return m_profiles; }

    // Either empty or the id of a profile that currently exists — a dangling
    // value read from a hand-edited store is dropped on load.
    QString activeId() const { return m_activeId; }

    // Ignores anything that is neither an existing profile id nor the empty
    // string (clearing the selection); no signal is emitted in that case.
    void setActiveId(QString id);

    // Returns the new profile's id, or an empty string when `fields` describes a
    // profile that could never connect (see the class comment). Any `id` key in
    // `fields` is ignored: ids are minted here. The first profile added to an
    // empty selection also becomes the active one.
    Q_INVOKABLE QString addProfile(QVariantMap fields);

    // Merges `fields` over the stored profile: keys that are absent keep their
    // stored value. A no-op for an unknown id, and a no-op (leaving the stored
    // profile intact) when the merged result would be invalid.
    Q_INVOKABLE void updateProfile(QString id, QVariantMap fields);

    // Removing the active profile advances the selection to the profile that
    // took its place in the list, or to the new last profile when the removed
    // one was last, or clears it when nothing is left. Removing any other
    // profile leaves the selection alone.
    Q_INVOKABLE void removeProfile(QString id);

    // Empty map for an unknown id.
    Q_INVOKABLE QVariantMap profile(QString id) const;

signals:
    void profilesChanged();
    void activeIdChanged();
    // A save went ahead without the interprocess lock that normally protects it
    // (see WHEN THE LOCK CANNOT BE TAKEN above). The profile WAS saved; only the
    // safeguard was missing. `reason` is the CAUSE as a clause — "another
    // CodeHarbor (process 4711 on box) has been holding it for more than 1.5
    // seconds" — because the surface that shows it owns the sentence: it is
    // AppController that frames it as a saved-but-unprotected notice and puts it
    // in the shell's toast.
    //
    // Edge-triggered: only the first degraded save of a run is announced, and a
    // save that does get the lock re-arms it. A store that just lost the race
    // will lose it for the next keystroke too, and a toast per keystroke would
    // bury its own message.
    //
    // Emitted after the write and after the lock is released, so a handler may
    // safely mutate the store again.
    void saveDegraded(QString reason);

private:
    // Index into m_profiles, or -1.
    int indexOf(const QString& id) const;
    // Read the whole `servers` group into m_profiles/m_activeId.
    void load();
    // The `servers` group as it currently stands on disk, in the same order and
    // shape load() produces (ordinal, then id; ports repaired; rows with a
    // blank host or user skipped, because they are not connectable profiles and
    // the save path would not have written them; exactly the whitelisted fields
    // plus id). Non-null `activeOut` receives the raw stored `servers/active`,
    // which may name nothing that exists.
    QVariantList readStoredProfiles(QString* activeOut) const;
    // Write one profile's keys at the given list position.
    void writeEntry(const QVariantMap& fields, int ordinal);
    // Save: take the interprocess lock, merge, write, flush (see CONCURRENT
    // WRITERS above). Emits saveDegraded() and writes anyway when the lock
    // cannot be had.
    void persist();
    // The merge itself, under the lock: re-read the file, keep what is neither
    // ours nor deleted, rewrite the whole `servers` group — so removals leave no
    // orphan `servers/<id>/*` keys and the ordinals always match the list order
    // — and flush.
    void mergeAndWrite();
    // Narrow the on-disk store to owner-only. No secret is stored here, but the
    // record of which hosts you reach — and, where the umask leaves the file
    // group-writable, the ability to REDIRECT one — is not other accounts'
    // business. Best effort; a filesystem that cannot express it is not an error.
    void restrictPermissions() const;

    std::unique_ptr<QSettings> m_settings;
    // Ordered list of QVariantMaps, each holding id + the seven profile fields.
    QVariantList m_profiles;
    QString m_activeId;
    // Ids removed through this instance, so a merge cannot bring them back.
    // Ids are UUIDs, so another writer cannot legitimately reuse one.
    QSet<QString> m_removedIds;
    // True for the native per-user store, whose containing directory is ours to
    // lock down; false for a caller-supplied ini path, where only the file is.
    bool m_ownsDirectory = false;
    // NO re-entrancy guard: nothing inside the locked region emits, so persist()
    // can never be re-entered while the lock is held. See the INVARIANT comment
    // on ServerProfiles::persist(), which is what keeps that true.
    // Whether the previous save had to go ahead unlocked, so saveDegraded() can
    // be edge-triggered rather than fired on every keystroke of an outage.
    bool m_lastSaveDegraded = false;
};

} // namespace ch