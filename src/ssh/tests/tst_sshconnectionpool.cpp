#include "KnownHosts.h"
#include "SshConnectionPool.h"

#include <QByteArray>
#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <memory>

using ch::KnownHosts;
using ch::SshConnectionPool;

namespace {

const QString kHost = QStringLiteral("example.test");
const QString kKeyType = QStringLiteral("ssh-ed25519");
const QByteArray kKeyBlob = QByteArrayLiteral("\x00\x0bssh-ed25519 first key");
const QByteArray kOtherKeyBlob =
    QByteArrayLiteral("\x00\x0bssh-ed25519 second key");

} // namespace

// The two halves of SshConnectionPool that need no SSH server: what its
// destructor is allowed to announce, and the known-hosts policy it applies to a
// key the handshake has already read off the wire.
class TstSshConnectionPool : public QObject {
    Q_OBJECT
private slots:
    void aTrustedKeyIsAcceptedSilently();
    void aChangedKeyIsRefusedAndReported();
    void anUnknownKeyWithNobodyToAskIsRefusedAndReported();
    void anAcceptedUnknownKeyIsTrustedAndPersisted();
    void aDeclinedUnknownKeyIsRefusedWithAnExplanation();
    void aPortedEndpointIsLookedUpAndStoredOpenSshStyle();
    void aChangedKeyNeverReachesTheTrustPrompt();
    void aRevokedKeyIsRefusedWithoutAsking();
    void anAcceptedKeyThatCannotBeStoredSaysSo();
#if CH_HAVE_LIBSSH
    void aDisconnectDuringHandshakeIsDeferredUntilTheCallReturns();
    void aNestedConnectDuringHandshakeIsRefusedWithAReason();
    void aNestedConnectDuringTheInitialTeardownIsRefused();
    void destroyingAnUnconnectedPoolChangesNoProperty();
#endif
};

void TstSshConnectionPool::aTrustedKeyIsAcceptedSilently()
{
    KnownHosts hosts;
    hosts.add(kHost, kKeyType, kKeyBlob);

    SshConnectionPool pool;
    pool.setKnownHosts(hosts);
    QSignalSpy errors(&pool, &SshConnectionPool::errorOccurred);
    QSignalSpy mismatches(&pool, &SshConnectionPool::hostKeyMismatch);

    QVERIFY(pool.applyHostKeyPolicy(kHost, 22, kKeyType, kKeyBlob));
    QCOMPARE(errors.size(), 0);
    QCOMPARE(mismatches.size(), 0);
}

// A changed key is the one refusal a user must never have to guess at: it is
// what a man-in-the-middle looks like.
void TstSshConnectionPool::aChangedKeyIsRefusedAndReported()
{
    KnownHosts hosts;
    hosts.add(kHost, kKeyType, kKeyBlob);

    SshConnectionPool pool;
    pool.setKnownHosts(hosts);
    QSignalSpy errors(&pool, &SshConnectionPool::errorOccurred);
    QSignalSpy mismatches(&pool, &SshConnectionPool::hostKeyMismatch);

    QVERIFY(!pool.applyHostKeyPolicy(kHost, 22, kKeyType, kOtherKeyBlob));
    QCOMPARE(mismatches.size(), 1);
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.at(0).at(0).toString().contains(kHost));
}

void TstSshConnectionPool::anUnknownKeyWithNobodyToAskIsRefusedAndReported()
{
    SshConnectionPool pool;  // no host-key callback installed
    QSignalSpy errors(&pool, &SshConnectionPool::errorOccurred);

    QVERIFY(!pool.applyHostKeyPolicy(kHost, 22, kKeyType, kKeyBlob));
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.at(0).at(0).toString().contains(kHost));
}

void TstSshConnectionPool::anAcceptedUnknownKeyIsTrustedAndPersisted()
{
    SshConnectionPool pool;
    QString promptedHost;
    QByteArray promptedBlob;
    pool.setHostKeyCallback([&](const QString& host, const QString&,
                                const QByteArray& blob, KnownHosts::Verdict) {
        promptedHost = host;
        promptedBlob = blob;
        return SshConnectionPool::HostKeyDecision::Accept;
    });
    QSignalSpy errors(&pool, &SshConnectionPool::errorOccurred);

    QVERIFY(pool.applyHostKeyPolicy(kHost, 22, kKeyType, kKeyBlob));
    QCOMPARE(promptedHost, kHost);
    QCOMPARE(promptedBlob, kKeyBlob);
    QCOMPARE(errors.size(), 0);
    // Trusted from here on, and visible to the caller that persists the store.
    QCOMPARE(pool.knownHosts().verify(kHost, kKeyType, kKeyBlob),
             KnownHosts::Verdict::Match);
}

// The user pressed "reject". The connection stops, and until this was fixed it
// stopped with no errorOccurred() anywhere: the window went to Error with a
// reason that existed only in the diagnostic transcript, so the banner the rest
// of the client shows for every other refusal simply never appeared.
void TstSshConnectionPool::aDeclinedUnknownKeyIsRefusedWithAnExplanation()
{
    SshConnectionPool pool;
    pool.setHostKeyCallback([](const QString&, const QString&,
                               const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Reject;
    });
    QSignalSpy errors(&pool, &SshConnectionPool::errorOccurred);

    QVERIFY(!pool.applyHostKeyPolicy(kHost, 22, kKeyType, kKeyBlob));
    QCOMPARE(errors.size(), 1);
    const QString message = errors.at(0).at(0).toString();
    // It has to name what was refused: which host, and which key type it
    // offered. A bare "connection failed" is what this replaces.
    QVERIFY2(message.contains(kHost), qPrintable(message));
    QVERIFY2(message.contains(kKeyType), qPrintable(message));

    // A refusal must not leave the key trusted for next time.
    QCOMPARE(pool.knownHosts().verify(kHost, kKeyType, kKeyBlob),
             KnownHosts::Verdict::Unknown);
}

// A non-default port is a different known-hosts identity, OpenSSH-style. The
// entry a first-use acceptance writes must be the one a later lookup finds.
void TstSshConnectionPool::aPortedEndpointIsLookedUpAndStoredOpenSshStyle()
{
    SshConnectionPool pool;
    pool.setHostKeyCallback([](const QString&, const QString&,
                               const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    QVERIFY(pool.applyHostKeyPolicy(kHost, 2222, kKeyType, kKeyBlob));
    const QString ported = SshConnectionPool::lookupHostFor(kHost, 2222);
    QCOMPARE(ported, QStringLiteral("[%1]:2222").arg(kHost));
    QCOMPARE(pool.knownHosts().verify(ported, kKeyType, kKeyBlob),
             KnownHosts::Verdict::Match);
    // And it is NOT trust for the same host on the default port.
    QCOMPARE(pool.knownHosts().verify(kHost, kKeyType, kKeyBlob),
             KnownHosts::Verdict::Unknown);
}

// The header promises it and SPEC 12.1 depends on it: a CHANGED key is a hard
// refusal, never a question. If it ever reached the callback the user would be
// shown a trust prompt for a key that contradicts one they already approved —
// which is exactly what a man-in-the-middle needs, since Accept is also the one
// answer that persists the presented key into the store.
void TstSshConnectionPool::aChangedKeyNeverReachesTheTrustPrompt()
{
    KnownHosts hosts;
    hosts.add(kHost, kKeyType, kKeyBlob);

    SshConnectionPool pool;
    pool.setKnownHosts(hosts);
    int prompts = 0;
    pool.setHostKeyCallback([&prompts](const QString&, const QString&,
                                       const QByteArray&, KnownHosts::Verdict) {
        // Accept, deliberately: if the refusal ever leaked into the prompt, the
        // most dangerous possible answer is the one that must not be reachable.
        ++prompts;
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    // A changed key of the SAME type, and a key of a type this host was never
    // trusted with — the downgrade route around the refusal.
    QVERIFY(!pool.applyHostKeyPolicy(kHost, 22, kKeyType, kOtherKeyBlob));
    QVERIFY(!pool.applyHostKeyPolicy(kHost, 22, QStringLiteral("ssh-rsa"),
                                     kOtherKeyBlob));
    QCOMPARE(prompts, 0);
    // And neither refused key was recorded, whatever the callback would have
    // answered.
    QCOMPARE(pool.knownHosts().verify(kHost, kKeyType, kKeyBlob),
             KnownHosts::Verdict::Match);
    QCOMPARE(pool.knownHosts().verify(kHost, QStringLiteral("ssh-rsa"),
                                      kOtherKeyBlob),
             KnownHosts::Verdict::Mismatch);
}

// An administrator writes @revoked into known_hosts to make one key
// unusable. The store answers Mismatch for it, so the pool must treat it as the
// same hard refusal a changed key gets: reported, flagged as a mismatch, and
// never offered to the user as a first-use decision.
void TstSshConnectionPool::aRevokedKeyIsRefusedWithoutAsking()
{
    const KnownHosts hosts = KnownHosts::parse(
        QStringLiteral("@revoked %1 %2 %3\n")
            .arg(kHost, kKeyType, QString::fromUtf8(kKeyBlob.toBase64())));

    SshConnectionPool pool;
    pool.setKnownHosts(hosts);
    int prompts = 0;
    pool.setHostKeyCallback([&prompts](const QString&, const QString&,
                                       const QByteArray&, KnownHosts::Verdict) {
        ++prompts;
        return SshConnectionPool::HostKeyDecision::Accept;
    });
    QSignalSpy errors(&pool, &SshConnectionPool::errorOccurred);
    QSignalSpy mismatches(&pool, &SshConnectionPool::hostKeyMismatch);

    QVERIFY(!pool.applyHostKeyPolicy(kHost, 22, kKeyType, kKeyBlob));
    QCOMPARE(prompts, 0);
    QCOMPARE(mismatches.size(), 1);
    QCOMPARE(errors.size(), 1);

    // A revocation grants no trust, so a DIFFERENT key at that host is still
    // ordinary first use — the refusal must not make the host unconnectable.
    QVERIFY(pool.applyHostKeyPolicy(kHost, 22, kKeyType, kOtherKeyBlob));
    QCOMPARE(prompts, 1);
}

// The host token comes from the server profile, i.e. from whatever the user
// typed, and KnownHosts::add() refuses anything that cannot round-trip as a
// single known_hosts line. The connection is still the user's to make, but the
// trust was not written down, and staying silent about that is what would make
// the same prompt reappear on every launch for no visible reason.
void TstSshConnectionPool::anAcceptedKeyThatCannotBeStoredSaysSo()
{
    // '*' is known_hosts pattern syntax, so this token can never be stored as a
    // literal host name.
    const QString unstorable = QStringLiteral("web*.example.test");

    SshConnectionPool pool;
    pool.setHostKeyCallback([](const QString&, const QString&,
                               const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });
    QSignalSpy errors(&pool, &SshConnectionPool::errorOccurred);

    QVERIFY(pool.applyHostKeyPolicy(unstorable, 22, kKeyType, kKeyBlob));
    // Accepting is not an error, so no banner is raised...
    QCOMPARE(errors.size(), 0);
    // ...but nothing was recorded, and the transcript says as much.
    QVERIFY(pool.knownHosts().entries().isEmpty());
    QVERIFY2(pool.diagnosticLog().contains(
                 QStringLiteral("trusted for this session only")),
             qPrintable(pool.diagnosticLog()));

    // The ordinary case still records silently.
    SshConnectionPool storable;
    storable.setHostKeyCallback([](const QString&, const QString&,
                                   const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });
    QVERIFY(storable.applyHostKeyPolicy(kHost, 22, kKeyType, kKeyBlob));
    QVERIFY2(!storable.diagnosticLog().contains(
                 QStringLiteral("trusted for this session only")),
             qPrintable(storable.diagnosticLog()));
}

#if CH_HAVE_LIBSSH
void TstSshConnectionPool::
    aDisconnectDuringHandshakeIsDeferredUntilTheCallReturns()
{
    // The pool parses ~/.ssh/config; point HOME at an empty directory so a
    // developer's real config cannot influence, or hang, this test.
    QTemporaryDir emptyHome;
    QVERIFY(emptyHome.isValid());
    const QByteArray realHome = qgetenv("HOME");
    QVERIFY(qputenv("HOME", QFile::encodeName(emptyHome.path())));
    const auto restoreHome = qScopeGuard([&realHome] {
        qputenv("HOME", realHome);
    });

    SshConnectionPool pool;
    bool sawConnecting = false;
    connect(&pool, &SshConnectionPool::stateChanged, &pool,
            [&pool, &sawConnecting](SshConnectionPool::State state) {
                if (state == SshConnectionPool::State::Connecting) {
                    sawConnecting = true;
                    pool.disconnectFromHost();
                }
            });

    QVERIFY(!pool.connectToHost(QStringLiteral("127.0.0.1"), 1,
                                QStringLiteral("nobody")));
    QVERIFY(sawConnecting);
    QCOMPARE(pool.state(), SshConnectionPool::State::Disconnected);
}

// The handshake is synchronous and runs Qt signals and user callbacks while
// libssh is on the stack, so a slot can call straight back into connectToHost()
// — an auto-reconnect reacting to a state change is the realistic way. Starting
// a nested handshake would leak the first session and overwrite its state, so
// it is refused. It must be refused OUT LOUD: answering a bare false left the
// caller with no signal, no state change and nothing in the log, which is
// indistinguishable from a connect that simply did not happen.
void TstSshConnectionPool::aNestedConnectDuringHandshakeIsRefusedWithAReason()
{
    // The pool parses ~/.ssh/config; point HOME at an empty directory so a
    // developer's real config cannot influence, or hang, this test.
    QTemporaryDir emptyHome;
    QVERIFY(emptyHome.isValid());
    const QByteArray realHome = qgetenv("HOME");
    QVERIFY(qputenv("HOME", QFile::encodeName(emptyHome.path())));
    const auto restoreHome = qScopeGuard([&realHome] {
        qputenv("HOME", realHome);
    });

    SshConnectionPool pool;
    QSignalSpy errors(&pool, &SshConnectionPool::errorOccurred);
    int nestedAttempts = 0;
    bool nestedResult = true;
    connect(&pool, &SshConnectionPool::stateChanged, &pool,
            [&](SshConnectionPool::State state) {
                if (state != SshConnectionPool::State::Connecting)
                    return;
                ++nestedAttempts;
                nestedResult = pool.connectToHost(QStringLiteral("127.0.0.1"),
                                                  1, QStringLiteral("nobody"));
            });

    // Port 1 is never listening, so the outer attempt fails on its own; what is
    // under test is what the nested one did while it was still running.
    QVERIFY(!pool.connectToHost(QStringLiteral("127.0.0.1"), 1,
                                QStringLiteral("nobody")));
    QCOMPARE(nestedAttempts, 1);
    QVERIFY(!nestedResult);

    bool sawBusyRefusal = false;
    for (const QList<QVariant>& arguments : errors) {
        if (arguments.at(0).toString().contains(
                QStringLiteral("already busy connecting or disconnecting")))
            sawBusyRefusal = true;
    }
    QVERIFY2(sawBusyRefusal,
             "the refused nested connect reported nothing at all");
    // And the refusal really was a refusal: no second session was started, so
    // the outer attempt still owns the outcome.
    QCOMPARE(pool.state(), SshConnectionPool::State::Error);
}

// The same re-entrancy, one moment earlier. Before it does anything else,
// connectToHost() tears down whatever session the pool already had, and that
// teardown announces itself: stateChanged(Disconnected). A slot on that signal
// — an auto-reconnect is the realistic one — used to find the pool completely
// unguarded, because the guard was only raised AFTER the teardown. The nested
// handshake then ran to completion and parked its own ssh_session in the pool,
// which the outer attempt's ssh_new() overwrote a few lines later: the nested
// session, its socket and every channel on it were leaked for the life of the
// process, and whatever the nested attempt achieved was silently thrown away.
void TstSshConnectionPool::aNestedConnectDuringTheInitialTeardownIsRefused()
{
    // The pool parses ~/.ssh/config; point HOME at an empty directory so a
    // developer's real config cannot influence, or hang, this test.
    QTemporaryDir emptyHome;
    QVERIFY(emptyHome.isValid());
    const QByteArray realHome = qgetenv("HOME");
    QVERIFY(qputenv("HOME", QFile::encodeName(emptyHome.path())));
    const auto restoreHome = qScopeGuard([&realHome] {
        qputenv("HOME", realHome);
    });

    SshConnectionPool pool;
    // Port 1 is never listening. This first attempt exists only to leave the
    // pool in State::Error, so that the SECOND attempt's opening teardown has
    // a state change to announce at all.
    QVERIFY(!pool.connectToHost(QStringLiteral("127.0.0.1"), 1,
                                QStringLiteral("nobody")));
    QCOMPARE(pool.state(), SshConnectionPool::State::Error);

    QSignalSpy errors(&pool, &SshConnectionPool::errorOccurred);
    int nestedAttempts = 0;
    bool nestedResult = true;
    connect(&pool, &SshConnectionPool::stateChanged, &pool,
            [&](SshConnectionPool::State state) {
                if (state != SshConnectionPool::State::Disconnected)
                    return;
                ++nestedAttempts;
                nestedResult = pool.connectToHost(QStringLiteral("127.0.0.1"),
                                                  1, QStringLiteral("nobody"));
            });

    QVERIFY(!pool.connectToHost(QStringLiteral("127.0.0.1"), 1,
                                QStringLiteral("nobody")));
    // The teardown announced itself exactly once, and the slot on it was told
    // no — out loud, with the same reason every other refused nested connect
    // gets. Without the guard the nested call ran a whole second handshake and
    // this message never appeared anywhere.
    QCOMPARE(nestedAttempts, 1);
    QVERIFY(!nestedResult);
    bool sawBusyRefusal = false;
    for (const QList<QVariant>& arguments : errors) {
        if (arguments.at(0).toString().contains(
                QStringLiteral("already busy connecting or disconnecting")))
            sawBusyRefusal = true;
    }
    QVERIFY2(sawBusyRefusal,
             "the nested connect during the opening teardown reported nothing");
    QCOMPARE(pool.state(), SshConnectionPool::State::Error);
}

// An unconnected pool has no live session, so its destructor has no teardown
// signal to announce. It still must not emit stateChanged() or
// diagnosticLogChanged() while transitioning its private state to
// Disconnected; the live gate checks the corresponding sessionClosing()
// behavior against a real session.
void TstSshConnectionPool::
    destroyingAnUnconnectedPoolChangesNoProperty()
{
    // The pool parses ~/.ssh/config; point HOME at an empty directory so a
    // developer's real config cannot influence, or hang, this test.
    QTemporaryDir emptyHome;
    QVERIFY(emptyHome.isValid());
    const QByteArray realHome = qgetenv("HOME");
    QVERIFY(qputenv("HOME", QFile::encodeName(emptyHome.path())));
    const auto restoreHome = qScopeGuard([&realHome] {
        qputenv("HOME", realHome);
    });

    auto pool = std::make_unique<SshConnectionPool>();
    // Port 1 is never listening, so this is refused immediately — and it leaves
    // the pool in State::Error, which is what makes the destructor's own
    // setState(Disconnected) observable at all. A pool destroyed straight out
    // of Disconnected would emit nothing either way and prove nothing.
    QVERIFY(!pool->connectToHost(QStringLiteral("127.0.0.1"), 1,
                                 QStringLiteral("nobody")));
    QCOMPARE(pool->state(), SshConnectionPool::State::Error);
    QVERIFY(!pool->diagnosticLog().isEmpty());

    QSignalSpy states(pool.get(), &SshConnectionPool::stateChanged);
    QSignalSpy logs(pool.get(), &SshConnectionPool::diagnosticLogChanged);
    QSignalSpy closing(pool.get(), &SshConnectionPool::sessionClosing);

    pool.reset();

    QCOMPARE(states.size(), 0);
    QCOMPARE(logs.size(), 0);
    // Nothing was connected, so nothing had to be announced either.
    QCOMPARE(closing.size(), 0);
}

#endif // CH_HAVE_LIBSSH

QTEST_GUILESS_MAIN(TstSshConnectionPool)
#include "tst_sshconnectionpool.moc"
