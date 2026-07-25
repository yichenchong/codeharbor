#include "KnownHosts.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QStringList>

namespace ch {

namespace {

// OpenSSH hashes a known_hosts hostname as "|1|<base64 salt>|<base64 hash>",
// where hash = HMAC-SHA1(key = salt, message = hostname). Return true iff the
// stored hashed token names this host. Malformed tokens never match.
bool hashedHostMatches(const QString& hostField, const QString& host)
{
    const QStringList parts = hostField.split(QLatin1Char('|'));
    // Well-formed token splits to ["", "1", salt, hash].
    if (parts.size() != 4 || !parts.at(0).isEmpty()
        || parts.at(1) != QLatin1String("1"))
        return false;
    const QByteArray salt = QByteArray::fromBase64(parts.at(2).toUtf8());
    const QByteArray expected = QByteArray::fromBase64(parts.at(3).toUtf8());
    if (salt.isEmpty() || expected.isEmpty())
        return false;
    QMessageAuthenticationCode mac(QCryptographicHash::Sha1);
    mac.setKey(salt);
    mac.addData(host.toUtf8());
    return mac.result() == expected;
}

// A stored entry names `host` if it matches the plaintext token or, for a
// hashed |1| token, the HMAC-SHA1 salted hash of the hostname.
bool entryHostMatches(const QString& storedHost, const QString& host)
{
    if (storedHost.startsWith(QLatin1Char('|')))
        return hashedHostMatches(storedHost, host);
    return storedHost == host;
}

} // namespace

KnownHosts::Verdict KnownHosts::verify(const QString& host,
                                       const QString& keyType,
                                       const QByteArray& keyBlob) const
{
    // Revocation takes precedence regardless of entry order (OpenSSH semantics):
    // if any @revoked entry names this exact host+keyType+blob, refuse it even
    // when a trusted entry for the same key appears earlier in the file.
    for (const Entry& e : m_entries) {
        if (e.marker == QLatin1String("@revoked") && e.keyType == keyType
            && e.key == keyBlob && entryHostMatches(e.host, host))
            return Verdict::Mismatch;
    }

    bool sawType = false;
    for (const Entry& e : m_entries) {
        if (e.keyType != keyType || !entryHostMatches(e.host, host))
            continue;
        // @revoked entries were handled above; a revoked entry never establishes
        // trust, so other keys for the same host stay Unknown through it.
        if (e.marker == QLatin1String("@revoked"))
            continue;
        // @cert-authority entries are not direct host keys: never a source of
        // Match/Mismatch. Hashed (|1|) entries DO participate — entryHostMatches
        // resolves them via HMAC-SHA1 above.
        if (e.marker == QLatin1String("@cert-authority"))
            continue;
        sawType = true;
        if (e.key == keyBlob)
            return Verdict::Match;
    }
    // Same host+keyType seen but no blob matched: the key changed -> refuse.
    return sawType ? Verdict::Mismatch : Verdict::Unknown;
}

void KnownHosts::add(const QString& host, const QString& keyType,
                     const QByteArray& keyBlob)
{
    for (Entry& e : m_entries) {
        if (e.supported && e.host == host && e.keyType == keyType) {
            e.key = keyBlob;
            return;
        }
    }
    m_entries.append(Entry{host, keyType, keyBlob, true, QString(), QString()});
}

QByteArray KnownHosts::serialize() const
{
    QByteArray out;
    for (const Entry& e : m_entries) {
        if (!e.marker.isEmpty()) {
            out += e.marker.toUtf8();
            out += ' ';
        }
        out += e.host.toUtf8();
        out += ' ';
        out += e.keyType.toUtf8();
        out += ' ';
        out += e.key.toBase64();
        if (!e.comment.isEmpty()) {
            out += ' ';
            out += e.comment.toUtf8();
        }
        out += '\n';
    }
    return out;
}

KnownHosts KnownHosts::parse(const QString& text)
{
    KnownHosts store;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        QStringList fields =
            line.split(QRegularExpression(QStringLiteral("\\s+")),
                       Qt::SkipEmptyParts);
        int idx = 0;
        // Capture an optional leading marker (@cert-authority, @revoked).
        QString marker;
        if (idx < fields.size() && fields.at(idx).startsWith(QLatin1Char('@'))) {
            marker = fields.at(idx);
            ++idx;
        }
        if (fields.size() - idx < 3)
            continue;

        const QString hostField = fields.at(idx);
        const QString keyType = fields.at(idx + 1);
        const auto decoded = QByteArray::fromBase64Encoding(
            fields.at(idx + 2).toUtf8(),
            QByteArray::Base64Encoding
                | QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded)
            continue;  // malformed base64: drop the line, don't store a bogus key
        const QByteArray key = decoded.decoded;
        QString comment;
        for (int i = idx + 3; i < fields.size(); ++i) {
            if (!comment.isEmpty())
                comment += QLatin1Char(' ');
            comment += fields.at(i);
        }

        // @marker entries are excluded from add()/replace (supported == false)
        // but still round-trip and are consulted by verify(). Hashed (|1|) hosts
        // are likewise supported == false for add(), yet verify() resolves them
        // by HMAC-SHA1, so they DO produce Match/Mismatch (and honor @revoked).
        const bool opaque = !marker.isEmpty();
        if (hostField.startsWith(QLatin1Char('|'))) {
            store.m_entries.append(
                Entry{hostField, keyType, key, false, comment, marker});
            continue;
        }

        // A single line may list several comma-separated hosts sharing one key.
        const QStringList hosts =
            hostField.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& host : hosts)
            store.m_entries.append(
                Entry{host, keyType, key, !opaque, comment, marker});
    }
    return store;
}

const QList<KnownHosts::Entry>& KnownHosts::entries() const
{
    return m_entries;
}

} // namespace ch
