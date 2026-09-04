// Credential handling for the mobile client (SPEC 11.2, 12.1).
//
// Most of this file asserts a NEGATIVE, and that is why it exists: SPEC 11.2 is
// an exhaustive allowlist of what the client may store locally, and the only key
// material on it is "an opt-in, per-key copy in app-private storage, owner-only
// and excluded from OS backup" — a copy made by an explicit
// saveKeyOnDevice(name) and by nothing else. So every test below that imports,
// authenticates with, or forgets a key WITHOUT asking for it to be saved
// finishes by walking the store's whole storage root and proving that no file
// under it contains any part of the key or of the passphrase, and
// nothingReachesThePerAppSandboxEither() extends that walk to the per-app
// QStandardPaths directories — the place a mobile client would naturally put a
// keys directory, and a place a check confined to the store's own rootDirectory
// would never have looked.
//
// A positive-only suite would pass just as happily against an implementation
// that cached the key "just for this attempt" in a temp file, or that saved
// every imported key without being asked, which are exactly the compliance
// failures worth testing for.
//
// The saved-key cases in the last section are the mirror image: they use their
// OWN temporary app-data directory, precisely so that the file they expect to
// exist cannot make the leak scans elsewhere in this file — which share one
// m_root across the whole run — report it as a leak.
//
// The key material below is a throwaway ed25519 pair generated for this file
// alone; it authenticates nothing. Its fingerprint is `ssh-keygen -lf`'s own
// output, so the expectation is a known answer produced by OpenSSH rather than
// by the code under test.

#include "MobileKeyStore.h"

#include "SshConnectionPool.h"

#include <QtTest/QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

using ch::MobileKeyStore;
using ch::SshConnectionPool;

namespace {

// ssh-keygen -t ed25519 -N '' -C ch-test
constexpr auto kPlainKey = R"(-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW
QyNTUxOQAAACCFPLr0I6E8+7CRLJi8cjh4SciVWMEXlHzF/IIDXFBLIwAAAJDD6ACHw+gA
hwAAAAtzc2gtZWQyNTUxOQAAACCFPLr0I6E8+7CRLJi8cjh4SciVWMEXlHzF/IIDXFBLIw
AAAEB/HxJkLwbaZm48TCAx96WJuyrDUfAUyv9wZzQGfttws4U8uvQjoTz7sJEsmLxyOHhJ
yJVYwReUfMX8ggNcUEsjAAAAB2NoLXRlc3QBAgMEBQY=
-----END OPENSSH PRIVATE KEY-----
)";

// `ssh-keygen -lf` on the matching .pub, verbatim.
constexpr auto kPlainFingerprint =
    "SHA256:PitEpcPx5tdS93tsxqg3GiuFpvJrlWMle3KJ6R+lr1s";

// ssh-keygen -t ed25519 -N 'hunter2' -C ch-test-enc: same container, but the
// private half is sealed with bcrypt+aes256-ctr, and the PUBLIC blob still sits
// outside the encrypted section — which is why a fingerprint is available for a
// key nobody can use yet.
constexpr auto kEncryptedKey = R"(-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAACmFlczI1Ni1jdHIAAAAGYmNyeXB0AAAAGAAAABBTcp/M3c
nNn8Oi2dZiw75dAAAAGAAAAAEAAAAzAAAAC3NzaC1lZDI1NTE5AAAAIG6EvZuUuzjzQAl2
i6zHx+GGi4sIsm/e4YX+jRcSrliZAAAAkFIE5FM8xmyrcq/l69BabypTNlP65vuiNTz34Z
chBfcnKG4McfNNHZdw1TZeacl72vjmFrpAN63bN4HahCvsh+HgoDrgMHRMc39pEQT5EAAn
EqEzoQ7YsS3Tm6EYunq0XQL0Z8bTlNDGAnTWAZINad542hKZgGDVOkLtxEcWO9HhtJMzxN
oWZnUXt+al/XIEiA==
-----END OPENSSH PRIVATE KEY-----
)";

constexpr auto kEncryptedFingerprint =
    "SHA256:ttlpD1Nd/zTjn1PBsVuPFsk9VblszllpkxEskhumgFE";

constexpr auto kPublicKey =
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIIU8uvQjoTz7sJEsmLxyOHhJyJVYwReUfMX8"
    "ggNcUEsj ch-test\n";

constexpr auto kPassphrase = "correct horse battery staple";

// A distinctive slice of the key's base64 body. Searching for the whole armour
// would miss an implementation that stored the key re-encoded, so the needle is a
// run of body characters from ONE line of the armour — it survives a copy that
// re-wraps the block anywhere except inside these 35 characters.
QByteArray keyNeedle()
{
    return QByteArrayLiteral("yJVYwReUfMX8ggNcUEsjAAAAB2NoLXRlc3Q");
}

// A legacy PEM key, in the shape libssh still loads. The body is not a real key:
// nothing in describeKeyText() parses a PEM body — the format carries no public
// half in the clear, which is exactly why it reports no key type and no
// fingerprint — so what is under test here is the container, the RFC 1421 headers
// and the refusal to invent a fingerprint.
constexpr auto kPemBody =
    "MIIBOgIBAAJBAKj34GkxFhD90vcNLYLInFEX6Ppy1tPf9Cnzj4p4WGeKLs1Pt8Qu\n"
    "KUpRKfFLfRYC9AIKjbQnQLpuVFXQ7QnaJfSCAwEAAQJAX6uzFTVKMEAvL0Pmqzho\n";

QString legacyPemKey(const QString &headers = QString())
{
    return QStringLiteral("-----BEGIN RSA PRIVATE KEY-----\n") + headers
           + QString::fromLatin1(kPemBody)
           + QStringLiteral("-----END RSA PRIVATE KEY-----\n");
}

// The per-app sandbox directories a mobile client could plausibly write to.
// tst_mobilekeystore runs with QStandardPaths test mode on, so these resolve
// inside the test's own temporary tree rather than the developer's home — which
// is what makes scanning them safe AND makes the scan meaningful: a store that
// derived a keys directory from QStandardPaths instead of from its rootDirectory
// would land here, where a leak check that only walked rootDirectory would never
// look.
QStringList sandboxRoots()
{
    QStringList roots;
    // TempLocation is deliberately NOT among these. QTemporaryDir puts this
    // test's own fixture — including the user-managed key file the reference path
    // is supposed to read — inside it, so scanning it would report the fixture as
    // a leak. That the store writes no temporary file is instead a structural
    // fact: it opens no file for writing anywhere, and the rootDirectory scans
    // elsewhere in this file cover the one directory it does derive.
    for (const QStandardPaths::StandardLocation location :
         {QStandardPaths::AppDataLocation, QStandardPaths::AppLocalDataLocation,
          QStandardPaths::AppConfigLocation, QStandardPaths::CacheLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty() && !roots.contains(path))
            roots.append(path);
    }
    return roots;
}

// Every regular file anywhere under `root`, so the leak check cannot be fooled
// by a subdirectory created on demand.
QStringList filesUnder(const QString &root)
{
    QStringList out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        out << it.next();
    out.sort();
    return out;
}

// Fails the calling test with the offending path when any file under `root`
// contains `needle`. Reads bytes, not text: a leak does not have to be UTF-8.
void assertNoFileContains(const QString &root, const QByteArray &needle,
                          const char *what)
{
    const QStringList files = filesUnder(root);
    for (const QString &path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray contents = file.readAll();
        if (contents.contains(needle)) {
            QFAIL(qPrintable(QStringLiteral("%1 reached disk at %2")
                                 .arg(QString::fromLatin1(what), path)));
        }
    }
}

// assertNoFileContains() over several roots, for the claim that has to hold
// everywhere the client could write rather than only under the store's own
// directory. QFAIL inside the callee still fails the calling test.
void assertNoFileContainsAnywhere(const QStringList &roots,
                                  const QByteArray &needle, const char *what)
{
    for (const QString &root : roots)
        assertNoFileContains(root, needle, what);
}

// The saved-key directory as SPEC 11.2 and MobileKeyStore.h document it:
// <app data>/keys, one file per key named exactly as the key is. Spelled out
// here rather than asked of the store, so a change to the layout has to be a
// deliberate change to this expectation too — a test that asked the store where
// it put the key could not catch it putting it somewhere else.
QString savedKeysDirectory(const QString &appData)
{
    return QDir(appData).filePath(QStringLiteral("keys"));
}

QString savedKeyPath(const QString &appData, const QString &name)
{
    return QDir(savedKeysDirectory(appData)).filePath(name);
}

// assertNoFileContains(), minus the files the key is ALLOWED to be in. Used by
// the saved-key cases to make the strong claim rather than the weak one: not
// merely "the key is in the saved file", but "the saved file is the ONLY place
// under this app's storage that the key reached" — which is what covers
// QSettings, a log, a cache and a stray temporary in one assertion.
void assertNoFileOutsideContains(const QString &root, const QString &allowedDir,
                                 const QByteArray &needle, const char *what)
{
    const QStringList files = filesUnder(root);
    for (const QString &path : files) {
        if (path.startsWith(allowedDir + QLatin1Char('/')))
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        if (file.readAll().contains(needle)) {
            QFAIL(qPrintable(QStringLiteral("%1 reached disk at %2")
                                 .arg(QString::fromLatin1(what), path)));
        }
    }
}

}  // namespace

class TestMobileKeyStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // Pure classifiers, no filesystem involved at all.
    void describesAnOpensshKey();
    void describesAnEncryptedKey();
    void diagnosesAPublicKeyByName();
    void rejectsTextThatIsNotAKey();
    void rejectsUnusableNames();
    void refusesAnOversizeKey();

    // Path 1: in-memory import.
    void importsAKeyWithoutTouchingDisk();
    void refusesADuplicateName();
    void reportsAnEncryptedKeyAsSuch();
    void referenceForAnInMemoryKeyIsEmpty();
    void forgetsAnImportedKey();

    // Path 2: a reference to a user-managed file.
    void referencesAUserManagedFileWithoutCopyingIt();
    void reAdoptsAReferenceFromAProfile();
    void refusesAReferenceThisDeviceCannotResolve();
    void referencedKeyMaterialIsReadOnDemandOnly();

    // Using and forgetting a credential.
    void installsAnInMemoryIdentityOnThePool();
    void installsAReferencedIdentityOnThePool();
    void anEmptyNameClearsThePoolIdentity();
    void removingTheInstalledKeyDisarmsThePool();
    void forgetSessionDropsEveryStoredSecret();
    void aReferenceIsNeverTreatedAsAnIdentityFilePath();

    // One-shot passphrases (SPEC 12.1).
    void takingAPassphraseErasesIt();
    void forgetPassphraseDropsItUnused();
    void noPassphraseEverReachesDisk();

    // Formats and damaged input.
    void describesALegacyPemKey();
    void reportsAnEncryptedLegacyPemKey();
    void treatsDekInfoAloneAsEncrypted();
    void normalisesCrlfAndSurroundingText();
    void rejectsAnAlteredBase64Body();
    void acceptsAFileExactlyAtTheSizeCap();

    // References that are not what they claim.
    void refusesADirectoryReference();
    void registerReferenceRepairsAnUnusableName();
    void registerReferenceRefusesAnEmptyReference();
    void refusesAReferenceWhoseFileStoppedBeingAKey();

    // What the UI is allowed to learn.
    void describeTextNeverReturnsKeyBytes();
    void keyInfoForAnUnknownNameIsEmpty();
    void sessionOnlyFallbackIsOfferedOnlyForADurabilityFailure();
    void nothingReachesThePerAppSandboxEither();

    // Path 3: saved on this device, opt-in per key. Each of these uses its own
    // temporary app-data directory, so a file that is SUPPOSED to exist is never
    // seen by the leak scans above.
    void savesAPastedKeyForTheNextLaunch();
    void aSavedKeyIsOwnerOnlyAndSoIsItsFolder();
    void savingIsNeverImplicit();
    void refusesToSaveAKeyItWouldHaveToBuildAPathFor();
    void refusesToSaveAReferencedKey();
    void forgetSessionKeepsASavedKeyAndWipesTheSessionOne();
    void forgetSavedKeyDeletesTheFileFromDisk();
    void removeKeyDeletesTheSavedFileToo();
    void savingAKeyPutsNothingInQSettings();

private:
    // Storage root the store is confined to, so the leak checks have a closed
    // world to walk. The store writes nothing here by design; the directory
    // exists precisely so "nothing" is a checkable claim.
    QTemporaryDir m_root;
    // A key file OUTSIDE the store's root, standing in for a document the user
    // manages themselves.
    QTemporaryDir m_userStorage;
    QString m_userKeyPath;

    QString root() const { return m_root.path(); }
};

// QStandardPaths test mode redirects AppDataLocation and friends into the test's
// own temporary tree. Two things depend on it: the store's default constructor
// (used by no test here, but a future one must not scribble a known_hosts file
// into the developer's real config directory), and the sandbox scan below, which
// walks those very directories looking for key material and must not walk the
// user's actual home.
void TestMobileKeyStore::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void TestMobileKeyStore::init()
{
    QVERIFY(m_root.isValid());
    QVERIFY(m_userStorage.isValid());

    m_userKeyPath = m_userStorage.filePath(QStringLiteral("id_ed25519"));
    QFile key(m_userKeyPath);
    QVERIFY(key.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(key.write(QByteArray(kPlainKey)),
             qint64(qstrlen(kPlainKey)));
    key.close();
}

void TestMobileKeyStore::describesAnOpensshKey()
{
    const auto description =
        MobileKeyStore::describeKeyText(QString::fromLatin1(kPlainKey));
    QVERIFY(description.valid);
    QVERIFY(description.error.isEmpty());
    QCOMPARE(description.format, QStringLiteral("openssh-key-v1"));
    QCOMPARE(description.keyType, QStringLiteral("ssh-ed25519"));
    QVERIFY(!description.encrypted);
    QVERIFY(description.kdf.isEmpty());
    QCOMPARE(description.fingerprint, QString::fromLatin1(kPlainFingerprint));
    // The normalised bytes are what libssh is handed: the block itself, ending
    // in a newline, with nothing the user pasted around it.
    QVERIFY(description.normalized.startsWith("-----BEGIN OPENSSH PRIVATE KEY-----\n"));
    QVERIFY(description.normalized.endsWith("-----END OPENSSH PRIVATE KEY-----\n"));
}

void TestMobileKeyStore::describesAnEncryptedKey()
{
    const auto description =
        MobileKeyStore::describeKeyText(QString::fromLatin1(kEncryptedKey));
    QVERIFY(description.valid);
    QVERIFY(description.encrypted);
    QCOMPARE(description.kdf, QStringLiteral("bcrypt"));
    // The public half lives outside the sealed section, so this is knowable
    // while the key itself is still locked — that is what lets the import sheet
    // show a fingerprint before asking for the passphrase.
    QCOMPARE(description.keyType, QStringLiteral("ssh-ed25519"));
    QCOMPARE(description.fingerprint,
             QString::fromLatin1(kEncryptedFingerprint));
}

void TestMobileKeyStore::diagnosesAPublicKeyByName()
{
    // Handing over the .pub file is the mistake users actually make, so it must
    // be named rather than swept into a generic refusal.
    const auto description =
        MobileKeyStore::describeKeyText(QString::fromLatin1(kPublicKey));
    QVERIFY(!description.valid);
    QVERIFY(description.error.contains(QStringLiteral("public"),
                                       Qt::CaseInsensitive));
    QVERIFY(description.normalized.isEmpty());
}

void TestMobileKeyStore::rejectsTextThatIsNotAKey()
{
    for (const QString &text : {QStringLiteral(""),
                                QStringLiteral("   \n\t\n"),
                                QStringLiteral("hunter2"),
                                QStringLiteral("-----BEGIN OPENSSH PRIVATE KEY-----\n")}) {
        const auto description = MobileKeyStore::describeKeyText(text);
        QVERIFY2(!description.valid, qPrintable(text));
        QVERIFY(!description.error.isEmpty());
        // A refused paste may still be key material, and an error message is a
        // thing that gets logged: it must never quote the input.
        QVERIFY(!description.error.contains(QStringLiteral("hunter2")));
        QVERIFY(description.normalized.isEmpty());
    }
}

void TestMobileKeyStore::rejectsUnusableNames()
{
    QVERIFY(MobileKeyStore::isUsableKeyName(QStringLiteral("work-laptop")));
    QVERIFY(MobileKeyStore::isUsableKeyName(QStringLiteral("id_ed25519")));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QString()));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QStringLiteral(".hidden")));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QStringLiteral("has space")));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QStringLiteral("../escape")));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QString(65, QLatin1Char('a'))));
}

void TestMobileKeyStore::refusesAnOversizeKey()
{
    // A picker must not be usable to pull an arbitrary file into memory.
    MobileKeyStore store(root());
    const QString fat = m_userStorage.filePath(QStringLiteral("fat.key"));
    QFile file(fat);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QByteArray(MobileKeyStore::kMaxKeyBytes + 1, 'a')) > 0);
    file.close();

    QVERIFY(!store.importKeyFromFile(QUrl::fromLocalFile(fat),
                                     QStringLiteral("fat")));
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store.keyNames().isEmpty());
}

void TestMobileKeyStore::importsAKeyWithoutTouchingDisk()
{
    MobileKeyStore store(root());
    const QStringList before = filesUnder(root());

    QSignalSpy imported(&store, &MobileKeyStore::keyImported);
    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QCOMPARE(store.keyNames(), QStringList{QStringLiteral("pasted")});
    QVERIFY(store.hasKeys());
    QCOMPARE(imported.count(), 1);
    QCOMPARE(imported.at(0).at(1).toBool(), false);  // not a reference
    QCOMPARE(store.keyFingerprint(QStringLiteral("pasted")),
             QString::fromLatin1(kPlainFingerprint));

    // The whole point: an in-memory import performs no filesystem work.
    QCOMPARE(filesUnder(root()), before);
    assertNoFileContains(root(), keyNeedle(), "an imported key");
}

void TestMobileKeyStore::refusesADuplicateName()
{
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("dup"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(!store.importKeyFromText(QStringLiteral("dup"),
                                     QString::fromLatin1(kEncryptedKey)));
    QVERIFY(!store.lastError().isEmpty());
    // The first key is still the one under that name.
    QCOMPARE(store.keyFingerprint(QStringLiteral("dup")),
             QString::fromLatin1(kPlainFingerprint));
    QCOMPARE(store.keyNames().count(), 1);
}

void TestMobileKeyStore::reportsAnEncryptedKeyAsSuch()
{
    // An encrypted key must import: the UI needs it in hand to know a
    // passphrase is required, rather than discovering that at handshake time.
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("locked"),
                                    QString::fromLatin1(kEncryptedKey)));

    const QVariantMap info = store.keyInfo(QStringLiteral("locked"));
    QCOMPARE(info.value(QStringLiteral("encrypted")).toBool(), true);
    QCOMPARE(info.value(QStringLiteral("kdf")).toString(),
             QStringLiteral("bcrypt"));
    QCOMPARE(info.value(QStringLiteral("referenced")).toBool(), false);
    QCOMPARE(info.value(QStringLiteral("fingerprintAvailable")).toBool(), true);
    QCOMPARE(info.value(QStringLiteral("fingerprint")).toString(),
             QString::fromLatin1(kEncryptedFingerprint));
}

void TestMobileKeyStore::referenceForAnInMemoryKeyIsEmpty()
{
    // referenceFor() is the only value that leaves this class for persistence.
    // A pasted key has nothing durable to record, and an empty string is what
    // keeps key material out of the profile.
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(store.referenceFor(QStringLiteral("pasted")).isEmpty());
}

void TestMobileKeyStore::forgetsAnImportedKey()
{
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));

    QSignalSpy removed(&store, &MobileKeyStore::keyRemoved);
    QVERIFY(store.removeKey(QStringLiteral("pasted")));
    QCOMPARE(removed.count(), 1);
    QVERIFY(store.keyNames().isEmpty());
    QVERIFY(!store.hasKeys());
    QVERIFY(store.keyFingerprint(QStringLiteral("pasted")).isEmpty());

    QVERIFY(!store.removeKey(QStringLiteral("pasted")));
    QVERIFY(!store.lastError().isEmpty());
}

void TestMobileKeyStore::referencesAUserManagedFileWithoutCopyingIt()
{
    MobileKeyStore store(root());
    const QStringList before = filesUnder(root());

    const QString name =
        store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                   QStringLiteral("mine"));
    QCOMPARE(name, QStringLiteral("mine"));
    QCOMPARE(store.referenceNames(), QStringList{QStringLiteral("mine")});
    // The reference names a file; it is not key material, and it is exactly what
    // the profile's existing identityFile field will carry.
    QCOMPARE(store.referenceFor(QStringLiteral("mine")), m_userKeyPath);
    QCOMPARE(store.keyFingerprint(QStringLiteral("mine")),
             QString::fromLatin1(kPlainFingerprint));

    const QVariantMap info = store.keyInfo(QStringLiteral("mine"));
    QCOMPARE(info.value(QStringLiteral("referenced")).toBool(), true);

    // The user's file stays the only copy.
    QCOMPARE(filesUnder(root()), before);
    assertNoFileContains(root(), keyNeedle(), "a referenced key");
}

void TestMobileKeyStore::reAdoptsAReferenceFromAProfile()
{
    // A remembered server still offers its key after a relaunch, which is the
    // whole reason the durable path exists.
    MobileKeyStore store(root());
    const QString name = store.registerReference(m_userKeyPath,
                                                 QStringLiteral("saved"));
    QCOMPARE(name, QStringLiteral("saved"));
    QCOMPARE(store.referenceFor(QStringLiteral("saved")), m_userKeyPath);

    // Idempotent: re-adopting the same reference is a no-op, not a second entry.
    QCOMPARE(store.registerReference(m_userKeyPath, QStringLiteral("other")),
             QStringLiteral("saved"));
    QCOMPARE(store.referenceNames().count(), 1);
}

void TestMobileKeyStore::refusesAReferenceThisDeviceCannotResolve()
{
    // A profile copied from another device names a file that is not here. That
    // is a thing to explain while the user is looking at the connect page, not
    // a connection that fails later for no visible reason.
    MobileKeyStore store(root());
    const QString missing =
        m_userStorage.filePath(QStringLiteral("not-here/id_ed25519"));
    QVERIFY(store.registerReference(missing, QStringLiteral("ghost")).isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store.referenceNames().isEmpty());
}

void TestMobileKeyStore::referencedKeyMaterialIsReadOnDemandOnly()
{
    // The reference is read once at pick time to report an unusable key early,
    // then dropped: deleting the user's file must make the credential
    // unavailable rather than silently using a cached copy.
    MobileKeyStore store(root());
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));

    QVERIFY(QFile::remove(m_userKeyPath));

    ch::SshConnectionPool pool;
    store.setConnectionPool(&pool);
    QVERIFY(!store.applyIdentityForConnect(QStringLiteral("mine")));
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(!pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::installsAnInMemoryIdentityOnThePool()
{
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(!pool.hasInMemoryIdentity());
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::installsAReferencedIdentityOnThePool()
{
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("mine")));
    QVERIFY(pool.hasInMemoryIdentity());

    // Resolving a reference must not have left a copy behind.
    assertNoFileContains(root(), keyNeedle(), "a resolved reference");
}

void TestMobileKeyStore::anEmptyNameClearsThePoolIdentity()
{
    // Switching to password auth must not leave the previous attempt's key
    // installed, or a connection aimed at one credential would be made with
    // another.
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(pool.hasInMemoryIdentity());

    QVERIFY(store.applyIdentityForConnect(QString()));
    QVERIFY(!pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::removingTheInstalledKeyDisarmsThePool()
{
    // "Remove" tells the user CodeHarbor will forget the key. The pool holds its
    // own copy of whatever was last installed, so forgetting has to reach in
    // there too — otherwise the bytes stay live for the rest of the run and
    // SessionBootstrap's reconnect ladder would re-authenticate with a credential
    // the user just deleted.
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(pool.hasInMemoryIdentity());

    // Tidying up an UNRELATED key must not disarm the credential the live session
    // is using, or the next reconnect would fail authentication.
    QVERIFY(store.removeKey(QStringLiteral("mine")));
    QVERIFY(pool.hasInMemoryIdentity());

    QVERIFY(store.removeKey(QStringLiteral("pasted")));
    QVERIFY(!pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::forgetSessionDropsEveryStoredSecret()
{
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));
    store.armPassphrase(QStringLiteral("pasted"),
                        QString::fromLatin1(kPassphrase));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));

    store.forgetSession();

    // Secrets are gone: the in-memory key, the armed passphrase and the pool's
    // installed identity.
    QVERIFY(store.keyNames().isEmpty());
    QVERIFY(!store.hasArmedPassphrase(QStringLiteral("pasted")));
    QVERIFY(!pool.hasInMemoryIdentity());
    // The reference is not a secret, so reconnecting needs no fresh pick.
    QCOMPARE(store.referenceNames(), QStringList{QStringLiteral("mine")});

    assertNoFileContains(root(), keyNeedle(), "a forgotten key");
}

void TestMobileKeyStore::aReferenceIsNeverTreatedAsAnIdentityFilePath()
{
    // The profile field a mobile durable reference lives in is the same field
    // SshConnectionPool::connectToHost() takes as an identity FILE, and the
    // reference has to stay in the profile verbatim so the connect page can read
    // it back after a relaunch. So the pool drops it at the point where the
    // stored string would become a path, and the ladder then runs on the
    // in-memory identity alone — which is the real credential on mobile anyway.
    QString scheme;

    // Both reference shapes the mobile client mints: ignored, and named.
    QVERIFY(SshConnectionPool::identityFilePathFor(
                QStringLiteral("content://com.android.providers.downloads/42"),
                &scheme)
                .isEmpty());
    QCOMPARE(scheme, QStringLiteral("content"));
    QVERIFY(SshConnectionPool::identityFilePathFor(
                QStringLiteral("chbookmark:Ym9va21hcmsA"), &scheme)
                .isEmpty());
    QCOMPARE(scheme, QStringLiteral("chbookmark"));

    // And everything that IS a path keeps working exactly as before, with an
    // empty `scheme` to say nothing was dropped.
    QVERIFY(SshConnectionPool::identityFilePathFor(QString(), &scheme).isEmpty());
    QVERIFY(scheme.isEmpty());
    QCOMPARE(SshConnectionPool::identityFilePathFor(
                 QStringLiteral("/home/me/.ssh/id_ed25519"), &scheme),
             QStringLiteral("/home/me/.ssh/id_ed25519"));
    QVERIFY(scheme.isEmpty());
    // Tilde expansion is this client's own doing — libssh does not do it — so it
    // has to survive the new guard.
    QCOMPARE(SshConnectionPool::identityFilePathFor(QStringLiteral("~/.ssh/id_rsa"),
                                                    &scheme),
             QDir::home().filePath(QStringLiteral(".ssh/id_rsa")));
    QVERIFY(scheme.isEmpty());
    QCOMPARE(SshConnectionPool::identityFilePathFor(QStringLiteral("~")),
             QDir::homePath());

    // A Windows drive letter is a ONE-character prefix and must never read as a
    // URI scheme: this is the case that would silently ignore every explicitly
    // configured key on Windows.
    QString windows = SshConnectionPool::identityFilePathFor(
        QStringLiteral("C:\\Users\\me\\.ssh\\id_ed25519"), &scheme);
    QVERIFY(scheme.isEmpty());
    QVERIFY2(windows.startsWith(QStringLiteral("C:")), qPrintable(windows));
    // A colon further along a relative path is not a scheme either.
    QCOMPARE(SshConnectionPool::identityFilePathFor(
                 QStringLiteral("keys/od:d/id_ed25519"), &scheme),
             QStringLiteral("keys/od:d/id_ed25519"));
    QVERIFY(scheme.isEmpty());
}

void TestMobileKeyStore::takingAPassphraseErasesIt()
{
    // SPEC 12.1: one secret, one attempt. A retry has to ask again by
    // construction, not by whoever writes the retry remembering to.
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("locked"),
                                    QString::fromLatin1(kEncryptedKey)));

    store.armPassphrase(QStringLiteral("locked"),
                        QString::fromLatin1(kPassphrase));
    QVERIFY(store.hasArmedPassphrase(QStringLiteral("locked")));
    QCOMPARE(store.takePassphrase(QStringLiteral("locked")),
             QString::fromLatin1(kPassphrase));

    QVERIFY(!store.hasArmedPassphrase(QStringLiteral("locked")));
    QVERIFY(store.takePassphrase(QStringLiteral("locked")).isEmpty());
}

void TestMobileKeyStore::forgetPassphraseDropsItUnused()
{
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("locked"),
                                    QString::fromLatin1(kEncryptedKey)));
    store.armPassphrase(QStringLiteral("locked"),
                        QString::fromLatin1(kPassphrase));
    store.forgetPassphrase();
    QVERIFY(!store.hasArmedPassphrase(QStringLiteral("locked")));
    QVERIFY(store.takePassphrase(QStringLiteral("locked")).isEmpty());
}

void TestMobileKeyStore::noPassphraseEverReachesDisk()
{
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("locked"),
                                    QString::fromLatin1(kEncryptedKey)));
    store.armPassphrase(QStringLiteral("locked"),
                        QString::fromLatin1(kPassphrase));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("locked")));
    QCOMPARE(store.takePassphrase(QStringLiteral("locked")),
             QString::fromLatin1(kPassphrase));
    store.forgetSession();

    assertNoFileContains(root(), QByteArray(kPassphrase), "a passphrase");
}

// ---------------------------------------------------------------------------
// formats and damaged input
// ---------------------------------------------------------------------------

void TestMobileKeyStore::describesALegacyPemKey()
{
    // libssh still loads these, so the client must too — and must be honest that
    // it cannot show a fingerprint for one: the format carries no public half in
    // the clear, so there is nothing to digest without parsing the private key.
    const auto description = MobileKeyStore::describeKeyText(legacyPemKey());
    QVERIFY2(description.valid, qPrintable(description.error));
    QCOMPARE(description.format, QStringLiteral("pem"));
    QVERIFY(description.keyType.isEmpty());
    QVERIFY(!description.encrypted);
    QVERIFY(description.fingerprint.isEmpty());
    QVERIFY(description.normalized.startsWith("-----BEGIN RSA PRIVATE KEY-----\n"));
    QVERIFY(description.normalized.endsWith("-----END RSA PRIVATE KEY-----\n"));
}

void TestMobileKeyStore::reportsAnEncryptedLegacyPemKey()
{
    // RFC 1421 headers, and the blank line between them and the body has to
    // SURVIVE normalisation: libssh needs it to find the body at all.
    const auto description = MobileKeyStore::describeKeyText(
        legacyPemKey(QStringLiteral("Proc-Type: 4,ENCRYPTED\n"
                                    "DEK-Info: AES-128-CBC,0123456789ABCDEF\n"
                                    "\n")));
    QVERIFY2(description.valid, qPrintable(description.error));
    QVERIFY(description.encrypted);
    QCOMPARE(description.kdf, QStringLiteral("AES-128-CBC"));
    QVERIFY(description.normalized.contains("\nProc-Type: 4,ENCRYPTED\n"));
    QVERIFY(description.normalized.contains("\n\n"));
}

void TestMobileKeyStore::treatsDekInfoAloneAsEncrypted()
{
    // A DEK-Info without its Proc-Type is malformed, but the header exists only to
    // describe an encryption. Reporting such a key as unencrypted would hide the
    // connect page's passphrase field and cost the user a failed authentication
    // round before the ladder asked for a passphrase.
    const auto description = MobileKeyStore::describeKeyText(
        legacyPemKey(QStringLiteral("DEK-Info: AES-256-CBC,0123456789ABCDEF\n"
                                    "\n")));
    QVERIFY2(description.valid, qPrintable(description.error));
    QVERIFY(description.encrypted);
    QCOMPARE(description.kdf, QStringLiteral("AES-256-CBC"));
}

void TestMobileKeyStore::normalisesCrlfAndSurroundingText()
{
    // What a user actually pastes: a key that came through a mail client, with
    // CRLF line endings, an indented first line and a signature after the end
    // marker. All of it has to come out as the exact bytes libssh is handed.
    QString crlf = QString::fromLatin1(kPlainKey);
    crlf.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    const QString pasted = QStringLiteral("Here is the key:\r\n\r\n   ") + crlf
                           + QStringLiteral("-- \r\nSent from my phone\r\n");

    const auto description = MobileKeyStore::describeKeyText(pasted);
    QVERIFY2(description.valid, qPrintable(description.error));
    QCOMPARE(description.fingerprint, QString::fromLatin1(kPlainFingerprint));
    // Byte for byte the original armour: no CR anywhere, nothing from around the
    // block, and the trailing newline libssh expects.
    QCOMPARE(description.normalized, QByteArray(kPlainKey));
    QVERIFY(!description.normalized.contains('\r'));
}

void TestMobileKeyStore::rejectsAnAlteredBase64Body()
{
    // A chat client that inserted an ellipsis, or a diff that lost a chunk. The
    // alphabet is checked BEFORE decoding, so this is reported as damage rather
    // than as a malformed container after a lenient decoder silently dropped the
    // offending characters.
    QString altered = QString::fromLatin1(kPlainKey);
    altered.replace(QStringLiteral("hwAAAAtzc2gtZWQyNTUxOQ"),
                    QStringLiteral("hwAAAAt...c2gtZWQyNTUxOQ"));
    QVERIFY(altered != QString::fromLatin1(kPlainKey));  // the replace matched

    const auto description = MobileKeyStore::describeKeyText(altered);
    QVERIFY(!description.valid);
    QVERIFY(description.error.contains(QStringLiteral("altered")));
    QVERIFY(description.normalized.isEmpty());
}

void TestMobileKeyStore::acceptsAFileExactlyAtTheSizeCap()
{
    // The bound is inclusive, and it is enforced on what was READ rather than on
    // what the file claims: exactly kMaxKeyBytes goes through, one byte more does
    // not. Padding after the END marker is dropped by normalisation, which is what
    // lets a real key be padded up to the boundary at all.
    MobileKeyStore store(root());

    QByteArray padded(kPlainKey);
    QVERIFY(padded.size() < MobileKeyStore::kMaxKeyBytes);
    padded.append(QByteArray(qsizetype(MobileKeyStore::kMaxKeyBytes)
                                 - padded.size(),
                             '\n'));
    QCOMPARE(qint64(padded.size()), MobileKeyStore::kMaxKeyBytes);

    const QString atCap = m_userStorage.filePath(QStringLiteral("at-cap.key"));
    QFile atCapFile(atCap);
    QVERIFY(atCapFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(atCapFile.write(padded), qint64(padded.size()));
    atCapFile.close();

    QVERIFY(store.importKeyFromFile(QUrl::fromLocalFile(atCap),
                                    QStringLiteral("at-cap")));
    QCOMPARE(store.keyFingerprint(QStringLiteral("at-cap")),
             QString::fromLatin1(kPlainFingerprint));

    const QString overCap = m_userStorage.filePath(QStringLiteral("over-cap.key"));
    QFile overCapFile(overCap);
    QVERIFY(overCapFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(overCapFile.write(padded + '\n'), qint64(padded.size() + 1));
    overCapFile.close();

    QVERIFY(!store.importKeyFromFile(QUrl::fromLocalFile(overCap),
                                     QStringLiteral("over-cap")));
    QVERIFY(!store.lastError().isEmpty());
    QCOMPARE(store.keyNames(), QStringList{QStringLiteral("at-cap")});
}

// ---------------------------------------------------------------------------
// references that are not what they claim
// ---------------------------------------------------------------------------

void TestMobileKeyStore::refusesADirectoryReference()
{
    // A picker can hand back a directory, and a stored reference can end up
    // naming one. Whichever way it arrives it must be refused with something to
    // read, not accepted into the key list to fail at connect time.
    MobileKeyStore store(root());
    const QString directory = m_userStorage.path();

    QVERIFY(store.registerReference(directory, QStringLiteral("dir")).isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store
                .addReferenceFromFile(QUrl::fromLocalFile(directory),
                                      QStringLiteral("dir"))
                .isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store.referenceNames().isEmpty());
    QVERIFY(!store.hasKeys());
}

void TestMobileKeyStore::registerReferenceRepairsAnUnusableName()
{
    // This is the one import path whose name does not go through claimName() on
    // the happy path, and the name it is handed comes from a saved profile rather
    // than from a field the user just typed. An unusable one must not end up in
    // the key maps and in the QML picker; the credential is too useful to refuse
    // over it, so the name is repaired instead.
    MobileKeyStore store(root());
    const QString name =
        store.registerReference(m_userKeyPath, QStringLiteral("not a name"));
    QVERIFY(!name.isEmpty());
    QVERIFY2(MobileKeyStore::isUsableKeyName(name), qPrintable(name));
    QCOMPARE(store.referenceNames(), QStringList{name});
    QCOMPARE(store.referenceFor(name), m_userKeyPath);
    QCOMPARE(store.keyFingerprint(name), QString::fromLatin1(kPlainFingerprint));
}

void TestMobileKeyStore::registerReferenceRefusesAnEmptyReference()
{
    // A password-only profile stores an empty identityFile. Asking the store to
    // adopt that is a caller mistake and is reported as one — which is why
    // ConnectPage only calls this when the profile actually carries a reference,
    // rather than showing the user an error for a profile that is simply
    // password-only.
    MobileKeyStore store(root());
    QVERIFY(store.registerReference(QString(), QString()).isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store.registerReference(QStringLiteral("   "), QString()).isEmpty());
    QVERIFY(store.referenceNames().isEmpty());
}

void TestMobileKeyStore::refusesAReferenceWhoseFileStoppedBeingAKey()
{
    // The resolvable check at pick time and the read at connect time are separated
    // by however long the user's session lasts, and the file belongs to the user:
    // it can be replaced with anything in between. The read validates again rather
    // than trusting the earlier answer.
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));

    QFile replaced(m_userKeyPath);
    QVERIFY(replaced.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(replaced.write("this is not a key any more\n") > 0);
    replaced.close();

    QVERIFY(!store.applyIdentityForConnect(QStringLiteral("mine")));
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(!pool.hasInMemoryIdentity());
    // And the UI is told there is nothing to describe rather than shown a stale
    // fingerprint from the file that used to be there.
    QVERIFY(store.keyInfo(QStringLiteral("mine")).isEmpty());
}

// ---------------------------------------------------------------------------
// what the UI is allowed to learn
// ---------------------------------------------------------------------------

void TestMobileKeyStore::describeTextNeverReturnsKeyBytes()
{
    // describeText() is the preview the import sheet renders. It goes to QML, so
    // anything in it can end up in a visible item, in a debug dump or in a log
    // line: it must carry the DESCRIPTION and never the key.
    MobileKeyStore store(root());
    const QVariantMap described =
        store.describeText(QString::fromLatin1(kPlainKey));
    QCOMPARE(described.value(QStringLiteral("valid")).toBool(), true);
    QCOMPARE(described.value(QStringLiteral("fingerprint")).toString(),
             QString::fromLatin1(kPlainFingerprint));
    for (auto it = described.cbegin(); it != described.cend(); ++it) {
        QVERIFY2(!it.value().toString().toLatin1().contains(keyNeedle()),
                 qPrintable(it.key()));
    }

    // A refusal must not quote the input either: the text that was refused may
    // still be key material, and an error message is a thing that gets logged.
    const QVariantMap refused = store.describeText(
        QStringLiteral("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5 someone@host"));
    QCOMPARE(refused.value(QStringLiteral("valid")).toBool(), false);
    QVERIFY(!refused.value(QStringLiteral("error"))
                 .toString()
                 .contains(QStringLiteral("AAAAC3")));
}

void TestMobileKeyStore::keyInfoForAnUnknownNameIsEmpty()
{
    MobileKeyStore store(root());
    QVERIFY(store.keyInfo(QStringLiteral("never-loaded")).isEmpty());
    QVERIFY(store.keyFingerprint(QStringLiteral("never-loaded")).isEmpty());
    QVERIFY(store.referenceFor(QStringLiteral("never-loaded")).isEmpty());
}

void TestMobileKeyStore::sessionOnlyFallbackIsOfferedOnlyForADurabilityFailure()
{
    // KeyImportSheet offers "use that file just for this session" off this flag.
    // A file that is not a key, or one too large to read, would fail the
    // session-only import in exactly the same way, so offering the fallback for
    // those is a button whose only outcome is the identical refusal.
    MobileKeyStore store(root());

    const QString publicKey = m_userStorage.filePath(QStringLiteral("id.pub"));
    QFile publicFile(publicKey);
    QVERIFY(publicFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(publicFile.write(QByteArray(kPublicKey)) > 0);
    publicFile.close();

    QVERIFY(store.addReferenceFromFile(QUrl::fromLocalFile(publicKey),
                                       QStringLiteral("pub"))
                .isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(!store.lastFailureAllowsSessionOnly());

    // A durable reference to a plain path always succeeds on this platform, so the
    // flag stays false after a SUCCESS too — it describes a failure, not a state.
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));
    QVERIFY(!store.lastFailureAllowsSessionOnly());
}

void TestMobileKeyStore::nothingReachesThePerAppSandboxEither()
{
    // The leak checks elsewhere walk the store's own rootDirectory, which proves
    // nothing about an implementation that derived a keys directory from
    // QStandardPaths instead — the one place a mobile app would naturally put it.
    // initTestCase() puts QStandardPaths in test mode, so these paths are inside
    // the test's temporary tree and can be scanned safely.
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));
    store.armPassphrase(QStringLiteral("pasted"),
                        QString::fromLatin1(kPassphrase));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("mine")));
    QVERIFY(!store.keyInfo(QStringLiteral("mine")).isEmpty());
    store.forgetSession();

    const QStringList roots = sandboxRoots();
    QVERIFY(!roots.isEmpty());
    assertNoFileContainsAnywhere(roots, keyNeedle(), "an imported key");
    assertNoFileContainsAnywhere(roots, QByteArray(kPassphrase), "a passphrase");
    assertNoFileContains(root(), keyNeedle(), "an imported key");
}

// ---------------------------------------------------------------------------
// path 3: saved on this device (opt-in, per key)
// ---------------------------------------------------------------------------

void TestMobileKeyStore::savesAPastedKeyForTheNextLaunch()
{
    // The whole point of the third path: a key that was PASTED once is offered
    // again by a store built from scratch over the same app-data directory —
    // which is what a relaunch is — and can still authenticate.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());

    {
        MobileKeyStore store(appData.path());
        QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                        QString::fromLatin1(kPlainKey)));
        QVERIFY(!store.isSavedOnDevice(QStringLiteral("kept")));

        QSignalSpy changed(&store, &MobileKeyStore::keyNamesChanged);
        QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
                 qPrintable(store.lastError()));
        QVERIFY(store.lastError().isEmpty());
        QCOMPARE(changed.count(), 1);
        QVERIFY(store.isSavedOnDevice(QStringLiteral("kept")));
        // The in-memory copy is untouched: saving adds durability to the key the
        // session is already using rather than replacing it.
        QCOMPARE(store.keyNames(), QStringList{QStringLiteral("kept")});
        // And it is ONE entry in the list the connect page renders, not two.
        QCOMPARE(store.allKeyNames(), QStringList{QStringLiteral("kept")});
        QVERIFY(QFile::exists(savedKeyPath(appData.path(),
                                           QStringLiteral("kept"))));
    }

    // A FRESH store over the same directory: this is the relaunch.
    ch::SshConnectionPool pool;
    MobileKeyStore reopened(appData.path());
    reopened.setConnectionPool(&pool);
    QCOMPARE(reopened.savedKeyNames(), QStringList{QStringLiteral("kept")});
    QCOMPARE(reopened.allKeyNames(), QStringList{QStringLiteral("kept")});
    QVERIFY(reopened.hasKeys());
    QVERIFY(reopened.isSavedOnDevice(QStringLiteral("kept")));
    // Nothing is loaded in memory: the bytes are read when they are needed.
    QVERIFY(reopened.keyNames().isEmpty());

    const QVariantMap info = reopened.keyInfo(QStringLiteral("kept"));
    QCOMPARE(info.value(QStringLiteral("saved")).toBool(), true);
    QCOMPARE(info.value(QStringLiteral("referenced")).toBool(), false);
    QVERIFY(info.value(QStringLiteral("reference")).toString().isEmpty());
    QCOMPARE(info.value(QStringLiteral("fingerprint")).toString(),
             QString::fromLatin1(kPlainFingerprint));
    // The saved bytes are the same key, byte for byte, so it still authenticates.
    QVERIFY2(reopened.applyIdentityForConnect(QStringLiteral("kept")),
             qPrintable(reopened.lastError()));
    QVERIFY(pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::aSavedKeyIsOwnerOnlyAndSoIsItsFolder()
{
#ifndef Q_OS_UNIX
    QSKIP("POSIX modes only: QFile::permissions() synthesises these bits from an "
          "ACL on Windows, so the same assertion would describe nothing real.");
#else
    // A private key readable by the group or by the world is the failure this
    // path could most easily ship with and nobody would notice, because
    // everything else about it would still work. The directory matters as much as
    // the file: a listable keys directory tells another process which keys exist
    // and under what names.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    MobileKeyStore store(appData.path());
    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));

    const QFile::Permissions others =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther
        | QFileDevice::ExeOther;

    const QFile::Permissions file =
        QFile::permissions(savedKeyPath(appData.path(), QStringLiteral("kept")));
    QVERIFY(file.testFlag(QFileDevice::ReadOwner));
    QVERIFY(file.testFlag(QFileDevice::WriteOwner));
    QCOMPARE(file & others, QFile::Permissions());

    const QFile::Permissions directory =
        QFile::permissions(savedKeysDirectory(appData.path()));
    QVERIFY(directory.testFlag(QFileDevice::ReadOwner));
    QVERIFY(directory.testFlag(QFileDevice::WriteOwner));
    QVERIFY(directory.testFlag(QFileDevice::ExeOwner));
    QCOMPARE(directory & others, QFile::Permissions());
#endif
}

void TestMobileKeyStore::savingIsNeverImplicit()
{
    // SPEC 11.2 admits the copy only for a key the user explicitly asked to keep.
    // So an ordinary import — the default path, and the one nearly every user
    // takes — must leave the device with no keys directory at all, never mind a
    // key in it.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    ch::SshConnectionPool pool;
    MobileKeyStore store(appData.path());
    store.setConnectionPool(&pool);
    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    // Using the key for a connection is not asking for it to be kept, so a whole
    // authenticate cycle must still leave nothing on the device.
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(pool.hasInMemoryIdentity());
    QVERIFY(!store.isSavedOnDevice(QStringLiteral("pasted")));
    QVERIFY(store.savedKeyNames().isEmpty());
    QVERIFY(!QFile::exists(savedKeysDirectory(appData.path())));
    assertNoFileContains(appData.path(), keyNeedle(), "an unsaved key");

    // Same for a store constructed over a directory that has never seen a save:
    // construction reads, it does not create.
    MobileKeyStore reopened(appData.path());
    QVERIFY(reopened.savedKeyNames().isEmpty());
    QVERIFY(!reopened.hasKeys());
    QVERIFY(!QFile::exists(savedKeysDirectory(appData.path())));
}

void TestMobileKeyStore::refusesToSaveAKeyItWouldHaveToBuildAPathFor()
{
    // The name reaches the FILENAME, so an unvalidated one is a write outside the
    // app's own data directory. "../evil" is the case that matters: it is a name
    // no import path would accept, but saveKeyOnDevice() is reachable from QML
    // with whatever string a caller passes, so it has to refuse it itself — and
    // refuse it BEFORE any filesystem call, which is what "no file was created"
    // below actually checks.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    MobileKeyStore store(appData.path());
    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));

    for (const QString &name : {QStringLiteral("../evil"),
                                QStringLiteral("../../evil"),
                                QStringLiteral("sub/evil"),
                                QStringLiteral(".hidden"),
                                QStringLiteral("has space"),
                                QString(65, QLatin1Char('a')),
                                QString()}) {
        QVERIFY2(!store.saveKeyOnDevice(name), qPrintable(name));
        QVERIFY(!store.lastError().isEmpty());
        QVERIFY2(!store.isSavedOnDevice(name), qPrintable(name));
    }
    QVERIFY(store.savedKeyNames().isEmpty());

    // Nothing was created anywhere: not the keys directory, not the traversal
    // target one level up from it, and no file under the app-data root at all.
    QVERIFY(!QFile::exists(savedKeysDirectory(appData.path())));
    QVERIFY(!QFile::exists(QDir(appData.path()).filePath(QStringLiteral("evil"))));
    QVERIFY(filesUnder(appData.path()).isEmpty());

    // The refusal is about the NAME, not about the key: the same store still
    // saves the key it does have.
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));
}

void TestMobileKeyStore::refusesToSaveAReferencedKey()
{
    // Path 2's promise is that the key stays in the user's own storage. Copying a
    // referenced key into app storage behind the same switch would quietly break
    // it, so this is refused with something to read rather than silently doing
    // the more surprising of the two possible things.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    MobileKeyStore store(appData.path());
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));

    QVERIFY(!store.saveKeyOnDevice(QStringLiteral("mine")));
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(!store.isSavedOnDevice(QStringLiteral("mine")));
    QVERIFY(!QFile::exists(savedKeysDirectory(appData.path())));
    // An unknown name is refused too, and neither refusal creates anything.
    QVERIFY(!store.saveKeyOnDevice(QStringLiteral("never-loaded")));
    QVERIFY(filesUnder(appData.path()).isEmpty());
}

void TestMobileKeyStore::forgetSessionKeepsASavedKeyAndWipesTheSessionOne()
{
    // "Disconnect" is not "delete my key". A saved key survives it — that is what
    // saving means — while the session-only key beside it is wiped, which is what
    // NOT saving means.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    ch::SshConnectionPool pool;
    MobileKeyStore store(appData.path());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(store.importKeyFromText(QStringLiteral("session-only"),
                                    QString::fromLatin1(kEncryptedKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    store.armPassphrase(QStringLiteral("session-only"),
                        QString::fromLatin1(kPassphrase));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("kept")));

    store.forgetSession();

    // The session secrets are gone, including the loaded copy of the saved key.
    QVERIFY(store.keyNames().isEmpty());
    QVERIFY(!store.hasArmedPassphrase(QStringLiteral("session-only")));
    QVERIFY(!pool.hasInMemoryIdentity());
    // The saved key is still there, still listed, and still usable without a
    // fresh paste — which is the only reason the user saved it.
    QCOMPARE(store.savedKeyNames(), QStringList{QStringLiteral("kept")});
    QCOMPARE(store.allKeyNames(), QStringList{QStringLiteral("kept")});
    QVERIFY(QFile::exists(savedKeyPath(appData.path(), QStringLiteral("kept"))));
    QVERIFY2(store.applyIdentityForConnect(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QVERIFY(pool.hasInMemoryIdentity());
    // The one that was not saved is gone from the device's point of view too.
    QVERIFY(!store.applyIdentityForConnect(QStringLiteral("session-only")));
    QVERIFY(store.keyInfo(QStringLiteral("session-only")).isEmpty());
    // And the encrypted key's passphrase never reached the disk, saved key or no
    // saved key.
    assertNoFileContains(appData.path(), QByteArray(kPassphrase),
                         "a passphrase");
}

void TestMobileKeyStore::forgetSavedKeyDeletesTheFileFromDisk()
{
    // The surface that offers to keep a key also offers to delete it, and
    // "delete" has to mean the file is gone — not merely dropped from a list that
    // the next launch's directory scan would repopulate.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    ch::SshConnectionPool pool;
    MobileKeyStore store(appData.path());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("kept")));
    QVERIFY(pool.hasInMemoryIdentity());

    const QString path = savedKeyPath(appData.path(), QStringLiteral("kept"));
    QVERIFY(QFile::exists(path));

    QSignalSpy removed(&store, &MobileKeyStore::keyRemoved);
    QVERIFY2(store.forgetSavedKey(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QCOMPARE(removed.count(), 1);

    // Gone from disk, from the lists, from memory, and from the pool.
    QVERIFY(!QFile::exists(path));
    QVERIFY(store.savedKeyNames().isEmpty());
    QVERIFY(store.keyNames().isEmpty());
    QVERIFY(store.allKeyNames().isEmpty());
    QVERIFY(!store.hasKeys());
    QVERIFY(!store.isSavedOnDevice(QStringLiteral("kept")));
    QVERIFY(!pool.hasInMemoryIdentity());
    assertNoFileContains(appData.path(), keyNeedle(), "a deleted saved key");

    // And it stays gone across a relaunch, which is the claim a list-only
    // deletion would have failed.
    MobileKeyStore reopened(appData.path());
    QVERIFY(reopened.savedKeyNames().isEmpty());
    QVERIFY(!reopened.hasKeys());

    // A second deletion is refused rather than silently reported as a success.
    QVERIFY(!store.forgetSavedKey(QStringLiteral("kept")));
    QVERIFY(!store.lastError().isEmpty());
}

void TestMobileKeyStore::removeKeyDeletesTheSavedFileToo()
{
    // removeKey() is the general "forget this key". Leaving the saved file behind
    // would resurrect at the next launch exactly the credential the user just
    // removed, which is the worst possible reading of "forget".
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    MobileKeyStore store(appData.path());
    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));

    const QString path = savedKeyPath(appData.path(), QStringLiteral("kept"));
    QVERIFY(QFile::exists(path));
    QVERIFY2(store.removeKey(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QVERIFY(!QFile::exists(path));
    QVERIFY(store.savedKeyNames().isEmpty());

    MobileKeyStore reopened(appData.path());
    QVERIFY(reopened.savedKeyNames().isEmpty());
}

void TestMobileKeyStore::savingAKeyPutsNothingInQSettings()
{
    // SPEC 11.2 permits the copy in ONE place: a file in app-private storage that
    // the user asked for. A profile still carries a reference or nothing, and no
    // key byte may reach the settings store, a cache or a stray temporary. So the
    // assertion is the strong one — the saved file is the ONLY file under this
    // app's storage that contains any part of the key — which covers every one of
    // those at once.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    ch::SshConnectionPool pool;
    MobileKeyStore store(appData.path());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("kept")));
    QVERIFY(!store.keyInfo(QStringLiteral("kept")).isEmpty());
    // referenceFor() is the only value that ever leaves this class for a profile,
    // and a saved key has no reference to record: what the profile would carry is
    // an empty string, not the key and not even the path to it.
    QVERIFY(store.referenceFor(QStringLiteral("kept")).isEmpty());
    store.forgetSession();

    const QString keysDirectory = savedKeysDirectory(appData.path());
    assertNoFileOutsideContains(appData.path(), keysDirectory, keyNeedle(),
                                "a saved key");
    // The per-app sandbox as well, which is where QSettings actually writes: the
    // store under test was pointed at its own temporary directory, so nothing
    // here is exempt at all.
    assertNoFileContainsAnywhere(sandboxRoots(), keyNeedle(), "a saved key");

    // And the settings file by name, with the exact constructor the client uses,
    // so this does not depend on the scan above having found it. QStandardPaths
    // test mode puts it inside the test's own tree.
    QSettings settings(QStringLiteral("CodeHarbor"), QStringLiteral("CodeHarbor"));
    settings.sync();
    QFile ini(settings.fileName());
    if (ini.open(QIODevice::ReadOnly))
        QVERIFY(!ini.readAll().contains(keyNeedle()));

    // The saved file, meanwhile, does hold the key — otherwise the assertions
    // above would pass against a save that wrote nothing at all.
    QFile saved(savedKeyPath(appData.path(), QStringLiteral("kept")));
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QVERIFY(saved.readAll().contains(keyNeedle()));
}

// Guiless: nothing here draws, and a credential test must not depend on a
// display being reachable to prove that a key never reached disk.
QTEST_GUILESS_MAIN(TestMobileKeyStore)
#include "tst_mobilekeystore.moc"
