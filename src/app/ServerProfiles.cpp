#include "ServerProfiles.h"

#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMetaType>
#include <QSettings>
#include <QStringList>
#include <QUuid>
#include <QVariant>

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
    if (value.typeId() == QMetaType::QString)
        value = value.toString().trimmed(); // a UI field may carry stray spaces
    if (!value.isValid() || value.toString().isEmpty())
        return kDefaultPort;

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

void ServerProfiles::load()
{
    m_profiles.clear();
    m_activeId.clear();

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
        // Read-side normalization is limited to the port: a hand-edited store is
        // still the user's data, so nothing is dropped here, but a nonsense port
        // must not reach connectToHost().
        const int storedPort = m_settings->value(prefix + kPort).toInt();
        const bool portUsable = storedPort >= kMinPort && storedPort <= kMaxPort;

        QVariantMap fields;
        fields.insert(kId, id);
        fields.insert(kName, m_settings->value(prefix + kName).toString());
        fields.insert(kHost, m_settings->value(prefix + kHost).toString());
        fields.insert(kPort, portUsable ? storedPort : kDefaultPort);
        fields.insert(kUser, m_settings->value(prefix + kUser).toString());
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
    const QString storedActive = m_settings->value(QStringLiteral("active")).toString();
    m_settings->endGroup();

    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.ordinal != b.ordinal)
            return a.ordinal < b.ordinal;
        return a.id < b.id;
    });

    m_profiles.reserve(rows.size());
    for (Row& row : rows)
        m_profiles.append(std::move(row.fields));

    // A dangling selection (profile removed behind our back, hand-edited file)
    // must not survive: activeId is either empty or an existing profile.
    if (indexOf(storedActive) >= 0)
        m_activeId = storedActive;
}

void ServerProfiles::persist()
{
    // Wipe and rewrite: removals then leave no orphan `servers/<id>/*` keys, and
    // the ordinals always match the current list order.
    m_settings->remove(kServersGroup);
    for (qsizetype i = 0; i < m_profiles.size(); ++i) {
        const QVariantMap fields = m_profiles.at(i).toMap();
        const QString id = fields.value(kId).toString();
        m_settings->setValue(entryKey(id, kName), fields.value(kName));
        m_settings->setValue(entryKey(id, kHost), fields.value(kHost));
        m_settings->setValue(entryKey(id, kPort), fields.value(kPort).toInt());
        m_settings->setValue(entryKey(id, kUser), fields.value(kUser));
        m_settings->setValue(entryKey(id, kIdentityFile),
                             fields.value(kIdentityFile));
        m_settings->setValue(entryKey(id, kNodePath), fields.value(kNodePath));
        m_settings->setValue(entryKey(id, kRepoRoot), fields.value(kRepoRoot));
        m_settings->setValue(entryKey(id, kOrdinalField), static_cast<int>(i));
    }
    m_settings->setValue(kActiveKey, m_activeId);
    // Profile edits are rare and user-driven (unlike drag-resize state), and a
    // half-written server list is exactly what a crash must not leave behind.
    m_settings->sync();
    restrictPermissions();
}

// The store holds no secret — authentication is ssh-agent/keys and nothing here
// ever writes a password or a key (see sanitize(): six whitelisted fields, and
// anything else in the caller's map is dropped on the floor). It is still not
// world business: it names every machine you reach, the account you reach it
// as, and where your checkout lives. Qt leaves it at the umask default, which
// on a typical box is 0644 — and 0664 wherever the user's primary group is
// shared, which makes it WRITABLE by another account. That last case is the
// real one: an attacker who can edit this file redirects `host` at a machine
// they own, and the app connects there on its next launch.
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
