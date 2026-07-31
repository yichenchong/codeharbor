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

// OpenSSH's known_hosts host field is a pattern list, not always a literal
// name: `*` stands for any run of characters, `?` for exactly one, and a
// leading `!` on a token excludes the hosts that token covers. Report whether a
// field uses any of that syntax: such a field can be matched, but never
// compared for equality nor replaced by add().
bool hasHostPatternSyntax(const QString& hostField)
{
    return hostField.contains(QLatin1Char('*'))
           || hostField.contains(QLatin1Char('?'))
           || hostField.contains(QLatin1Char('!'));
}

// OpenSSH's match_pattern(): `*` matches any run of characters (including
// none), `?` matches exactly one, every other character is literal, and the
// comparison is case-insensitive. Iterative with a single backtrack point, so
// an adversarial pattern cannot recurse the stack away.
bool globMatches(const QString& pattern, const QString& host)
{
    qsizetype p = 0;
    qsizetype h = 0;
    qsizetype star = -1;   // index of the last '*' seen, -1 while none
    qsizetype resume = 0;  // how far into `host` that '*' has consumed
    while (h < host.size()) {
        // `*` FIRST: it is a metacharacter, so a looked-up name that itself
        // contains a literal '*' at the same offset must not be allowed to
        // consume it as an ordinary character. Testing equality first made
        // "web*.example.com" fail to cover a lookup of "web*x.example.com" —
        // the entry then named no host at all and a different key there read as
        // first use (Unknown) instead of the refusal (Mismatch) a covered host
        // is owed.
        if (p < pattern.size() && pattern.at(p) == QLatin1Char('*')) {
            star = p++;
            resume = h;
        } else if (p < pattern.size()
                   && (pattern.at(p) == QLatin1Char('?')
                       || pattern.at(p).toCaseFolded()
                              == host.at(h).toCaseFolded())) {
            ++p;
            ++h;
        } else if (star >= 0) {
            // Backtrack: let the most recent '*' swallow one more character.
            p = star + 1;
            h = ++resume;
        } else {
            return false;
        }
    }
    // Trailing '*'s may match nothing at all.
    while (p < pattern.size() && pattern.at(p) == QLatin1Char('*'))
        ++p;
    return p == pattern.size();
}

// OpenSSH pattern-list semantics: comma-separated patterns where a match on any
// negated (`!`) token rejects the whole entry outright, and one positive match
// accepts it. The negation is the only thing that stops a wildcard from
// covering a host the file deliberately carves out
// ("*.example.com,!secret.example.com"), so the list must be evaluated as a
// whole rather than one token at a time.
bool patternListMatches(const QString& hostField, const QString& host)
{
    bool matched = false;
    const QStringList tokens =
        hostField.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (token.startsWith(QLatin1Char('!'))) {
            if (globMatches(token.sliced(1), host))
                return false;
        } else if (globMatches(token, host)) {
            matched = true;
        }
    }
    return matched;
}

// A stored entry names `host` if it matches the plaintext token (compared
// case-insensitively, as OpenSSH does), the pattern list of a wildcard or
// negated token, or, for a hashed |1| token, the HMAC-SHA1 salted hash of the
// hostname.
bool entryHostMatches(const QString& storedHost, const QString& host)
{
    if (storedHost.startsWith(QLatin1Char('|')))
        return hashedHostMatches(storedHost, host);
    if (hasHostPatternSyntax(storedHost))
        return patternListMatches(storedHost, host);
    return storedHost.compare(host, Qt::CaseInsensitive) == 0;
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

    // Does this host have ANY trusted entry? That question, not "any entry of
    // this key TYPE", is what separates first use from a changed key. Keying
    // the refusal on host+keyType let a MITM pick the type: a host trusted for
    // ssh-ed25519 that suddenly presents ssh-rsa read as Unknown, and the user
    // was shown the reassuring first-use prompt instead of the hard refusal.
    bool sawTrustedHost = false;
    for (const Entry& e : m_entries) {
        if (!entryHostMatches(e.host, host))
            continue;
        // Neither marker is a host key we would ever accept, so neither may
        // establish the trust that turns an unrelated key into a Mismatch:
        // @revoked entries were handled above, and @cert-authority entries are
        // opaque (a CA is not a direct host key).
        if (e.marker == QLatin1String("@revoked")
            || e.marker == QLatin1String("@cert-authority"))
            continue;
        sawTrustedHost = true;
        if (e.keyType == keyType && e.key == keyBlob)
            return Verdict::Match;
    }
    // The host is trusted, but not with this key: the key changed, or the
    // server switched to a type we never trusted. Both are refusals.
    return sawTrustedHost ? Verdict::Mismatch : Verdict::Unknown;
}


void KnownHosts::add(const QString& host, const QString& keyType,
                     const QByteArray& keyBlob)
{
    // A line needs all three fields to survive serialize()/parse(): an empty
    // host, key type or blob writes "host type\n" (or worse), which parse()
    // then drops as malformed — the trust would be silently gone on the next
    // launch and the user asked to approve the same key all over again. Refuse
    // to record something that cannot be stored instead.
    if (host.isEmpty() || keyType.isEmpty() || keyBlob.isEmpty())
        return;

    for (Entry& e : m_entries) {
        // Case-insensitively, because that is how verify() matches: a store
        // holding "Host.Example" must not gain a second "host.example" line
        // whose key silently contradicts the first.
        if (e.supported && e.host.compare(host, Qt::CaseInsensitive) == 0
            && e.keyType == keyType) {
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
        // A wildcard/negated host field is kept whole for the same reason plus
        // one of its own: splitting "*.example.com,!secret.example.com" on the
        // comma would strand the negated token as a host name of its own, and
        // the surviving wildcard would then cover the very host the file carves
        // out — a key accepted for a host the administrator excluded.
        const bool opaque = !marker.isEmpty();
        if (hostField.startsWith(QLatin1Char('|'))
            || hasHostPatternSyntax(hostField)) {
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
