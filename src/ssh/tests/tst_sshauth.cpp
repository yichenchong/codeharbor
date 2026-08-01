#include "SshConnectionPool.h"

#include <QString>
#include <QtTest/QtTest>

#if CH_HAVE_LIBSSH
#include <libssh/libssh.h>
#endif

using ch::SshConnectionPool;
using AuthMethods = SshConnectionPool::AuthMethods;
using AuthOutcome = SshConnectionPool::AuthOutcome;
using AuthRung = SshConnectionPool::AuthRung;
using AuthRungsTried = SshConnectionPool::AuthRungsTried;

namespace {

constexpr AuthMethods kBoth{true, true};
constexpr AuthMethods kPublicKeyOnly{true, false};
constexpr AuthMethods kPasswordOnly{false, true};

} // namespace

// Multi-step authentication (SPEC 12.1). An SSH server configured with
// `AuthenticationMethods publickey,password` accepts the client's key and then
// answers SSH_AUTH_PARTIAL: the method WAS accepted, and another one is
// required. The decision that turns those answers into the next method to try
// is a pure function precisely so it can be driven here, in every order a
// server can ask for, without a network.
class TstSshAuth : public QObject {
    Q_OBJECT
private slots:
    void publicKeyLadderIsClimbedBeforeAnySecretIsAsked();
    void partialSuccessRoutesToTheMethodTheServerStillWants();
    void serverThatKeepsReportingPartialRunsOutOfRungs();
    void passwordOnlyServerSpendsNoPublicKeyAttempt();
    void withoutACredentialCallbackOnlySilentMethodsAreOffered();
    void aRungIsNeverClimbedTwiceEvenIfStillOffered();
    void keyboardInteractiveIsClimbedAfterPasswordWithACallback();
    void everyMethodOfferedIsClimbedOnceThenExhausted();
    void aServerOfferingNoSupportedMethodIsExhaustedImmediately();
#if CH_HAVE_LIBSSH
    void partialIsClassifiedApartFromSuccessAndFailure();
    void anUnknownMethodMaskFallsBackToTryingBoth();
#endif
};

// Nothing that needs a human comes first: agent, then the key files, and only
// then a passphrase. A user must not be interrupted for a secret while a key
// that authenticates silently has not been tried.
void TstSshAuth::publicKeyLadderIsClimbedBeforeAnySecretIsAsked()
{
    AuthRungsTried tried;
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, true),
             AuthRung::Agent);
    tried.add(AuthRung::Agent);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, true),
             AuthRung::KeyFile);
    tried.add(AuthRung::KeyFile);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, true),
             AuthRung::KeyPassphrase);
    tried.add(AuthRung::KeyPassphrase);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, true),
             AuthRung::Password);
    tried.add(AuthRung::Password);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, true),
             AuthRung::Exhausted);
}

// The regression this whole change exists for. The server took the key,
// reported partial success, and now offers password only. The ladder must
// follow the server onto the password rung instead of trying more keys — and it
// must do so in EITHER order the server can require the two methods in.
void TstSshAuth::partialSuccessRoutesToTheMethodTheServerStillWants()
{
    // `AuthenticationMethods publickey,password`: the key was accepted, the
    // remaining offer is password.
    AuthRungsTried afterKey;
    afterKey.add(AuthRung::Agent);
    QCOMPARE(SshConnectionPool::nextAuthRung(afterKey, kPasswordOnly, true),
             AuthRung::Password);

    // `AuthenticationMethods password,publickey`: the password was accepted and
    // the server now wants a key. The ladder must go BACK to the key rungs
    // rather than declaring itself finished.
    AuthRungsTried afterPassword;
    afterPassword.add(AuthRung::Password);
    QCOMPARE(SshConnectionPool::nextAuthRung(afterPassword, kPublicKeyOnly, true),
             AuthRung::Agent);
}

// A server may keep answering "partial" forever. The ladder is bounded by the
// number of rungs, not by the server's answers: four steps, all distinct, then
// Exhausted. Without that bound the handshake loop would never return.
void TstSshAuth::serverThatKeepsReportingPartialRunsOutOfRungs()
{
    AuthRungsTried tried;
    QList<AuthRung> climbed;
    for (int step = 0; step < 100; ++step) {
        const AuthRung rung =
            SshConnectionPool::nextAuthRung(tried, kBoth, true);
        if (rung == AuthRung::Exhausted)
            break;
        QVERIFY2(!climbed.contains(rung), "a rung was offered twice");
        climbed.append(rung);
        tried.add(rung);  // every step answered SSH_AUTH_PARTIAL
    }
    QCOMPARE(climbed.size(), 4);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, true),
             AuthRung::Exhausted);
}

// A server offering password only must not have public-key attempts thrown at
// it: OpenSSH counts every one against MaxAuthTries and can drop the
// connection before the method it actually wants is reached.
void TstSshAuth::passwordOnlyServerSpendsNoPublicKeyAttempt()
{
    AuthRungsTried tried;
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kPasswordOnly, true),
             AuthRung::Password);
    tried.add(AuthRung::Password);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kPasswordOnly, true),
             AuthRung::Exhausted);
}

// With no credential callback installed there is nobody to ask, so the two
// rungs that need a secret are unreachable — the handshake must fail rather
// than call a callback that does not exist.
void TstSshAuth::withoutACredentialCallbackOnlySilentMethodsAreOffered()
{
    AuthRungsTried tried;
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, false),
             AuthRung::Agent);
    tried.add(AuthRung::Agent);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, false),
             AuthRung::KeyFile);
    tried.add(AuthRung::KeyFile);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, false),
             AuthRung::Exhausted);

    AuthRungsTried passwordServer;
    QCOMPARE(
        SshConnectionPool::nextAuthRung(passwordServer, kPasswordOnly, false),
        AuthRung::Exhausted);
}

// The tried-set is what the handshake loop relies on to make progress: a method
// the server still advertises after refusing it must not come back round, or a
// wrong password would be sent to the server over and over.
void TstSshAuth::aRungIsNeverClimbedTwiceEvenIfStillOffered()
{
    AuthRungsTried tried;
    tried.add(AuthRung::Agent);
    tried.add(AuthRung::KeyFile);
    tried.add(AuthRung::KeyPassphrase);
    tried.add(AuthRung::Password);
    QVERIFY(tried.contains(AuthRung::Agent));
    QVERIFY(tried.contains(AuthRung::KeyFile));
    QVERIFY(tried.contains(AuthRung::KeyPassphrase));
    QVERIFY(tried.contains(AuthRung::Password));
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kBoth, true),
             AuthRung::Exhausted);
}

// SH14: a server that publishes the account password through
// keyboard-interactive (PAM: PasswordAuthentication no, KbdInteractive yes) is
// no longer a dead end. The rung sits after plain password — both carry the
// same single secret — and it needs a callback, exactly like the password rung.
void TstSshAuth::keyboardInteractiveIsClimbedAfterPasswordWithACallback()
{
    constexpr AuthMethods kKbdOnly{false, false, true};
    AuthRungsTried tried;
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kKbdOnly, true),
             AuthRung::KeyboardInteractive);
    tried.add(AuthRung::KeyboardInteractive);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kKbdOnly, true),
             AuthRung::Exhausted);

    // No callback means nobody to answer the challenge, so the rung is
    // unreachable rather than calling a callback that does not exist.
    AuthRungsTried noCallback;
    QCOMPARE(SshConnectionPool::nextAuthRung(noCallback, kKbdOnly, false),
             AuthRung::Exhausted);

    // Offered alongside plain password, the password rung is spent first; only
    // then does the ladder fall back to the keyboard-interactive carrier.
    constexpr AuthMethods kPasswordAndKbd{false, true, true};
    AuthRungsTried both;
    QCOMPARE(SshConnectionPool::nextAuthRung(both, kPasswordAndKbd, true),
             AuthRung::Password);
    both.add(AuthRung::Password);
    QCOMPARE(SshConnectionPool::nextAuthRung(both, kPasswordAndKbd, true),
             AuthRung::KeyboardInteractive);
}

// The rung count still bounds the loop with every method on offer: five
// distinct rungs, each climbed at most once, then Exhausted — a server that
// keeps answering "partial" cannot spin the handshake forever.
void TstSshAuth::everyMethodOfferedIsClimbedOnceThenExhausted()
{
    constexpr AuthMethods kAll{true, true, true};
    AuthRungsTried tried;
    QList<AuthRung> climbed;
    for (int step = 0; step < 100; ++step) {
        const AuthRung rung =
            SshConnectionPool::nextAuthRung(tried, kAll, true);
        if (rung == AuthRung::Exhausted)
            break;
        QVERIFY2(!climbed.contains(rung), "a rung was offered twice");
        climbed.append(rung);
        tried.add(rung);  // every step answered SSH_AUTH_PARTIAL
    }
    QCOMPARE(climbed.size(), 5);
    QVERIFY(climbed.contains(AuthRung::KeyboardInteractive));
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kAll, true),
             AuthRung::Exhausted);
}

// A server may offer only methods this client cannot climb (host-based,
// GSSAPI). The ladder must then be finished on the spot rather than throwing a
// key or a password at a method the server never asked for: OpenSSH counts each
// attempt against MaxAuthTries and drops the connection, which turns a clear
// "no supported method" into a confusing mid-handshake disconnect.
void TstSshAuth::aServerOfferingNoSupportedMethodIsExhaustedImmediately()
{
    constexpr AuthMethods kNothing{false, false, false};
    AuthRungsTried tried;
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kNothing, true),
             AuthRung::Exhausted);
    QCOMPARE(SshConnectionPool::nextAuthRung(tried, kNothing, false),
             AuthRung::Exhausted);

    // Public-key-only with every key rung already spent and no callback: the
    // secret-bearing rungs stay unreachable instead of looping.
    AuthRungsTried spent;
    spent.add(AuthRung::Agent);
    spent.add(AuthRung::KeyFile);
    QCOMPARE(SshConnectionPool::nextAuthRung(spent, kPublicKeyOnly, false),
             AuthRung::Exhausted);
    // With a callback the passphrase rung is still available on that same offer.
    QCOMPARE(SshConnectionPool::nextAuthRung(spent, kPublicKeyOnly, true),
             AuthRung::KeyPassphrase);
}

#if CH_HAVE_LIBSSH

// SSH_AUTH_PARTIAL used to be indistinguishable from a refusal, which is what
// made a two-method server unusable: the accepted key was read as a failure.
// Against the real libssh constants, partial is its own answer.
void TstSshAuth::partialIsClassifiedApartFromSuccessAndFailure()
{
    QCOMPARE(SshConnectionPool::classifyAuthResult(SSH_AUTH_SUCCESS),
             AuthOutcome::Granted);
    QCOMPARE(SshConnectionPool::classifyAuthResult(SSH_AUTH_PARTIAL),
             AuthOutcome::Partial);
    QCOMPARE(SshConnectionPool::classifyAuthResult(SSH_AUTH_DENIED),
             AuthOutcome::Refused);
    QCOMPARE(SshConnectionPool::classifyAuthResult(SSH_AUTH_ERROR),
             AuthOutcome::Refused);
    QCOMPARE(SshConnectionPool::classifyAuthResult(SSH_AUTH_AGAIN),
             AuthOutcome::Refused);
    // Keyboard-interactive's "more prompts follow" is not partial success. It
    // reaches this classifier only when the conversation was abandoned mid-way
    // (nobody supplied an answer, libssh refused one, or the round cap was
    // hit), and every one of those ends the rung rather than advancing it.
    QCOMPARE(SshConnectionPool::classifyAuthResult(SSH_AUTH_INFO),
             AuthOutcome::Refused);
}

// ssh_userauth_list() answers 0 when the server never sent a method list, and
// SSH_AUTH_ERROR (-1) when the query itself failed. Neither may be read as "no
// method is allowed" — the client would then refuse to try anything at all and
// every connection would fail.
void TstSshAuth::anUnknownMethodMaskFallsBackToTryingBoth()
{
    const AuthMethods unknown =
        SshConnectionPool::methodsFromMask(SSH_AUTH_METHOD_UNKNOWN);
    QVERIFY(unknown.publicKey);
    QVERIFY(unknown.password);
    QVERIFY(unknown.keyboardInteractive);

    // The error return is a NEGATIVE int, not a bitmask. Decoding it as one
    // happens to set every bit today, which reaches the right answer by
    // accident; the fallback has to be reached on purpose so a future method
    // bit cannot silently be reported as offered.
    const AuthMethods errored =
        SshConnectionPool::methodsFromMask(SSH_AUTH_ERROR);
    QVERIFY(errored.publicKey);
    QVERIFY(errored.password);
    QVERIFY(errored.keyboardInteractive);

    const AuthMethods both = SshConnectionPool::methodsFromMask(
        SSH_AUTH_METHOD_PUBLICKEY | SSH_AUTH_METHOD_PASSWORD);
    QVERIFY(both.publicKey);
    QVERIFY(both.password);

    const AuthMethods keyOnly =
        SshConnectionPool::methodsFromMask(SSH_AUTH_METHOD_PUBLICKEY);
    QVERIFY(keyOnly.publicKey);
    QVERIFY(!keyOnly.password);

    const AuthMethods passwordOnly =
        SshConnectionPool::methodsFromMask(SSH_AUTH_METHOD_PASSWORD);
    QVERIFY(!passwordOnly.publicKey);
    QVERIFY(passwordOnly.password);

    // GSSAPI remains unsupported, but keyboard-interactive is now a rung this
    // client can climb: a PAM/keyboard-interactive server is no longer offered
    // nothing at all.
    const AuthMethods interactive = SshConnectionPool::methodsFromMask(
        SSH_AUTH_METHOD_INTERACTIVE | SSH_AUTH_METHOD_GSSAPI_MIC);
    QVERIFY(!interactive.publicKey);
    QVERIFY(!interactive.password);
    QVERIFY(interactive.keyboardInteractive);

    // A mask that IS present but names only methods this client cannot climb
    // must decode to nothing, not to the "the server did not say, try
    // everything" fallback: 0 is the only value that means "not told".
    const AuthMethods unsupportedOnly = SshConnectionPool::methodsFromMask(
        SSH_AUTH_METHOD_NONE | SSH_AUTH_METHOD_HOSTBASED
        | SSH_AUTH_METHOD_GSSAPI_MIC);
    QVERIFY(!unsupportedOnly.publicKey);
    QVERIFY(!unsupportedOnly.password);
    QVERIFY(!unsupportedOnly.keyboardInteractive);
}

#endif // CH_HAVE_LIBSSH

QTEST_GUILESS_MAIN(TstSshAuth)
#include "tst_sshauth.moc"
