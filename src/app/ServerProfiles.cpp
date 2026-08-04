#include "ServerProfiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QLockFile>
#include <QMetaType>
#include <QSettings>
#include <QStringList>
#include <QUuid>
#include <QVariant>
#include <cmath>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace ch {

namespace {

constexpr int kDefaultPort = 22;
constexpr int kMinPort = 1;
constexpr int kMaxPort = 65535;

const QString kServersGroup = QStringLiteral("servers");
const QString kActiveKey = QStringLiteral("servers/active");
const QString kOrdinalField = QStringLiteral("ordinal");

// How long a save waits for another process to finish its own save. Long enough
// that a busy peer is simply waited out (a write is milliseconds), short enough
// that a wedged one cannot freeze the UI thread for a noticeable time.
constexpr int kLockTimeoutMs = 1500;
// How long a lock file whose holder is still alive may sit there before it is
// treated as abandoned. QLockFile clears a lock whose pid is gone immediately;
// this is only the backstop for a live-but-hung holder.
constexpr int kStaleLockMs = 30000;
// NOT ".lock": QSettings::sync() takes a QLockFile on exactly that name.
const QString kLockSuffix = QStringLiteral(".merge-lock");

const QString kId = QStringLiteral("id");
const QString kName = QStringLiteral("name");
const QString kHost = QStringLiteral("host");
const QString kPort = QStringLiteral("port");
const QString kUser = QStringLiteral("user");
const QString kIdentityFile = QStringLiteral("identityFile");
const QString kNodePath = QStringLiteral("nodePath");
const QString kRepoRoot = QStringLiteral("repoRoot");

QString entryKey(const QString& id, const QString& field)
{
    return kServersGroup + QLatin1Char('/') + id + QLatin1Char('/') + field;
}

// The port as an int, or nullopt when the value is present but not a usable
// port. An absent or blank value is not an error: it means "use the default".
std::optional<int> parsePort(const QVariant& raw)
{
    QVariant value = raw;
    if (!value.isValid())
        return kDefaultPort;
    if (value.typeId() == QMetaType::QString) {
        value = value.toString().trimmed(); // a UI field may carry stray spaces
        if (value.toString().isEmpty())
            return kDefaultPort;
    }
    if (value.typeId() == QMetaType::Bool)
        return std::nullopt;
    if (value.typeId() == QMetaType::Double
        || value.typeId() == QMetaType::Float) {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || !std::isfinite(number) || std::trunc(number) != number)
            return std::nullopt;
        if (number < kMinPort || number > kMaxPort)
            return std::nullopt;
        return static_cast<int>(number);
    }

    bool ok = false;
    const int port = value.toInt(&ok);
    if (!ok || port < kMinPort || port > kMaxPort)
        return std::nullopt;
    return port;
}

// Trim, default, and validate one field set. Returns nullopt for a profile that
// could never connect: SshConnectionPool::connectToHost(host, port, user) needs
// all three, so storing a record missing any of them is storing garbage.
// identityFile/nodePath/repoRoot may legitimately be filled in later, so they
// are only trimmed. Unknown keys (including a caller-supplied "id") are
// dropped.
std::optional<QVariantMap> sanitize(const QVariantMap& in)
{
    const QString host = in.value(kHost).toString().trimmed();
    if (host.isEmpty())
        return std::nullopt;

    const QString user = in.value(kUser).toString().trimmed();
    if (user.isEmpty())
        return std::nullopt;

    const std::optional<int> port = parsePort(in.value(kPort));
    if (!port)
        return std::nullopt;

    QString name = in.value(kName).toString().trimmed();
    if (name.isEmpty())
        name = host;

    QVariantMap out;
    out.insert(kName, name);
    out.insert(kHost, host);
    out.insert(kPort, *port);
    out.insert(kUser, user);
    out.insert(kIdentityFile, in.value(kIdentityFile).toString().trimmed());
    out.insert(kNodePath, in.value(kNodePath).toString().trimmed());
    out.insert(kRepoRoot, in.value(kRepoRoot).toString().trimmed());
    return out;
}

// WHY the save could not be serialised, as a clause: the surface that shows it
// owns the sentence around it (AppController turns this into the toast), this
// owns the fact. One branch per error QLockFile can actually report, because a
// message that says "could not be acquired" when the truth is "the directory it
// goes in does not exist" sends whoever reads it hunting for a second process
// that was never there.
QString describeLockFailure(const QLockFile& lock, const QString& lockPath)
{
    switch (lock.error()) {
    case QLockFile::LockFailedError: {
        // Held by somebody else. The pid and host come out of the lock file, so
        // "who has it" is a fact rather than a guess — but a holder that
        // vanished between the last attempt and this read leaves nothing.
        qint64 pid = 0;
        QString host;
        QString application;
        if (lock.getLockInfo(&pid, &host, &application)) {
            return ServerProfiles::tr(
                       "%1 (process %2 on %3) has been holding it for more than "
                       "%4 seconds")
                .arg(application.isEmpty() ? ServerProfiles::tr("another CodeHarbor")
                                           : application)
                .arg(pid)
                .arg(host.isEmpty() ? ServerProfiles::tr("this machine") : host)
                .arg(kLockTimeoutMs / 1000.0);
        }
        return ServerProfiles::tr("another process is holding %1").arg(lockPath);
    }
    case QLockFile::PermissionError:
        return ServerProfiles::tr("%1 could not be created — permission denied")
            .arg(lockPath);
    case QLockFile::NoError:
    case QLockFile::UnknownError:
        break;
    }
    // UnknownError is the catch-all for "the file could not be made", the
    // missing-directory case among them.
    return ServerProfiles::tr("%1 could not be created").arg(lockPath);
}

} // namespace

ServerProfiles::ServerProfiles(QString iniPath, QObject* parent)
    : QObject(parent), m_ownsDirectory(iniPath.isEmpty())
{
    if (iniPath.isEmpty()) {
        m_settings = std::make_unique<QSettings>(
            QStringLiteral("CodeHarbor"), QStringLiteral("CodeHarbor"));
    } else {
        m_settings =
            std::make_unique<QSettings>(iniPath, QSettings::IniFormat);
    }
    // A store written before this rule existed keeps its old, looser mode until
    // something happens to save it; narrow it on the way in instead.
    restrictPermissions();
    load();
}

ServerProfiles::~ServerProfiles() = default;

int ServerProfiles::indexOf(const QString& id) const
{
    if (id.isEmpty())
        return -1;
    for (qsizetype i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles.at(i).toMap().value(kId).toString() == id)
            return static_cast<int>(i);
    }
    return -1;
}

QVariantList ServerProfiles::readStoredProfiles(QString* activeOut) const
{
    struct Row {
        int ordinal;
        QString id;
        QVariantMap fields;
    };
    QList<Row> rows;

    m_settings->beginGroup(kServersGroup);
    const QStringList ids = m_settings->childGroups();
    rows.reserve(ids.size());
    for (const QString& id : ids) {
        const QString prefix = id + QLatin1Char('/');
        // Read-side normalisation, and the ONLY read-side normalisation there
        // is: a hand-edited store is still the user's data, so a field that can
        // be repaired is repaired rather than dropped. A nonsense port becomes
        // the default, because the default is what an ABSENT port already
        // means, so the repair invents nothing.
        //
        // Host and user have no such repair, and no default to fall back on.
        // SshConnectionPool::connectToHost(host, port, user) needs all three,
        // and the save path already refuses to store a profile missing any of
        // them (see sanitize()), so a row with either blank cannot have come
        // from this client and is not a profile at all - it is a row that can
        // only ever offer the user a server entry which fails the moment it is
        // selected. It is skipped, which is the same verdict sanitize() reaches
        // on the way in; the rule lives in this class and is applied on both
        // sides of it rather than being duplicated somewhere else. Like the
        // port repair, the skip is written back by the next save.
        const QString host = m_settings->value(prefix + kHost).toString().trimmed();
        const QString user = m_settings->value(prefix + kUser).toString().trimmed();
        if (host.isEmpty() || user.isEmpty())
            continue;
        const std::optional<int> parsedPort =
            parsePort(m_settings->value(prefix + kPort));

        QVariantMap fields;
        fields.insert(kId, id);
        fields.insert(kName, m_settings->value(prefix + kName).toString());
        fields.insert(kHost, host);
        fields.insert(kPort, parsedPort.value_or(kDefaultPort));
        fields.insert(kUser, user);
        fields.insert(kIdentityFile,
                      m_settings->value(prefix + kIdentityFile).toString());
        fields.insert(kNodePath, m_settings->value(prefix + kNodePath).toString());
        fields.insert(kRepoRoot, m_settings->value(prefix + kRepoRoot).toString());

        // A store written by an older/other writer may carry no ordinal; those
        // entries sort last, in id order, rather than jumping to the front.
        bool hasOrdinal = false;
        const int ordinal = m_settings->value(prefix + kOrdinalField).toInt(&hasOrdinal);
        rows.append({hasOrdinal ? ordinal : std::numeric_limits<int>::max(), id,
                     std::move(fields)});
    }
    if (activeOut)
        *activeOut = m_settings->value(QStringLiteral("active")).toString();
    m_settings->endGroup();

    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.ordinal != b.ordinal)
            return a.ordinal < b.ordinal;
        return a.id < b.id;
    });

    QVariantList out;
    out.reserve(rows.size());
    for (Row& row : rows)
        out.append(std::move(row.fields));
    return out;
}

void ServerProfiles::load()
{
    QString storedActive;
    m_profiles = readStoredProfiles(&storedActive);
    m_activeId.clear();

    // A dangling selection (profile removed behind our back, hand-edited file)
    // must not survive: activeId is either empty or an existing profile.
    if (indexOf(storedActive) >= 0)
        m_activeId = storedActive;
}

void ServerProfiles::writeEntry(const QVariantMap& fields, int ordinal)
{
    const QString id = fields.value(kId).toString();
    m_settings->setValue(entryKey(id, kName), fields.value(kName));
    m_settings->setValue(entryKey(id, kHost), fields.value(kHost));
    m_settings->setValue(entryKey(id, kPort), fields.value(kPort).toInt());
    m_settings->setValue(entryKey(id, kUser), fields.value(kUser));
    m_settings->setValue(entryKey(id, kIdentityFile), fields.value(kIdentityFile));
    m_settings->setValue(entryKey(id, kNodePath), fields.value(kNodePath));
    m_settings->setValue(entryKey(id, kRepoRoot), fields.value(kRepoRoot));
    m_settings->setValue(entryKey(id, kOrdinalField), ordinal);
}

// Serialise the read-merge-write against every other ServerProfiles on this
// file, in this process or another one. Merging alone leaves a window — both
// sides re-read, both merge the same list, both write, the loser's brand new
// profile is gone — and that window is exactly what the class comment promises
// does not exist.
//
// INVARIANT, and the reason there is no re-entrancy guard here: nothing between
// tryLock() and unlock() may emit a signal, run a callback, or spin an event
// loop. QLockFile is not recursive, so a slot that reacted by mutating the
// store would re-enter this function and wait out the whole timeout against a
// lock this very call stack holds — then report "another process is holding it"
// about itself. The locked region is therefore only mergeAndWrite(), which
// touches QSettings and the filesystem and nothing else. The one signal this
// function emits, saveDegraded(), is emitted AFTER the unlock, precisely so a
// handler is free to mutate the store; that re-entry is normal and takes the
// lock cleanly. Anything new that emits belongs after the unlock too.
void ServerProfiles::persist()
{
    const QString settingsPath = m_settings->fileName();
    // A brand new installation has no config directory yet: QSettings creates it
    // lazily, on its first write, which is AFTER this point. QLockFile cannot
    // make its lock file in a directory that does not exist — it fails with
    // UnknownError — so without this every first run would save its first
    // profile unlocked and tell the user so. The very first save is the one case
    // where there is provably no second writer, and it must be silent.
    const QString directory = QFileInfo(settingsPath).absolutePath();
    if (!directory.isEmpty())
        QDir().mkpath(directory);
    // Beside the settings file, so it is on the same filesystem, and NOT the
    // plain `.lock` suffix: that one is QSettings' own sync lock, and taking it
    // here would deadlock inside the sync() calls below.
    const QString lockPath = settingsPath + kLockSuffix;
    QLockFile lock(lockPath);
    // A holder that died leaves the file behind; QLockFile notices the recorded
    // pid is gone and clears it. The stale time is the backstop for a holder
    // that is alive but wedged — generous next to a write measured in
    // milliseconds, and still finite, so the store cannot stay locked forever.
    lock.setStaleLockTime(kStaleLockMs);
    const bool locked = !settingsPath.isEmpty() && lock.tryLock(kLockTimeoutMs);
    if (locked) {
        // The lock file records this process's pid and executable name next to a
        // store we keep owner-only; QLockFile creates it at the umask default.
        QFile::setPermissions(lockPath, QFile::ReadOwner | QFile::WriteOwner);
    }

    mergeAndWrite();

    if (locked)
        lock.unlock();

    // After the unlock: a handler is free to react by mutating the store, and
    // must not find the lock still held (see the INVARIANT above).
    //
    // Only the FIRST degraded save of a run is reported. A store that could not
    // get the lock a moment ago will not get it for the next keystroke either,
    // and one toast per keystroke buries the very message it repeats. A save
    // that does get the lock re-arms the report, so a fresh outage is announced
    // again rather than swallowed for the rest of the session.
    const bool degraded = !locked && !settingsPath.isEmpty();
    const bool announce = degraded && !m_lastSaveDegraded;
    m_lastSaveDegraded = degraded;
    if (announce)
        emit saveDegraded(describeLockFailure(lock, lockPath));
}

void ServerProfiles::mergeAndWrite()
{
    // Pick up anything another writer has put in the file since we last looked,
    // so the wipe-and-rewrite below cannot silently delete it. sync() is the
    // only way to make QSettings re-read a file changed underneath it; there is
    // nothing of ours pending at this point, so it cannot flush a half state.
    m_settings->sync();

    // Profiles in the file that this instance neither holds nor deleted belong
    // to another writer and must survive. An id we removed stays removed even
    // though the file still shows it: that entry is a copy predating our
    // removal, not a new profile, so a plain union would resurrect a deletion.
    // Reading them back through readStoredProfiles() reapplies the field
    // whitelist, so a stray key some other writer added — a "password" a hand
    // edit put there, say — is dropped rather than copied forward.
    QVariantList foreign;
    const QVariantList stored = readStoredProfiles(nullptr);
    for (const QVariant& entry : stored) {
        const QString id = entry.toMap().value(kId).toString();
        if (indexOf(id) >= 0 || m_removedIds.contains(id))
            continue;
        foreign.append(entry);
    }

    // Wipe and rewrite: removals then leave no orphan `servers/<id>/*` keys, and
    // the ordinals always match the merged list order. Ours keep the list order
    // the user sees; another writer's follow, so our insertion order — which the
    // ordinals exist to preserve — is unaffected by their presence.
    m_settings->remove(kServersGroup);
    int ordinal = 0;
    for (const QVariant& entry : std::as_const(m_profiles))
        writeEntry(entry.toMap(), ordinal++);
    for (const QVariant& entry : std::as_const(foreign))
        writeEntry(entry.toMap(), ordinal++);
    // Last write wins on the selection too. m_activeId is either empty or an id
    // in m_profiles, so this can never leave `active` naming a profile that the
    // merged group does not contain.
    m_settings->setValue(kActiveKey, m_activeId);
    // Profile edits are rare and user-driven (unlike drag-resize state), and a
    // half-written server list is exactly what a crash must not leave behind.
    m_settings->sync();
    restrictPermissions();
}

// The store holds no secret — authentication is ssh-agent/keys and nothing here
// ever writes a password or a passphrase (see sanitize(): seven whitelisted
// fields — name, host, port, user, identityFile, nodePath, repoRoot — and
// anything else in the caller's map, a credential prompt's answer included, is
// dropped on the floor). It is still not world business: it names every machine
// you reach, the account you reach it as, and where your checkout lives. Qt
// leaves it at the umask default, which on a typical box is 0644 — and 0664
// wherever the user's primary group is shared, which makes it WRITABLE by
// another account. That last case is the real one: an attacker who can edit
// this file redirects `host` at a machine they own, and the app connects there
// on its next launch.
//
// So: owner-only. Best effort by design — a store on a filesystem with no POSIX
// permissions is not a reason to refuse to save. The containing DIRECTORY is
// only narrowed for the native store, which is ours (~/.config/CodeHarbor); an
// explicit ini path is the caller's business and its parent may well be a
// directory we have no right to relock.
void ServerProfiles::restrictPermissions() const
{
    const QString path = m_settings->fileName();
    if (path.isEmpty())
        return;
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    if (!m_ownsDirectory)
        return;
    const QString dir = QFileInfo(path).absolutePath();
    if (!dir.isEmpty())
        QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

void ServerProfiles::setActiveId(QString id)
{
    if (id == m_activeId)
        return;
    if (!id.isEmpty() && indexOf(id) < 0)
        return; // never point the selection at a profile that does not exist

    m_activeId = std::move(id);
    persist();
    emit activeIdChanged();
}

QString ServerProfiles::addProfile(QVariantMap fields)
{
    const std::optional<QVariantMap> sane = sanitize(fields);
    if (!sane)
        return QString();

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap stored = *sane;
    stored.insert(kId, id);
    m_profiles.append(stored);

    // A first profile with nothing selected is self-evidently the one the user
    // wants: the cold-start path is "add a server, then connect".
    const bool activated = m_activeId.isEmpty();
    if (activated)
        m_activeId = id;

    persist();
    emit profilesChanged();
    if (activated)
        emit activeIdChanged();
    return id;
}

void ServerProfiles::updateProfile(QString id, QVariantMap fields)
{
    const int index = indexOf(id);
    if (index < 0)
        return;

    const QVariantMap existing = m_profiles.at(index).toMap();
    QVariantMap merged = existing;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        merged.insert(it.key(), it.value());

    const std::optional<QVariantMap> sane = sanitize(merged);
    if (!sane)
        return; // an invalid edit must not corrupt a working profile

    QVariantMap stored = *sane;
    stored.insert(kId, id);
    if (stored == existing)
        return; // no spurious change notification for a no-op edit

    m_profiles[index] = stored;
    persist();
    emit profilesChanged();
}

void ServerProfiles::removeProfile(QString id)
{
    const int index = indexOf(id);
    if (index < 0)
        return;

    m_profiles.removeAt(index);
    // Remember the deletion: the next persist() re-reads the file and must not
    // treat a copy of this profile written before we deleted it as somebody
    // else's new profile to be preserved.
    m_removedIds.insert(id);

    const bool wasActive = (m_activeId == id);
    if (wasActive) {
        // Advance to whatever now occupies the removed row, else the new last
        // row, else nothing at all.
        if (m_profiles.isEmpty())
            m_activeId.clear();
        else if (index < m_profiles.size())
            m_activeId = m_profiles.at(index).toMap().value(kId).toString();
        else
            m_activeId = m_profiles.last().toMap().value(kId).toString();
    }

    persist();
    emit profilesChanged();
    if (wasActive)
        emit activeIdChanged();
}

QVariantMap ServerProfiles::profile(QString id) const
{
    const int index = indexOf(id);
    return index < 0 ? QVariantMap() : m_profiles.at(index).toMap();
}

} // namespace ch
