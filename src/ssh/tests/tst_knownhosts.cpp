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
    void malformedHashedHostTokenNeverMatches();
    void hashedHostMatchesAndMismatches();
    void hashedRevokedKeyRefused();
    void bracketedHostPortMatches();
    void revokedKeyRefused();
    void certAuthorityIsOpaque();
    void revocationWinsBeforeTrustedEntry();
    void anUnrecognisedMarkerLineIsDropped();
    void twoKeysOfOneTypeForOneHostBothMatch();
    void ipv6LiteralEndpointsAreMatchedAndCanonicalized();
    void malformedBase64LineSkipped();
    void emptyBase64LineSkipped();
    void crlfAndTabWhitespaceParsed();
    void hostnamesMatchCaseInsensitively();
    void wildcardHostIsATrustedHost();
    void negatedWildcardHostIsExcluded();
    void wildcardCoversAHostSpelledWithAnAsterisk();
    void commentsAndMarkersSurviveARoundTrip();
    void addIgnoresATripleThatCannotBeStored();
    void addRejectsNonPlainHostTokens();

    void lookupHostCanonicalizesTheEndpoint();
    void trustedHostRefusesUnknownKeyType();
    void markerOnlyHostStaysUnknownForOtherTypes();
    void recognizesWindowsNamedPipeAgentSocket();
    void recognizesLibsshVersionsWithBrokenHybridKex();
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

// A hashed host token whose salt or hash is not valid base64 names no host at
// all. The decode has to be STRICT for that to hold, and the difference is not
// theoretical: Qt's lenient decoder SILENTLY DISCARDS characters outside the
// base64 alphabet, so "Y2-9kZWhhcmJvcg==" decodes to exactly the same bytes as
// "Y29kZWhhcmJvcg==". A token corrupted in transit — or crafted — therefore
// resolved to a real hostname and its key line was honoured for that host,
// while OpenSSH itself would have rejected the line outright. The two tools
// must agree about which lines in the user's known_hosts file count.
void TstKnownHosts::malformedHashedHostTokenNeverMatches()
{
    const QByteArray salt = QByteArrayLiteral("codeharbor-hash-salt");
    const QString wellFormed = hashedHost(QStringLiteral("secret.host"), salt);
    const QString keyFields =
        QStringLiteral(" ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n");

    // Control: the intact token DOES match, so the assertions below fail for
    // the corruption and not for the fixture.
    QCOMPARE(KnownHosts::parse(wellFormed + keyFields)
                 .verify(QStringLiteral("secret.host"),
                         QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);

    const QStringList parts = wellFormed.split(QLatin1Char('|'));
    QCOMPARE(parts.size(), 4);
    // '-' is not in the standard base64 alphabet, so each of these tokens is
    // malformed while still decoding — leniently — to the correct bytes.
    const QString dirtyHash = QStringLiteral("|1|%1|%2").arg(
        parts.at(2), QString(parts.at(3)).insert(2, QLatin1Char('-')));
    const QString dirtySalt = QStringLiteral("|1|%1|%2").arg(
        QString(parts.at(2)).insert(2, QLatin1Char('-')), parts.at(3));
    // And one that is simply unreadable either way.
    const QString rubbish = QStringLiteral("|1|***|(((not-b64");

    for (const QString& token : {dirtyHash, dirtySalt, rubbish}) {
        const KnownHosts store = KnownHosts::parse(token + keyFields);
        QCOMPARE(store.entries().size(), 1);
        QCOMPARE(store.verify(QStringLiteral("secret.host"),
                              QStringLiteral("ssh-ed25519"), kEd25519Alpha),
                 KnownHosts::Verdict::Unknown);
        // Unreadable, but still the user's file: the line survives a round trip
        // rather than being quietly rewritten or dropped.
        QVERIFY(store.serialize().contains(token.toUtf8()));
    }
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

// OpenSSH knows exactly two markers and skips any line that begins with a
// different @token. Accepting an unknown one here was a silent trust upgrade,
// because verify() only withholds trust for the two markers it recognises: a
// revocation whose marker was mistyped — or merely mis-cased, "@Revoked", which
// is the likeliest way an administrator gets this wrong — stopped revoking and
// became an ordinary trusted host key for exactly the key that was meant to be
// refused. The presented key then verified as Match and CodeHarbor connected.
void TstKnownHosts::anUnrecognisedMarkerLineIsDropped()
{
    for (const QString& marker : {QStringLiteral("@Revoked"),
                                  QStringLiteral("@revocked"),
                                  QStringLiteral("@CERT-AUTHORITY"),
                                  QStringLiteral("@some-future-marker")}) {
        const KnownHosts store = KnownHosts::parse(
            marker
            + QStringLiteral(" bad.host ssh-ed25519 "
                             "ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
        QVERIFY2(store.entries().isEmpty(), qPrintable(marker));
        // Above all: the key on that line is NOT trusted.
        QCOMPARE(store.verify(QStringLiteral("bad.host"),
                              QStringLiteral("ssh-ed25519"), kEd25519Alpha),
                 KnownHosts::Verdict::Unknown);
    }

    // The two real markers still work, and a dropped line does not take the
    // rest of the file with it.
    const KnownHosts mixed = KnownHosts::parse(QStringLiteral(
        "@Revoked bad.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"
        "@revoked bad.host ssh-ed25519 ZWQyNTUxOS1rZXktYmV0YS0wMDAy\n"
        "good.host ssh-rsa cnNhLWtleS1nYW1tYS0wMDAz\n"));
    QCOMPARE(mixed.entries().size(), 2);
    QCOMPARE(mixed.verify(QStringLiteral("bad.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Mismatch);
    QCOMPARE(mixed.verify(QStringLiteral("good.host"),
                          QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Match);
}

// A key rotation leaves a real known_hosts file holding two lines of the SAME
// type for one host for as long as both keys are in service. Both must verify:
// stopping at the first entry of a matching type would turn the second, equally
// trusted key into a hard refusal and lock the user out of their own server.
void TstKnownHosts::twoKeysOfOneTypeForOneHostBothMatch()
{
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "rotating.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"
        "rotating.host ssh-ed25519 ZWQyNTUxOS1rZXktYmV0YS0wMDAy\n"));
    QCOMPARE(store.entries().size(), 2);
    QCOMPARE(store.verify(QStringLiteral("rotating.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("rotating.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Match);
    // A third, unlisted key is still the hard refusal.
    QCOMPARE(store.verify(QStringLiteral("rotating.host"),
                          QStringLiteral("ssh-ed25519"), kEd25519Delta),
             KnownHosts::Verdict::Mismatch);

    // add() rewrites the FIRST entry it finds for the host+type and leaves the
    // rest of the file alone: the newly approved key is trusted, the second
    // rotation key keeps its own line, and no contradictory duplicate is
    // appended. (Unreachable from the app — the pool only calls add() after a
    // Verdict::Unknown, which a host with trusted entries never produces — but
    // it is the store's public contract.)
    KnownHosts trusted = store;
    trusted.add(QStringLiteral("rotating.host"), QStringLiteral("ssh-ed25519"),
                kEd25519Delta);
    QCOMPARE(trusted.entries().size(), 2);
    QCOMPARE(trusted.verify(QStringLiteral("rotating.host"),
                            QStringLiteral("ssh-ed25519"), kEd25519Delta),
             KnownHosts::Verdict::Match);
    QCOMPARE(trusted.verify(QStringLiteral("rotating.host"),
                            QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Match);
    QCOMPARE(trusted.verify(QStringLiteral("rotating.host"),
                            QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Mismatch);
}

// An IPv6 address is a host name like any other here, but its colons collide
// visually with OpenSSH's "[host]:port" spelling, so both forms are pinned.
void TstKnownHosts::ipv6LiteralEndpointsAreMatchedAndCanonicalized()
{
    using ch::SshConnectionPool;

    // Default port: OpenSSH stores the bare address, brackets and all absent.
    QCOMPARE(SshConnectionPool::lookupHostFor(QStringLiteral("2001:db8::1"), 22),
             QStringLiteral("2001:db8::1"));
    QCOMPARE(
        SshConnectionPool::lookupHostFor(QStringLiteral("2001:db8::1"), 2222),
        QStringLiteral("[2001:db8::1]:2222"));

    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "2001:db8::1 ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"
        "[2001:db8::1]:2222 ssh-rsa cnNhLWtleS1nYW1tYS0wMDAz\n"
        "::1 ssh-ed25519 ZWQyNTUxOS1rZXktZGVsdGEtMDAwNA==\n"));
    QCOMPARE(store.entries().size(), 3);
    QCOMPARE(store.verify(QStringLiteral("2001:db8::1"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("[2001:db8::1]:2222"),
                          QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("::1"),
                          QStringLiteral("ssh-ed25519"), kEd25519Delta),
             KnownHosts::Verdict::Match);
    // The two spellings of the same address are DIFFERENT endpoints: the ported
    // one must not inherit the default port's trust, or a server moved to
    // another port on the same host would silently reuse the old key.
    QCOMPARE(store.verify(QStringLiteral("2001:db8::1"),
                          QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Mismatch);
    QCOMPARE(store.verify(QStringLiteral("[2001:db8::1]:3333"),
                          QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Unknown);

    // And the canonical token round-trips through add()/serialize().
    KnownHosts fresh;
    fresh.add(SshConnectionPool::lookupHostFor(QStringLiteral("fe80::1%eth0"),
                                               2200),
              QStringLiteral("ssh-ed25519"), kEd25519Alpha);
    const KnownHosts reparsed =
        KnownHosts::parse(QString::fromUtf8(fresh.serialize()));
    QCOMPARE(reparsed.verify(QStringLiteral("[fe80::1%eth0]:2200"),
                             QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
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

void TstKnownHosts::emptyBase64LineSkipped()
{
    // Qt's strict decoder considers an empty string valid base64, but an empty
    // key blob is not a usable known_hosts key and must not establish trust.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "good.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"
        "empty.host ssh-ed25519\n"));
    QCOMPARE(store.entries().size(), 1);
    QCOMPARE(store.verify(QStringLiteral("empty.host"),
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

void TstKnownHosts::hostnamesMatchCaseInsensitively()
{
    // OpenSSH's match_pattern() compares host names case-insensitively, and the
    // strictness is the point: if "Host.Example" did not match a lookup of
    // "host.example", a CHANGED key for that host would read as first use and
    // the user would be shown the reassuring trust-this-host prompt instead of
    // the hard refusal. The stored spelling is preserved either way.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "Host.Example ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(store.verify(QStringLiteral("Host.Example"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("host.example"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("HOST.EXAMPLE"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Mismatch);
    QVERIFY(store.serialize().contains("Host.Example "));

    // add() follows the same rule: re-trusting the host under another spelling
    // replaces the key instead of appending a second, contradictory line.
    KnownHosts trusted = store;
    trusted.add(QStringLiteral("host.example"), QStringLiteral("ssh-ed25519"),
                kEd25519Beta);
    QCOMPARE(trusted.entries().size(), 1);
    QCOMPARE(trusted.verify(QStringLiteral("Host.Example"),
                            QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Match);
}

void TstKnownHosts::wildcardHostIsATrustedHost()
{
    // A wildcard entry trusts every host it covers, so a DIFFERENT key at one of
    // those hosts is a changed key (Mismatch), not first use (Unknown). Treating
    // the pattern as a literal name made it match nothing at all — the same
    // downgrade trustedHostRefusesUnknownKeyType() closes for key types: the
    // friendly first-use prompt where SPEC 12.1 demands a hard refusal.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "*.example.com ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    // One entry, kept verbatim, and it survives a serialize round-trip.
    QCOMPARE(store.entries().size(), 1);
    QVERIFY(store.serialize().contains("*.example.com "));

    QCOMPARE(store.verify(QStringLiteral("build.example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("build.example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Mismatch);
    // A key of another TYPE at a covered host is refused too.
    QCOMPARE(store.verify(QStringLiteral("build.example.com"),
                          QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Mismatch);
    // Hosts the pattern does not cover stay first use: "*." needs a label, and
    // another domain is unrelated.
    QCOMPARE(store.verify(QStringLiteral("example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
    QCOMPARE(store.verify(QStringLiteral("build.example.net"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);

    // '?' stands for exactly one character, never two.
    const KnownHosts single = KnownHosts::parse(QStringLiteral(
        "web?.example.com ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(single.verify(QStringLiteral("web1.example.com"),
                           QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(single.verify(QStringLiteral("web12.example.com"),
                           QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);

    // A wildcard @revoked entry still revokes the key it names.
    const KnownHosts revoked = KnownHosts::parse(QStringLiteral(
        "@revoked *.bad.example ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(revoked.verify(QStringLiteral("one.bad.example"),
                            QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Mismatch);
}

void TstKnownHosts::negatedWildcardHostIsExcluded()
{
    // OpenSSH lets one line carve a host OUT of its own wildcard with '!'. The
    // whole field therefore has to stay in a single entry: splitting it on the
    // comma would strand the negated token as a host name of its own and leave
    // "*.example.com" trusting the very host the file excludes — that host's key
    // would then be accepted as a match.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "*.example.com,!secret.example.com ssh-ed25519 "
        "ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));
    QCOMPARE(store.entries().size(), 1);
    QCOMPARE(store.verify(QStringLiteral("build.example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("secret.example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
    QVERIFY(store.serialize().contains("*.example.com,!secret.example.com "));
}

void TstKnownHosts::wildcardCoversAHostSpelledWithAnAsterisk()
{
    // '*' is a metacharacter in the STORED pattern, and the looked-up name is
    // just data. Comparing the two for equality first let a name that happens
    // to contain '*' at the wildcard's own offset consume it as an ordinary
    // character: "web*.example.com" then covered no host at all for such a
    // lookup, and a key presented for it read as first use (Unknown) instead of
    // the refusal a covered host is owed. The lookup name is not always a
    // resolved DNS label — it is whatever the server profile says — so it must
    // not be able to steer pattern matching.
    const KnownHosts store = KnownHosts::parse(QStringLiteral(
        "web*.example.com ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ==\n"));

    QCOMPARE(store.verify(QStringLiteral("web*x.example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("web*x.example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Mismatch);
    // The ordinary covered and uncovered names are unaffected.
    QCOMPARE(store.verify(QStringLiteral("web1.example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("db1.example.com"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
}

void TstKnownHosts::commentsAndMarkersSurviveARoundTrip()
{
    // known_hosts is the user's file: CodeHarbor reads it, adds at most one
    // line, and writes the whole thing back. Anything it drops on that trip is
    // data destroyed in a file it does not own — a trailing comment naming the
    // machine, or the @cert-authority marker that makes a line a CA rather than
    // a host key.
    const QString text = QStringLiteral(
        "@cert-authority ca.host ssh-ed25519 ZWQyNTUxOS1rZXktYWxwaGEtMDAwMQ== "
        "the org CA\n"
        "plain.host ssh-rsa cnNhLWtleS1nYW1tYS0wMDAz lab machine 3\n");
    const KnownHosts store = KnownHosts::parse(text);
    QCOMPARE(store.entries().size(), 2);
    QCOMPARE(store.entries().at(0).marker, QStringLiteral("@cert-authority"));
    QCOMPARE(store.entries().at(0).comment, QStringLiteral("the org CA"));
    QCOMPARE(store.entries().at(1).marker, QString());
    QCOMPARE(store.entries().at(1).comment, QStringLiteral("lab machine 3"));

    const KnownHosts reparsed =
        KnownHosts::parse(QString::fromUtf8(store.serialize()));
    QCOMPARE(reparsed.entries().size(), 2);
    QCOMPARE(reparsed.entries().at(0).marker, QStringLiteral("@cert-authority"));
    QCOMPARE(reparsed.entries().at(0).comment, QStringLiteral("the org CA"));
    QCOMPARE(reparsed.entries().at(1).comment,
             QStringLiteral("lab machine 3"));
    QCOMPARE(reparsed.verify(QStringLiteral("plain.host"),
                             QStringLiteral("ssh-rsa"), kRsaGamma),
             KnownHosts::Verdict::Match);
}

void TstKnownHosts::addIgnoresATripleThatCannotBeStored()
{
    // A known_hosts line needs all three fields. Recording one with an empty
    // key type or blob would write "host type\n", which parse() drops as
    // malformed: verify() would answer Match for the rest of this process and
    // Unknown on the next launch, so the user would approve the same key again
    // and again with no idea why. Refusing to record it keeps the store honest.
    KnownHosts store;
    store.add(QStringLiteral("no.type"), QString(), kEd25519Alpha);
    store.add(QStringLiteral("no.blob"), QStringLiteral("ssh-ed25519"),
              QByteArray());
    store.add(QString(), QStringLiteral("ssh-ed25519"), kEd25519Alpha);
    QCOMPARE(store.entries().size(), 0);
    QCOMPARE(store.serialize(), QByteArray());
    QCOMPARE(store.verify(QStringLiteral("no.type"), QString(), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);

    // A complete triple is still recorded, and it still round-trips.
    store.add(QStringLiteral("good.host"), QStringLiteral("ssh-ed25519"),
              kEd25519Alpha);
    QCOMPARE(store.entries().size(), 1);
    const KnownHosts reparsed =
        KnownHosts::parse(QString::fromUtf8(store.serialize()));
    QCOMPARE(reparsed.verify(QStringLiteral("good.host"),
                             QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
}

void TstKnownHosts::addRejectsNonPlainHostTokens()
{
    // add() writes one host field, so known_hosts pattern/list syntax and
    // whitespace must not be accepted as though they were literal host names.
    KnownHosts store;
    for (const QString& host : {QStringLiteral("*.example.com"),
                                QStringLiteral("!excluded.example.com"),
                                QStringLiteral("|1|salt|hash"),
                                QStringLiteral("one.example,two.example"),
                                QStringLiteral("@marker-looking"),
                                QStringLiteral("#comment-looking"),
                                QStringLiteral("line\ninjection"),
                                QStringLiteral("two words"),
                                QStringLiteral("tab\tseparated"),
                                QStringLiteral("trailing.space "),
                                QStringLiteral("carriage\rreturn")}) {
        store.add(host, QStringLiteral("ssh-ed25519"), kEd25519Alpha);
    }
    // An embedded NUL would terminate the C string libssh and the file writer
    // see, silently changing which host the line names.
    QString nulHost = QStringLiteral("nul.host");
    nulHost.insert(3, QChar(u'\0'));
    store.add(nulHost, QStringLiteral("ssh-ed25519"), kEd25519Alpha);
    store.add(QStringLiteral("host.example"), QStringLiteral("ssh ed25519"),
              kEd25519Alpha);
    QCOMPARE(store.entries().size(), 0);
    QCOMPARE(store.serialize(), QByteArray());
}

void TstKnownHosts::lookupHostCanonicalizesTheEndpoint()
{
    using ch::SshConnectionPool;

    // The store is keyed by OpenSSH's endpoint spelling, and both halves matter.
    // Writing "[host]:22" for the default port would never be found again by
    // OpenSSH (or by a later CodeHarbor launch), and dropping the port for a
    // non-default one would file two different servers behind one name — the
    // second would then look like a changed key for the first.
    QCOMPARE(SshConnectionPool::lookupHostFor(QStringLiteral("example.com"), 22),
             QStringLiteral("example.com"));
    QCOMPARE(
        SshConnectionPool::lookupHostFor(QStringLiteral("example.com"), 2222),
        QStringLiteral("[example.com]:2222"));
    QCOMPARE(SshConnectionPool::lookupHostFor(QStringLiteral("10.0.0.5"), 2022),
             QStringLiteral("[10.0.0.5]:2022"));

    // And the canonical token is what a store written from it matches.
    KnownHosts store;
    store.add(SshConnectionPool::lookupHostFor(QStringLiteral("example.com"),
                                               2222),
              QStringLiteral("ssh-ed25519"), kEd25519Alpha);
    QCOMPARE(store.verify(QStringLiteral("[example.com]:2222"),
                          QStringLiteral("ssh-ed25519"), kEd25519Alpha),
             KnownHosts::Verdict::Match);
    QCOMPARE(store.verify(QStringLiteral("example.com"),
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

void TstKnownHosts::recognizesWindowsNamedPipeAgentSocket()
{
    using ch::SshConnectionPool;

    QVERIFY(SshConnectionPool::isWindowsNamedPipeAgentSocket(
        QStringLiteral("\\\\.\\pipe\\openssh-ssh-agent")));
    QVERIFY(SshConnectionPool::isWindowsNamedPipeAgentSocket(
        QStringLiteral("\\\\.\\PIPE\\agent")));
    QVERIFY(!SshConnectionPool::isWindowsNamedPipeAgentSocket(
        QStringLiteral("/tmp/ssh-xxxx/agent.42")));
    QVERIFY(!SshConnectionPool::isWindowsNamedPipeAgentSocket(
        QStringLiteral("C:/Users/alice/.ssh/agent.sock")));
    QVERIFY(!SshConnectionPool::isWindowsNamedPipeAgentSocket(QString()));
}

void TstKnownHosts::recognizesLibsshVersionsWithBrokenHybridKex()
{
    using ch::SshConnectionPool;

    // Only 0.12.0 packs its hybrid ML-KEM client KEX init wrongly. ssh_version()
    // appends the crypto/compression backends, so the release token is what
    // decides - a bare prefix match would also catch 0.12.0x releases.
    QVERIFY(SshConnectionPool::hasBrokenHybridKex(
        QStringLiteral("0.12.0/openssl/zlib")));
    QVERIFY(SshConnectionPool::hasBrokenHybridKex(QStringLiteral("0.12.0")));
    QVERIFY(!SshConnectionPool::hasBrokenHybridKex(
        QStringLiteral("0.12.1/openssl/zlib")));
    QVERIFY(!SshConnectionPool::hasBrokenHybridKex(
        QStringLiteral("0.12.2/openssl/zlib")));
    QVERIFY(!SshConnectionPool::hasBrokenHybridKex(
        QStringLiteral("0.11.3/openssl/zlib")));
    QVERIFY(!SshConnectionPool::hasBrokenHybridKex(
        QStringLiteral("0.12.10/openssl")));
    QVERIFY(!SshConnectionPool::hasBrokenHybridKex(QString()));
}

QTEST_GUILESS_MAIN(TstKnownHosts)
#include "tst_knownhosts.moc"
