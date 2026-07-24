#include "KnownHosts.h"

#include <QByteArray>
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
    void bracketedHostPortMatches();
    void revokedKeyRefused();
    void certAuthorityIsOpaque();
    void revocationWinsBeforeTrustedEntry();
    void malformedBase64LineSkipped();
    void crlfAndTabWhitespaceParsed();
    void hostnamesAreCaseSensitive();
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
    // Present host but an absent key type is also Unknown.
    QCOMPARE(store.verify(QStringLiteral("example.com"),
                          QStringLiteral("ssh-dss"), kEd25519Alpha),
             KnownHosts::Verdict::Unknown);
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
    // A hashed |1| entry never matches (its host is opaque)...
    QCOMPARE(store.verify(QStringLiteral("|1|abc=|def="),
                          QStringLiteral("ssh-ed25519"), kEd25519Beta),
             KnownHosts::Verdict::Unknown);
    // ...but it survives a serialize round-trip.
    QVERIFY(store.serialize().contains("|1|abc=|def="));
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

QTEST_MAIN(TstKnownHosts)
#include "tst_knownhosts.moc"
