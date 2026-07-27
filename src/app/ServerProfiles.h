#pragma once

#include <QObject>
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
// the user is empty or the port falls outside 1..65535. Everything else is
// normalized: strings are trimmed, a missing/blank port becomes 22, and a blank
// name becomes the host.
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
    // nodePath, repoRoot}.
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

private:
    // Index into m_profiles, or -1.
    int indexOf(const QString& id) const;
    // Read the whole `servers` group into m_profiles/m_activeId.
    void load();
    // Rewrite the whole `servers` group from m_profiles/m_activeId, so removed
    // profiles leave no orphan keys behind, and flush it to disk.
    void persist();
    // Narrow the on-disk store to owner-only. No secret is stored here, but the
    // record of which hosts you reach — and, where the umask leaves the file
    // group-writable, the ability to REDIRECT one — is not other accounts'
    // business. Best effort; a filesystem that cannot express it is not an error.
    void restrictPermissions() const;

    std::unique_ptr<QSettings> m_settings;
    // Ordered list of QVariantMaps, each holding id + the six profile fields.
    QVariantList m_profiles;
    QString m_activeId;
    // True for the native per-user store, whose containing directory is ours to
    // lock down; false for a caller-supplied ini path, where only the file is.
    bool m_ownsDirectory = false;
};

} // namespace ch
