#include "KnownHosts.h"
#include "SshConnectionPool.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QString>
#include <QtTest/QtTest>

using ch::KnownHosts;

namespace {

// Base64 blobs used across the sample store. Held decoded for direct
// comparison against KnownHosts entries.
const QByteArray kEd25519Alpha =
    QByteArray::fromBase64("ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==");
const QByteArray kEd25519Beta =
    QByteArray::fromBase64("ZWQyNTUxOS1rZXktYmV0YS0wMDAy");
const QByteArray kRsaGamma =
    QByteArray::fromBase64("cnNhLWtleS1nYW1tYS0wMDAz");
const QByteArray kEd25519Delta =
    QByteArray::fromBase64("ZWQyNTUxOS1rZXktZGVsdGEtMDAwNA==");

QString sampleText()
{
    return QStringLiteral(
        "# sample known_hosts\n"
        "\n"
        "example.com ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"
        "example.com ssh-rsa cnNhLWtleS1nYW1tYS0wMDAz comment-here\n"
        "other.host,10.0.0.5 ssh-ed25519 ZWQyNTUxOS1rZXktZGVsdGEtMDAwNA==\n"
        "|1|abc=|def= ssh-ed25519 ZWQyNTUxOS1rZXktYmV0YS0wMDAy\n");
}

// Build an OpenSSH hashed-host token "|1|<b64 salt>|<b64 hmac-sha1(host)>" the
// same way ssh-keygen -H does, so tests exercise the real hashed match path.
QString hashedHost(const QString& host, const QByteArray& salt)
{
    const QByteArray mac = QMessageAuthenticationCode::hash(
        host.toUtf8(), salt, QCryptographicHash::Sha1);
    return QStringLiteral("|1|%1|%2")
        .arg(QString::fromUtf8(salt.toBase64()),
             QString::fromUtf8(mac.toBase64()));
}

} // namespace

class TstKnownHosts : public QObject {
    Q_OBJECT
private slots:
    void parsesSampleStore();
    void matchesKnownKey();
    void mismatchOnChangedKey();
    void unknownForAbsentHost();
    void multipleKeyTypesPerHost();
    void commaSeparatedHostsExpand();
    void addThenVerifyMatches();
    void serializeRoundTrips();
    void emptyStoreIsUnknown();
    void hashedEntryIsOpaqueButPreserved();
    void hashedHostMatchesAndMismatches();
    void hashedRevokedKeyRefused();
    void bracketedHostPortMatches();
    void revokedKeyRefused();
    void certAuthorityIsOpaque();
    void revocationWinsBeforeTrustedEntry();
    void malformedBase64LineSkipped();
    void crlfAndTabWhitespaceParsed();
    void hostnamesAreCaseSensitive();
    void trustedHostRefusesUnknownKeyType();
    void markerOnlyHostStaysUnknownForOtherTypes();
    void keyTypesForListsTrustedTypesOnly();
    void hostKeyAlgorithmsPinTrustedTypes();
};

void TstKnownHosts::parsesSampleStore()
{
    const KnownHosts store = KnownHosts::parse(sampleText());
    // example.com (ed25519 + rsa), other.host, 10.0.0.5, and the hashed entry.
    QCOMPARE(store.entries().size(), 5);
}

void TstKnownHosts::matchesKnownKey()
{
    const KnownHosts store = KnownHosts::parse(sampleText());
    QCOMPARE(store.verify(QStringLiteral("example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
}

void TstKnownHosts::mismatchOnChangedKey()
{
    const KnownHosts store = KnownHosts::parse(sampleText());
    // Same host + key type, different blob -> changed key, must refuse.
    QCOMPARE(store.verify(QStringLiteral("example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Mismatch);
}

void TstKnownHosts::unknownForAbsentHost()
{
    const KnownHosts store = KnownHosts::parse(sampleText());
    QCOMPARE(store.verify(QStringLiteral("absent.example"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
    // A host we already trust is NOT unknown just because the presented key is
    // of a type we have no entry for — see trustedHostRefusesUnknownKeyType().
    QCOMPARE(store.verify(QStringLiteral("example.com"),
                          QStringLiteral("ssh-dss"), kEd25519Alpha),
             KnownHosts::Verdict::Mismatch);
}

void TstKnownHosts::multipleKeyTypesPerHost()
{
    const KnownHosts store = KnownHosts::parse(sampleText());
    QCOMPARE(store.verify(QStringLiteral("example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("example.com"),
                          QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Match);
}

void TstKnownHosts::commaSeparatedHostsExpand()
{
    const KnownHosts store = KnownHosts::parse(sampleText());
    QCOMPARE(store.verify(QStringLiteral("other.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Delta),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("10.0.0.5"),
                          QStringLiteral("ssh-ed25519"), kEd25519Delta),
             KnownHosts::Verdict::Match);
}

void TstKnownHosts::addThenVerifyMatches()
{
    KnownHosts store;
    QCOMPARE(store.verify(QStringLiteral("new.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
    store.add(QStringLiteral("new.host"), QStringLiteral("ssh-ed25519"),
              kEd25519Alpha);
    QCOMPARE(store.verify(QStringLiteral("new.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);

    // Re-adding a changed key for the same host+type replaces (explicit trust).
    store.add(QStringLiteral("new.host"), QStringLiteral("ssh-ed25519"),
              kEd25519Beta);
    QCOMPARE(store.verify(QStringLiteral("new.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Match);
}

void TstKnownHosts::serializeRoundTrips()
{
    const KnownHosts store = KnownHosts::parse(sampleText());
    const QByteArray text = store.serialize();
    const KnownHosts reparsed =
        KnownHosts::parse(QString::fromUtf8(text));

    QCOMPARE(reparsed.entries().size(), store.entries().size());
    QCOMPARE(reparsed.verify(QStringLiteral("example.com"),
                             QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(reparsed.verify(QStringLiteral("example.com"),
                             QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Match);
    QCOMPARE(reparsed.verify(QStringLiteral("10.0.0.5"),
                             QStringLiteral("ssh-ed25519"), kEd25519Delta),
             KnownHosts::Verdict::Match);
}

void TstKnownHosts::emptyStoreIsUnknown()
{
    const KnownHosts store = KnownHosts::parse(QString());
    QCOMPARE(store.entries().size(), 0);
    QCOMPARE(store.verify(QStringLiteral("anything"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
}

void TstKnownHosts::hashedEntryIsOpaqueButPreserved()
{
    const KnownHosts store = KnownHosts::parse(sampleText());
    // A hashed |1| entry with an unrelated/bogus salt+hash resolves to no host,
    // so a lookup keyed by the literal token yields Unknown...
    QCOMPARE(store.verify(QStringLiteral("|1|abc=|def="),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Unknown);
    // ...but it survives a serialize round-trip.
    QVERIFY(store.serialize().contains("|1|abc=|def="));
}

void TstKnownHosts::hashedHostMatchesAndMismatches()
{
    // A store with ONLY a hashed entry must still detect a changed key: the
    // hostname is recovered via HMAC-SHA1 so a different blob => Mismatch, never
    // a first-use Unknown prompt (SPEC 12.1).
    const QByteArray salt = QByteArrayLiteral("codeharbor-hash-salt");
    const KnownHosts store = KnownHosts::parse(
        hashedHost(QStringLiteral("secret.host"), salt)
        + QStringLiteral(" ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    // The stored key matches the hashed host.
    QCOMPARE(store.verify(QStringLiteral("secret.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    // A changed key at that hashed host is refused.
    QCOMPARE(store.verify(QStringLiteral("secret.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Mismatch);
    // An unrelated host does not collide with the hash.
    QCOMPARE(store.verify(QStringLiteral("other.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
}

void TstKnownHosts::hashedRevokedKeyRefused()
{
    // A hashed @revoked entry must participate in the revocation precedence loop:
    // presenting the revoked key at the hashed host is refused (Mismatch).
    const QByteArray salt = QByteArrayLiteral("codeharbor-hash-salt");
    const KnownHosts store = KnownHosts::parse(
        QStringLiteral("@revoked ")
        + hashedHost(QStringLiteral("revoked.host"), salt)
        + QStringLiteral(" ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(store.verify(QStringLiteral("revoked.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Mismatch);
    // A different key at that host is merely Unknown (revocation grants no trust).
    QCOMPARE(store.verify(QStringLiteral("revoked.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Unknown);
}

void TstKnownHosts::bracketedHostPortMatches()
{
    // OpenSSH stores non-default ports as "[host]:port"; the exact token is
    // matched (and the bare host must NOT match a ported entry).
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "[example.com]:2222 ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(store.entries().size(), 1);
    QCOMPARE(store.verify(QStringLiteral("[example.com]:2222"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
    // The bracketed token survives serialization.
    QVERIFY(store.serialize().contains("[example.com]:2222 "));
}

void TstKnownHosts::revokedKeyRefused()
{
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "@revoked bad.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    // Presenting the revoked key must be refused, never accepted.
    QCOMPARE(store.verify(QStringLiteral("bad.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Mismatch);
    // A different key for that host is merely Unknown (no trusted entry).
    QCOMPARE(store.verify(QStringLiteral("bad.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Unknown);
    // The marker is preserved on serialize.
    QVERIFY(store.serialize().contains("@revoked "));
}

void TstKnownHosts::certAuthorityIsOpaque()
{
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "@cert-authority ca.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    // A CA entry is not a direct host key: never Match/Mismatch on blob compare.
    QCOMPARE(store.verify(QStringLiteral("ca.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
    QVERIFY(store.serialize().contains("@cert-authority "));
}

void TstKnownHosts::revocationWinsBeforeTrustedEntry()
{
    // A trusted entry precedes an @revoked entry for the SAME key. Revocation
    // must win regardless of file order (OpenSSH semantics), so the earlier
    // trusted line must not shadow the later revocation.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "host.example ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"
        "@revoked host.example ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(store.verify(QStringLiteral("host.example"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Mismatch);
    // The surviving trusted entry still detects a changed (non-revoked) key.
    QCOMPARE(store.verify(QStringLiteral("host.example"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Mismatch);
}

void TstKnownHosts::malformedBase64LineSkipped()
{
    // A line whose base64 key field is invalid is dropped entirely rather than
    // stored as a bogus key that would poison verify() (Unknown -> Mismatch).
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "good.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"
        "bad.host ssh-ed25519 ****not-valid-base64****\n"));
    QCOMPARE(store.entries().size(), 1);
    QCOMPARE(store.verify(QStringLiteral("good.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    // The dropped line must NOT turn an unknown host into a hard Mismatch.
    QCOMPARE(store.verify(QStringLiteral("bad.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
}

void TstKnownHosts::crlfAndTabWhitespaceParsed()
{
    // Tabs as field separators and CRLF line endings must parse cleanly.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "host.crlf\tssh-ed25519\tZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\r\n"));
    QCOMPARE(store.entries().size(), 1);
    QCOMPARE(store.verify(QStringLiteral("host.crlf"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
}

void TstKnownHosts::hostnamesAreCaseSensitive()
{
    // OpenSSH matches hostnames case-sensitively; the stored case is preserved
    // and a differently-cased lookup must not match.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "Host.Example ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(store.verify(QStringLiteral("Host.Example"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("host.example"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
}

void TstKnownHosts::trustedHostRefusesUnknownKeyType()
{
    // THE DOWNGRADE. The user trusted this endpoint's ed25519 key. A MITM that
    // cannot reproduce it presents an RSA key instead. Keyed on host+keyType
    // that read as Unknown, so SshConnectionPool called the host-key callback
    // and the user was shown the reassuring "new host, trust it?" prompt — the
    // hard refusal of SPEC 12.1 bypassed by simply picking another algorithm.
    //
    // Unknown is what makes it exploitable: it is the ONLY verdict that reaches
    // m_hostKeyCallback and the only one after which knownHosts.add() persists
    // the presented key.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "[victim.example]:2222 ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));

    QCOMPARE(store.verify(QStringLiteral("[victim.example]:2222"),
                          QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Mismatch);
    // Every other type the attacker might reach for, same answer.
    for (const QString& type : {QStringLiteral("ecdsa-sha2-nistp256"),
                                QStringLiteral("ssh-dss"),
                                QStringLiteral("sk-ssh-ed25519@openssh.com")}) {
        QCOMPARE(store.verify(QStringLiteral("[victim.example]:2222"), type,
                              kEd25519Beta),
                 KnownHosts::Verdict::Mismatch);
    }
    // The genuine key still matches, and an unrelated host is still first-use.
    QCOMPARE(store.verify(QStringLiteral("[victim.example]:2222"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("[other.example]:2222"),
                          QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Unknown);

    // A hashed host must not be a way around it: the same refusal applies once
    // the HMAC resolves the name.
    const QByteArray salt = QByteArrayLiteral("codeharbor-hash-salt");
    const KnownHosts hashed = KnownHosts::parse(
        hashedHost(QStringLiteral("victim.example"), salt)
        + QStringLiteral(" ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(hashed.verify(QStringLiteral("victim.example"),
                           QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Mismatch);
}

void TstKnownHosts::markerOnlyHostStaysUnknownForOtherTypes()
{
    // The refusal above keys on TRUST, and neither marker is trust. Tightening
    // it into "the host appears anywhere in the file" would make a revoked or
    // CA-only host permanently unconnectable instead of first-use.
    const KnownHosts revoked = KnownHosts::parse(QStringLiteral(
        "@revoked bad.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(revoked.verify(QStringLiteral("bad.host"),
                            QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Unknown);
    // ...but the revoked key itself is still refused, whatever else is asked.
    QCOMPARE(revoked.verify(QStringLiteral("bad.host"),
                            QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Mismatch);

    const KnownHosts ca = KnownHosts::parse(QStringLiteral(
        "@cert-authority ca.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(ca.verify(QStringLiteral("ca.host"), QStringLiteral("ssh-rsa"),
                       kRsaGamma),
             KnownHosts::Verdict::Unknown);
}

void TstKnownHosts::keyTypesForListsTrustedTypesOnly()
{
    const KnownHosts store = KnownHosts::parse(sampleText());
    // File order, deduplicated, both types for the multi-key host.
    QCOMPARE(store.keyTypesFor(QStringLiteral("example.com")),
             (QStringList{QStringLiteral("ssh-ed25519"), QStringLiteral("ssh-rsa")}));
    // A comma-expanded alias carries the same trust as its sibling.
    QCOMPARE(store.keyTypesFor(QStringLiteral("10.0.0.5")),
             (QStringList{QStringLiteral("ssh-ed25519")}));
    // Nothing trusted -> nothing to pin, so first use negotiates freely.
    QVERIFY(store.keyTypesFor(QStringLiteral("absent.example")).isEmpty());

    // Markers are not trust and must never pin the negotiation: pinning to a
    // revoked key's type would hand the attacker the choice back.
    const KnownHosts markers = KnownHosts::parse(QStringLiteral(
        "@revoked m.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"
        "@cert-authority m.host ssh-rsa cnNhLWtleS1nYW1tYS0wMDAz\n"));
    QVERIFY(markers.keyTypesFor(QStringLiteral("m.host")).isEmpty());

    // Hashed hosts resolve through the HMAC here too, so a hashed known_hosts
    // still gets its algorithms pinned.
    const QByteArray salt = QByteArrayLiteral("codeharbor-hash-salt");
    const KnownHosts hashed = KnownHosts::parse(
        hashedHost(QStringLiteral("secret.host"), salt)
        + QStringLiteral(" ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(hashed.keyTypesFor(QStringLiteral("secret.host")),
             (QStringList{QStringLiteral("ssh-ed25519")}));
}

void TstKnownHosts::hostKeyAlgorithmsPinTrustedTypes()
{
    using ch::SshConnectionPool;

    // Nothing trusted -> empty, and connectToHost() then leaves libssh's
    // default ordering alone so a fresh host still connects.
    QVERIFY(SshConnectionPool::hostKeyAlgorithms({}).isEmpty());

    QCOMPARE(SshConnectionPool::hostKeyAlgorithms({QStringLiteral("ssh-ed25519")}),
             QByteArrayLiteral("ssh-ed25519"));

    // "ssh-rsa" is a key TYPE covering three host-key ALGORITHMS. Pinning the
    // literal string alone would offer only the SHA-1 algorithm, which OpenSSH
    // 8.8+ disables by default — the pin would lock the user out of their own
    // RSA host. SHA-2 first so the modern algorithm is preferred.
    QCOMPARE(SshConnectionPool::hostKeyAlgorithms({QStringLiteral("ssh-rsa")}),
             QByteArrayLiteral("rsa-sha2-512,rsa-sha2-256,ssh-rsa"));

    // Several trusted types keep file order; nothing is duplicated.
    QCOMPARE(SshConnectionPool::hostKeyAlgorithms(
                 {QStringLiteral("ssh-ed25519"), QStringLiteral("ssh-rsa"),
                  QStringLiteral("ssh-ed25519")}),
             QByteArrayLiteral("ssh-ed25519,rsa-sha2-512,rsa-sha2-256,ssh-rsa"));

    // The pin is derived from the SAME token the verdict uses, or a ported host
    // would be pinned from an entry that does not govern it.
    QCOMPARE(SshConnectionPool::lookupHostFor(QStringLiteral("h.example"), 22),
             QStringLiteral("h.example"));
    QCOMPARE(SshConnectionPool::lookupHostFor(QStringLiteral("h.example"), 2222),
             QStringLiteral("[h.example]:2222"));

    // End to end on the store: a host trusted only for ed25519 pins exactly
    // that, so a server offering RSA cannot even reach the verdict.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "[victim.example]:2222 ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(SshConnectionPool::hostKeyAlgorithms(store.keyTypesFor(
                 SshConnectionPool::lookupHostFor(QStringLiteral("victim.example"),
                                                  2222))),
             QByteArrayLiteral("ssh-ed25519"));
}

QTEST_MAIN(TstKnownHosts)
#include "tst_knownhosts.moc"
