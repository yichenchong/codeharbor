// The LD_PRELOAD clean-quit shim (headlessquit.cpp), which until now had no
// test of its own — it was only ever exercised indirectly by the live UI-shell
// gate, which QSKIPs without an SSH fixture, so a broken shim could ship
// unnoticed on any machine without one.
//
// What is asserted is the shim's LIFETIME contract, not a timing margin. The
// shim's job is to reach into a running application from outside it, and the
// only safe way to do that is to let the application OWN the thing that does
// the reaching. So the host process (headlessquithost.cpp) checks, and reports
// through its exit code, that the quit came from a QTimer parented to the
// application and that nothing arrived as a queued call from another thread.
//
// This is what makes the earlier design's use-after-free impossible rather than
// merely unlikely: that version polled QCoreApplication::instance() from a
// detached thread and dereferenced the sampled pointer, with nothing preventing
// the main thread from destroying the application in between.
#include <QByteArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringLiteral>
#include <QTest>
#include <QtGlobal>

namespace {

// Mirrors the kExit* constants in headlessquithost.cpp.
constexpr int kExitClean = 0;
constexpr int kExitNeverQuit = 3;

constexpr int kQuitAfterMs = 300;
// The host's own watchdog fires at 5 s; this only has to outlast it.
constexpr int kProcessTimeoutMs = 30000;

} // namespace

class TestHeadlessQuit : public QObject {
    Q_OBJECT

private slots:
    void quitIsDeliveredByATimerTheApplicationOwns();
    void doesNothingWithoutTheTrigger();
    void doesNothingInANonTargetProcess();

private:
    int runHost(const QString& quitAfterMs, const QString& target, QString* output);
};

// Runs the host under the shim. Empty `quitAfterMs` leaves CH_QUIT_AFTER_MS
// unset; empty `target` leaves CH_QUIT_TARGET unset (the shim then defaults to
// "codeharbor", which the host is not).
int TestHeadlessQuit::runHost(const QString& quitAfterMs, const QString& target,
                              QString* output)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    // LD_PRELOAD applies to the whole process tree, which is the point: the
    // shim must decide for itself whether it is in the target process.
    env.insert(QStringLiteral("LD_PRELOAD"), QStringLiteral(CH_HEADLESSQUIT_SHIM));
    env.remove(QStringLiteral("CH_QUIT_AFTER_MS"));
    env.remove(QStringLiteral("CH_QUIT_TARGET"));
    if (!quitAfterMs.isEmpty())
        env.insert(QStringLiteral("CH_QUIT_AFTER_MS"), quitAfterMs);
    if (!target.isEmpty())
        env.insert(QStringLiteral("CH_QUIT_TARGET"), target);

    QProcess host;
    host.setProcessEnvironment(env);
    host.setProcessChannelMode(QProcess::MergedChannels);
    host.start(QStringLiteral(CH_HEADLESSQUIT_HOST), {});
    if (!host.waitForStarted(kProcessTimeoutMs)) {
        *output = QStringLiteral("could not start host: %1").arg(host.errorString());
        return -1;
    }
    if (!host.waitForFinished(kProcessTimeoutMs)) {
        host.kill();
        host.waitForFinished(5000);
        *output = QStringLiteral("host never exited: %1")
                      .arg(QString::fromUtf8(host.readAll()));
        return -1;
    }
    *output = QString::fromUtf8(host.readAll());
    if (host.exitStatus() != QProcess::NormalExit) {
        *output = QStringLiteral("host crashed: %1").arg(*output);
        return -1;
    }
    return host.exitCode();
}

// THE ONE THAT MATTERS. Against the pre-fix shim the host exits 4: the quit
// arrives, but from a detached thread that owns nothing, so the application has
// no child timer and the pointer that thread dereferences is one it merely
// sampled. Against the fixed shim the timer is the application's child, armed
// on the application's own thread from a Qt pre-routine, and the exit is 0.
void TestHeadlessQuit::quitIsDeliveredByATimerTheApplicationOwns()
{
    QString output;
    const int code = runHost(QString::number(kQuitAfterMs),
                             QStringLiteral(CH_HEADLESSQUIT_HOST_NAME), &output);
    QVERIFY2(code == kExitClean, qPrintable(QStringLiteral("exit %1:\n%2")
                                                .arg(code)
                                                .arg(output)));
}

// The shim is loaded into every process in the tree; without the trigger it
// must be inert, or `env`, `sh` and WebEngine's helpers would all start quitting
// themselves.
void TestHeadlessQuit::doesNothingWithoutTheTrigger()
{
    QString output;
    const int code = runHost(QString(), QStringLiteral(CH_HEADLESSQUIT_HOST_NAME), &output);
    QVERIFY2(code == kExitNeverQuit, qPrintable(QStringLiteral("exit %1:\n%2")
                                                    .arg(code)
                                                    .arg(output)));
}

void TestHeadlessQuit::doesNothingInANonTargetProcess()
{
    QString output;
    const int code = runHost(QString::number(kQuitAfterMs),
                             QStringLiteral("not-the-host"), &output);
    QVERIFY2(code == kExitNeverQuit, qPrintable(QStringLiteral("exit %1:\n%2")
                                                    .arg(code)
                                                    .arg(output)));
}

QTEST_GUILESS_MAIN(TestHeadlessQuit)
#include "tst_headlessquit.moc"
