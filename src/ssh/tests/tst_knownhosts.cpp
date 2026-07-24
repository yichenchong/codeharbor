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

QTEST_MAIN(TstKnownHosts)
#include "tst_knownhosts.moc"
