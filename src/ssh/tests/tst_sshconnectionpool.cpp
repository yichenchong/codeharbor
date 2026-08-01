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
#if CH_HAVE_LIBSSH
    void destroyingAPoolAnnouncesTheSessionButChangesNoProperty();
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

#if CH_HAVE_LIBSSH

// A destructor that emits Qt signals hands every connected slot — and every QML
// property binding, because diagnosticLog is a Q_PROPERTY — an object that is
// on its way out. Teardown therefore announces exactly ONE thing,
// sessionClosing(), which is load-bearing: it is how a channel device that
// outlives its pool is told to drop its channel handle before the handles are
// freed. It fires only when there is a live session to close, so what is
// checkable without a server is the other half, and it is the half that used to
// be wrong on every teardown: stateChanged() and diagnosticLogChanged() must
// stay quiet. (The live gate, tst_livessh, is what exercises sessionClosing()
// against a real session.)
void TstSshConnectionPool::
    destroyingAPoolAnnouncesTheSessionButChangesNoProperty()
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
