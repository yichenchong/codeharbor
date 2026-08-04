#include "SshConnectionPool.h"

#include <QRegularExpression>
#include <QSemaphore>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include <memory>

#if CH_HAVE_LIBSSH
#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#endif

using ch::SshConnectionPool;
using ch::SshLogRouter;

namespace {

// The truncation marker appendTranscriptLine() prepends, newline included. Spelt
// out here rather than shared with the implementation on purpose: if the marker
// text ever changes, this test should fail and say so.
const QString kTruncationMarker =
    QStringLiteral("… earlier SSH diagnostics discarded …\n");

#if CH_HAVE_LIBSSH

// A callback the test installs as "somebody else's" libssh logging state, so
// restoration can be checked against a value that is not null — which matters,
// because libssh REFUSES ssh_set_log_callback(nullptr) and there is therefore
// no way to observe a hook being removed.
void foreignLogCallback(int priority, const char* function, const char* buffer,
                        void* userdata)
{
    Q_UNUSED(priority);
    Q_UNUSED(function);
    Q_UNUSED(buffer);
    Q_UNUSED(userdata);
}

// Emit one real libssh log line through libssh's own dispatcher. This is the
// only way to exercise the router end to end without a server: the line travels
// the exact path a handshake's lines travel.
void emitLibsshLine(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    _ssh_log(SSH_LOG_WARNING, "tst_sshlogrouter", "%s", utf8.constData());
}

// Put this thread's libssh logging state into a known, quiet baseline. A null
// callback cannot be installed, so the baseline is a callback that discards
// everything at a verbosity that produces nothing.
void resetLibsshLoggingBaseline()
{
    ssh_set_log_callback(&foreignLogCallback);
    ssh_set_log_userdata(nullptr);
    ssh_set_log_level(SSH_LOG_NOLOG);
}

#endif // CH_HAVE_LIBSSH

} // namespace

// libssh has exactly one diagnostic logging hook, one user-data pointer and one
// verbosity level, and they are per-thread rather than per-session.
// SshConnectionPool used to install its own around each handshake, so anything
// else using libssh on that thread logged into whichever pool was connecting,
// and a pool that found no previous hook could never take its own back down
// (libssh refuses a null callback) — leaving libssh holding a pointer to a pool
// that may since have been destroyed. SshLogRouter owns that state instead.
// These tests pin the parts of it that need no SSH server: install/restore
// bookkeeping, ordering safety, and the fact that a line only ever reaches the
// route belonging to the thread that emitted it.
class TstSshLogRouter : public QObject {
    Q_OBJECT
private slots:
    void transcriptCapKeepsExactlyOneTruncationMarker();
    void transcriptBelowTheCapIsNeverMarked();
    void oneOversizedLineIsTruncatedRatherThanRetainedWhole();
#if CH_HAVE_LIBSSH
    void threadStateIsInstalledOnceAndRestoredOnlyByTheLastRelease();
    void routesReleasedOutOfOrderStillRestoreTheThreadStateExactlyOnce();
    void aReleasedRouteReceivesNothingAndReleaseIsIdempotent();
    void aRouteDestroyedWhileRegisteredDeregistersCleanly();
    void nestedRoutesRestoreTheOuterRouteWhenTheInnerGoes();
    void concurrentRoutesOnTwoThreadsNeverSeeEachOthersLines();
    void releasingARouteFromTheWrongThreadStopsItRoutingAnyway();
    void wrongThreadReleaseIsRepairedBeforeNextRoute();
    void routeOutlivingOwnerThreadIsStillWrongThread();
    void twoPoolsHandshakingConcurrentlyKeepSeparateTranscripts();
    void libsshActivityAfterAHandshakeNeverReachesThePoolsTranscript();
    void aPoolLeavesNoRouteBehindAfterAFailedHandshake();
    void aThreadWithNoPreviousHookIsLeftWithAnInertOne();
    void staticRouteAtProcessExitDoesNotUseDestroyedTls();
#endif
};

// The transcript is capped so a chatty or hostile server cannot grow it without
// bound, and the "earlier diagnostics discarded" marker must appear ONCE however
// many times the cap has been hit: each overflow removes strictly more
// characters from the front than the marker is long, so the previous marker is
// always eaten before a new one is written.
void TstSshLogRouter::transcriptCapKeepsExactlyOneTruncationMarker()
{
    constexpr qsizetype limit = SshConnectionPool::kTranscriptCharacterLimit;
    QString transcript;
    QString lastLine;
    for (int i = 0; i < 4000; ++i) {
        lastLine = QStringLiteral("libssh[3] some_libssh_function: message %1")
                       .arg(i, 6, 10, QLatin1Char('0'));
        SshConnectionPool::appendTranscriptLine(transcript, lastLine);
    }

    // Truncation leaves exactly the cap's worth of text plus the marker in
    // front of it — not the cap minus the marker, and not an unbounded amount.
    QCOMPARE(transcript.size(), limit + kTruncationMarker.size());
    QVERIFY(transcript.startsWith(kTruncationMarker));
    QCOMPARE(transcript.count(kTruncationMarker), qsizetype(1));
    // The NEWEST line always survives: truncation drops from the front.
    QVERIFY(transcript.endsWith(lastLine));
}

void TstSshLogRouter::transcriptBelowTheCapIsNeverMarked()
{
    QString transcript;
    SshConnectionPool::appendTranscriptLine(transcript, QStringLiteral("first"));
    SshConnectionPool::appendTranscriptLine(transcript, QStringLiteral("second"));
    QCOMPARE(transcript, QStringLiteral("first\nsecond"));
    QVERIFY(!transcript.contains(kTruncationMarker));
}

// The cap must hold for ONE enormous line too, not only for many small ones. A
// server (or a libssh diagnostic quoting a server's banner) can emit a single
// line larger than the whole budget; retaining it whole because no earlier line
// existed to drop would put an attacker-chosen amount of text into memory that
// is held for the life of the session.
void TstSshLogRouter::oneOversizedLineIsTruncatedRatherThanRetainedWhole()
{
    constexpr qsizetype limit = SshConnectionPool::kTranscriptCharacterLimit;
    QString transcript;
    const QString huge = QStringLiteral("HEAD")
                         + QString(limit * 2, QLatin1Char('x'))
                         + QStringLiteral("TAIL");
    SshConnectionPool::appendTranscriptLine(transcript, huge);

    QCOMPARE(transcript.size(), limit + kTruncationMarker.size());
    QVERIFY(transcript.startsWith(kTruncationMarker));
    QCOMPARE(transcript.count(kTruncationMarker), qsizetype(1));
    // The END of the line survives, which is the half a reader needs: the front
    // is what gets dropped.
    QVERIFY(transcript.endsWith(QStringLiteral("TAIL")));
    QVERIFY(!transcript.contains(QStringLiteral("HEAD")));

    // A following ordinary line still leaves exactly one marker.
    SshConnectionPool::appendTranscriptLine(transcript,
                                            QStringLiteral("next line"));
    QCOMPARE(transcript.count(kTruncationMarker), qsizetype(1));
    QVERIFY(transcript.endsWith(QStringLiteral("next line")));
}

#if CH_HAVE_LIBSSH

// The router must behave like a refcount over libssh's single set of logging
// hooks: the first route on a thread captures and replaces them, further routes
// change nothing, and only the last release puts back what was there. Restoring
// early would silence a still-running handshake; never restoring would leave the
// thread logging at maximum verbosity through a callback nobody owns.
void TstSshLogRouter::
    threadStateIsInstalledOnceAndRestoredOnlyByTheLastRelease()
{
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
    QVERIFY(!SshLogRouter::ownsThreadLoggingState());

    // Pretend another component owns libssh logging before any route exists.
    int foreignUserdata = 0;
    ssh_set_log_callback(&foreignLogCallback);
    ssh_set_log_userdata(&foreignUserdata);
    ssh_set_log_level(SSH_LOG_WARNING);

    {
        SshLogRouter::Route first([](int, const char*, const char*) {});
        QCOMPARE(SshLogRouter::activeRouteCount(), 1);
        QVERIFY(SshLogRouter::ownsThreadLoggingState());
        QVERIFY(ssh_get_log_callback() != &foreignLogCallback);
        // The verbosity the transcript has always been collected at.
        QCOMPARE(ssh_get_log_level(), int(SSH_LOG_FUNCTIONS));
        // Another component's user data must not stay visible behind the
        // router's callback.
        QVERIFY(ssh_get_log_userdata() != &foreignUserdata);

        {
            SshLogRouter::Route second([](int, const char*, const char*) {});
            QCOMPARE(SshLogRouter::activeRouteCount(), 2);
            QVERIFY(SshLogRouter::ownsThreadLoggingState());
        }

        // One route gone, one still wants diagnostics: nothing is restored yet.
        QCOMPARE(SshLogRouter::activeRouteCount(), 1);
        QVERIFY(SshLogRouter::ownsThreadLoggingState());
        QVERIFY(ssh_get_log_callback() != &foreignLogCallback);
        QCOMPARE(ssh_get_log_level(), int(SSH_LOG_FUNCTIONS));
    }

    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
    QVERIFY(!SshLogRouter::ownsThreadLoggingState());
    QVERIFY(ssh_get_log_callback() == &foreignLogCallback);
    QCOMPARE(ssh_get_log_userdata(), static_cast<void*>(&foreignUserdata));
    QCOMPARE(ssh_get_log_level(), int(SSH_LOG_WARNING));

    resetLibsshLoggingBaseline();
}

// Pools are created and destroyed in whatever order the UI destroys them, so
// routes must not have to unwind like a stack. Releasing the FIRST route while
// two later ones are alive must not restore anything, and must not corrupt the
// bookkeeping for the routes that remain.
void TstSshLogRouter::
    routesReleasedOutOfOrderStillRestoreTheThreadStateExactlyOnce()
{
    int foreignUserdata = 0;
    ssh_set_log_callback(&foreignLogCallback);
    ssh_set_log_userdata(&foreignUserdata);
    ssh_set_log_level(SSH_LOG_PROTOCOL);

    auto first =
        std::make_unique<SshLogRouter::Route>([](int, const char*, const char*) {});
    auto second =
        std::make_unique<SshLogRouter::Route>([](int, const char*, const char*) {});
    auto third =
        std::make_unique<SshLogRouter::Route>([](int, const char*, const char*) {});
    QCOMPARE(SshLogRouter::activeRouteCount(), 3);

    first.reset();  // oldest first
    QCOMPARE(SshLogRouter::activeRouteCount(), 2);
    QVERIFY(SshLogRouter::ownsThreadLoggingState());

    third.reset();  // newest next
    QCOMPARE(SshLogRouter::activeRouteCount(), 1);
    QVERIFY(SshLogRouter::ownsThreadLoggingState());
    QVERIFY(ssh_get_log_callback() != &foreignLogCallback);

    second.reset();  // the middle one closes it out
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
    QVERIFY(!SshLogRouter::ownsThreadLoggingState());
    QVERIFY(ssh_get_log_callback() == &foreignLogCallback);
    QCOMPARE(ssh_get_log_userdata(), static_cast<void*>(&foreignUserdata));
    QCOMPARE(ssh_get_log_level(), int(SSH_LOG_PROTOCOL));

    resetLibsshLoggingBaseline();
}

void TstSshLogRouter::aReleasedRouteReceivesNothingAndReleaseIsIdempotent()
{
    QStringList received;
    SshLogRouter::Route route(
        [&received](int, const char*, const char* buffer) {
            received << QString::fromUtf8(buffer ? buffer : "");
        });

    emitLibsshLine(QStringLiteral("while-registered"));
    QCOMPARE(received.size(), 1);
    QVERIFY(received.constLast().contains(QStringLiteral("while-registered")));

    route.release();
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
    emitLibsshLine(QStringLiteral("after-release"));
    QCOMPARE(received.size(), 1);

    // A second release (and the destructor's own, when this scope ends) must be
    // a no-op rather than a double decrement of the refcount.
    route.release();
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
}

// A pool destroyed while it holds a route destroys the route with it. That must
// leave the router consistent for every route that is still alive, including the
// case where the destroyed one was not the most recent.
void TstSshLogRouter::aRouteDestroyedWhileRegisteredDeregistersCleanly()
{
    QStringList survivorLines;
    SshLogRouter::Route survivor(
        [&survivorLines](int, const char*, const char* buffer) {
            survivorLines << QString::fromUtf8(buffer ? buffer : "");
        });

    QStringList doomedLines;
    {
        SshLogRouter::Route doomed(
            [&doomedLines](int, const char*, const char* buffer) {
                doomedLines << QString::fromUtf8(buffer ? buffer : "");
            });
        emitLibsshLine(QStringLiteral("belongs-to-doomed"));
    }

    QCOMPARE(SshLogRouter::activeRouteCount(), 1);
    QVERIFY(SshLogRouter::ownsThreadLoggingState());
    QCOMPARE(doomedLines.size(), 1);
    QVERIFY(survivorLines.isEmpty());

    // Routing must fall back to the survivor rather than to a dangling pointer.
    emitLibsshLine(QStringLiteral("belongs-to-survivor"));
    QCOMPARE(doomedLines.size(), 1);
    QCOMPARE(survivorLines.size(), 1);
    QVERIFY(survivorLines.constLast().contains(
        QStringLiteral("belongs-to-survivor")));
}

void TstSshLogRouter::nestedRoutesRestoreTheOuterRouteWhenTheInnerGoes()
{
    QStringList outerLines;
    SshLogRouter::Route outer(
        [&outerLines](int, const char*, const char* buffer) {
            outerLines << QString::fromUtf8(buffer ? buffer : "");
        });
    emitLibsshLine(QStringLiteral("outer-before"));

    QStringList innerLines;
    {
        SshLogRouter::Route inner(
            [&innerLines](int, const char*, const char* buffer) {
                innerLines << QString::fromUtf8(buffer ? buffer : "");
            });
        emitLibsshLine(QStringLiteral("inner-only"));
    }
    emitLibsshLine(QStringLiteral("outer-after"));

    QCOMPARE(innerLines.size(), 1);
    QVERIFY(innerLines.constLast().contains(QStringLiteral("inner-only")));
    QCOMPARE(outerLines.size(), 2);
    QVERIFY(outerLines.at(0).contains(QStringLiteral("outer-before")));
    QVERIFY(outerLines.at(1).contains(QStringLiteral("outer-after")));
}

// The bug this router exists to fix, at its smallest: two routes alive at the
// same time on different threads, each emitting real libssh log lines, must not
// see one another's. The old code had a single global user-data pointer, so
// whichever pool installed last collected everything.
void TstSshLogRouter::concurrentRoutesOnTwoThreadsNeverSeeEachOthersLines()
{
    constexpr int lineCount = 200;
    QSemaphore start;
    QStringList firstLines;
    QStringList secondLines;

    const auto worker = [&start](const QString& tag, QStringList& sink) {
        return [&start, tag, &sink] {
            SshLogRouter::Route route(
                [&sink](int, const char*, const char* buffer) {
                    sink << QString::fromUtf8(buffer ? buffer : "");
                });
            start.acquire();  // both threads log at the same time
            for (int i = 0; i < lineCount; ++i)
                emitLibsshLine(QStringLiteral("%1-%2").arg(tag).arg(i));
        };
    };

    QThread* const firstThread =
        QThread::create(worker(QStringLiteral("alpha"), firstLines));
    QThread* const secondThread =
        QThread::create(worker(QStringLiteral("beta"), secondLines));
    firstThread->start();
    secondThread->start();
    // Give both threads time to take their route before either logs.
    QTest::qWait(50);
    start.release(2);
    QVERIFY(firstThread->wait(60000));
    QVERIFY(secondThread->wait(60000));
    delete firstThread;
    delete secondThread;

    QCOMPARE(firstLines.size(), lineCount);
    QCOMPARE(secondLines.size(), lineCount);
    for (const QString& line : std::as_const(firstLines)) {
        QVERIFY2(line.contains(QStringLiteral("alpha-")),
                 qPrintable(QStringLiteral("stray line in alpha: %1").arg(line)));
        QVERIFY(!line.contains(QStringLiteral("beta-")));
    }
    for (const QString& line : std::as_const(secondLines)) {
        QVERIFY2(line.contains(QStringLiteral("beta-")),
                 qPrintable(QStringLiteral("stray line in beta: %1").arg(line)));
        QVERIFY(!line.contains(QStringLiteral("alpha-")));
    }

    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
    QVERIFY(!SshLogRouter::ownsThreadLoggingState());
}

// The same property one level up, through the real class: two pools handshaking
// at the same moment against two different addresses. No server is needed —
// both connections are refused by the loopback stack, and libssh has already
// emitted its address-resolution and socket lines by then. Each transcript must
// name its OWN address and never the other's.
void TstSshLogRouter::twoPoolsHandshakingConcurrentlyKeepSeparateTranscripts()
{
    // Both pools parse ~/.ssh/config. Point HOME at an empty directory so a
    // developer's real config cannot influence, or hang, this test.
    QTemporaryDir emptyHome;
    QVERIFY(emptyHome.isValid());
    const QByteArray realHome = qgetenv("HOME");
    QVERIFY(qputenv("HOME", QFile::encodeName(emptyHome.path())));
    const auto restoreHome = qScopeGuard([&realHome] {
        qputenv("HOME", realHome);
    });

    QSemaphore start;
    QString firstTranscript;
    QString secondTranscript;

    const auto worker = [&start](const QString& address, QString& transcript) {
        return [&start, address, &transcript] {
            SshConnectionPool pool;
            start.acquire();
            // Port 1 is never listening, so this is refused immediately.
            pool.connectToHost(address, 1, QStringLiteral("nobody"));
            transcript = pool.diagnosticLog();
        };
    };

    const QString firstAddress = QStringLiteral("127.0.0.1");
    const QString secondAddress = QStringLiteral("127.0.0.2");
    QThread* const firstThread =
        QThread::create(worker(firstAddress, firstTranscript));
    QThread* const secondThread =
        QThread::create(worker(secondAddress, secondTranscript));
    firstThread->start();
    secondThread->start();
    QTest::qWait(50);
    start.release(2);
    QVERIFY(firstThread->wait(60000));
    QVERIFY(secondThread->wait(60000));
    delete firstThread;
    delete secondThread;

    // Both really did collect libssh's own lines, not just the pool's own
    // narration — otherwise "no cross-contamination" would be vacuous.
    QVERIFY(firstTranscript.contains(QStringLiteral("libssh[")));
    QVERIFY(secondTranscript.contains(QStringLiteral("libssh[")));

    QVERIFY(firstTranscript.contains(firstAddress));
    QVERIFY2(!firstTranscript.contains(secondAddress),
             qPrintable(QStringLiteral("second server's lines leaked into the "
                                       "first transcript:\n%1")
                            .arg(firstTranscript)));
    QVERIFY(secondTranscript.contains(secondAddress));
    QVERIFY2(!secondTranscript.contains(firstAddress),
             qPrintable(QStringLiteral("first server's lines leaked into the "
                                       "second transcript:\n%1")
                            .arg(secondTranscript)));

    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
    QVERIFY(!SshLogRouter::ownsThreadLoggingState());
}

// A handshake that fails must drop its route on every return path, and the pool
// must not leave one behind when it is destroyed. Otherwise the process would
// stay at maximum libssh verbosity, logging into a freed object.
void TstSshLogRouter::aPoolLeavesNoRouteBehindAfterAFailedHandshake()
{
    QTemporaryDir emptyHome;
    QVERIFY(emptyHome.isValid());
    const QByteArray realHome = qgetenv("HOME");
    QVERIFY(qputenv("HOME", QFile::encodeName(emptyHome.path())));
    const auto restoreHome = qScopeGuard([&realHome] {
        qputenv("HOME", realHome);
    });

    int foreignUserdata = 0;
    ssh_set_log_callback(&foreignLogCallback);
    ssh_set_log_userdata(&foreignUserdata);
    ssh_set_log_level(SSH_LOG_NOLOG);

    auto pool = std::make_unique<SshConnectionPool>();
    QVERIFY(!pool->connectToHost(QStringLiteral("127.0.0.1"), 1,
                                 QStringLiteral("nobody")));
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
    QVERIFY(!SshLogRouter::ownsThreadLoggingState());
    QVERIFY(!pool->diagnosticLog().isEmpty());

    pool.reset();
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
    QVERIFY(!SshLogRouter::ownsThreadLoggingState());
    // Restored, not left raised: this is the guarantee the old per-handshake
    // scope guard gave, and the router must not lose it.
    QCOMPARE(ssh_get_log_level(), int(SSH_LOG_NOLOG));
    QVERIFY(ssh_get_log_callback() == &foreignLogCallback);
    QCOMPARE(ssh_get_log_userdata(), static_cast<void*>(&foreignUserdata));

    resetLibsshLoggingBaseline();
}

// The one thing the router cannot undo, pinned so nobody later mistakes it for
// a leak: libssh refuses ssh_set_log_callback(nullptr), so on a thread that had
// no hook at all the router's own hook stays installed after the last route
// goes. It must be INERT — the thread's original verbosity is back, and a line
// forced through anyway reaches nobody.
void TstSshLogRouter::aThreadWithNoPreviousHookIsLeftWithAnInertOne()
{
    // A brand new thread starts with libssh's thread-local logging state
    // untouched: no callback, verbosity SSH_LOG_NOLOG.
    bool sawOwnLine = false;
    bool hookStillInstalled = false;
    int levelAfterRelease = -1;
    int linesAfterRelease = 0;

    QThread* const thread = QThread::create([&] {
        QVERIFY(ssh_get_log_callback() == nullptr);
        QCOMPARE(ssh_get_log_level(), int(SSH_LOG_NOLOG));

        int received = 0;
        {
            SshLogRouter::Route route(
                [&received](int, const char*, const char*) { ++received; });
            emitLibsshLine(QStringLiteral("collected"));
        }
        sawOwnLine = received == 1;

        hookStillInstalled = ssh_get_log_callback() != nullptr;
        levelAfterRelease = ssh_get_log_level();
        // Force a line through the hook that libssh would not let the router
        // remove. It must reach nobody.
        emitLibsshLine(QStringLiteral("after-release"));
        linesAfterRelease = received - 1;
    });
    thread->start();
    QVERIFY(thread->wait(60000));
    delete thread;

    QVERIFY(sawOwnLine);
    QVERIFY2(hookStillInstalled,
             "libssh cannot uninstall a hook; the router's must remain");
    QCOMPARE(levelAfterRelease, int(SSH_LOG_NOLOG));
    QCOMPARE(linesAfterRelease, 0);
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
}

// Releasing a route from a thread that did not take it is a contract
// violation, and it used to be caught by nothing but Q_ASSERT_X — which is
// compiled out of every release build. What survived into production was
// silent: the route stayed in the owning thread's stack, so libssh lines kept
// reaching a sink whose Route was on its way out, through a pointer that
// ~Route() had already made dangling. The guard has to be a real runtime one:
// the route stops routing whichever thread ends it, the owning thread's other
// routes carry on, and the violation is reported rather than swallowed.
void TstSshLogRouter::releasingARouteFromTheWrongThreadStopsItRoutingAnyway()
{
    QSemaphore routesTaken;
    QSemaphore released;
    QStringList outerLines;
    QStringList innerLines;
    int countWhileBothHeld = 0;
    SshLogRouter::Route* innerRoute = nullptr;

    QThread* const owner = QThread::create([&] {
        SshLogRouter::Route outer(
            [&outerLines](int, const char*, const char* buffer) {
                outerLines << QString::fromUtf8(buffer ? buffer : "");
            });
        // Heap-allocated so the WRONG thread can end it while this thread is
        // still inside the route's lifetime, which is the violation.
        auto inner = std::make_unique<SshLogRouter::Route>(
            [&innerLines](int, const char*, const char* buffer) {
                innerLines << QString::fromUtf8(buffer ? buffer : "");
            });
        innerRoute = inner.get();
        countWhileBothHeld = SshLogRouter::activeRouteCount();
        emitLibsshLine(QStringLiteral("before-release"));

        routesTaken.release();
        released.acquire();

        // The inner route was ended from the main thread. Its sink must be out
        // of the picture, and the outer route must collect this line.
        emitLibsshLine(QStringLiteral("after-release"));
        inner.reset();  // the owning thread's own release: now a no-op
        emitLibsshLine(QStringLiteral("after-owner-reset"));
    });
    owner->start();
    routesTaken.acquire();

    // The violation, committed from the main thread. It is reported, not
    // swallowed: an assertion would have said nothing in a release build.
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression(QStringLiteral(
                             "route taken on another thread was released")));
    innerRoute->release();
    // The route really is gone from the process-wide count, exactly once.
    QCOMPARE(SshLogRouter::activeRouteCount(), 1);
    released.release();
    QVERIFY(owner->wait(60000));
    delete owner;

    QCOMPARE(countWhileBothHeld, 2);
    // Only the line emitted while the inner route was genuinely live reached
    // it. Everything after the wrong-thread release belongs to the outer route.
    QCOMPARE(innerLines.size(), 1);
    QVERIFY(innerLines.constLast().contains(QStringLiteral("before-release")));
    QCOMPARE(outerLines.size(), 2);
    QVERIFY(outerLines.at(0).contains(QStringLiteral("after-release")));
    QVERIFY(outerLines.at(1).contains(QStringLiteral("after-owner-reset")));

    // No double decrement from the owning thread's later release(), and that
    // thread's libssh state went back with its own last route.
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
}

void TstSshLogRouter::wrongThreadReleaseIsRepairedBeforeNextRoute()
{
    QSemaphore routeTaken;
    QSemaphore released;
    SshLogRouter::Route* route = nullptr;
    bool stateRestored = false;
    bool inertHookRemains = false;
    int levelAfter = -1;

    QThread* const worker = QThread::create([&] {
        auto doomed =
            std::make_unique<SshLogRouter::Route>([](int, const char*,
                                                      const char*) {});
        route = doomed.get();
        routeTaken.release();
        released.acquire();

        // The wrong-thread release leaves an inactive entry on this thread's
        // stack. Acquiring a new route must prune it and restore the original
        // state before installing the replacement route.
        {
            SshLogRouter::Route replacement(
                [](int, const char*, const char*) {});
            stateRestored = SshLogRouter::ownsThreadLoggingState();
        }
        levelAfter = ssh_get_log_level();
        inertHookRemains = ssh_get_log_callback() != nullptr;
    });
    worker->start();
    routeTaken.acquire();

    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral(
            "route taken on another thread was released")));
    route->release();
    released.release();
    QVERIFY(worker->wait(60000));
    delete worker;

    QVERIFY(stateRestored);
    QVERIFY(inertHookRemains);
    QCOMPARE(levelAfter, int(SSH_LOG_NOLOG));
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
}
// Native thread identifiers may be recycled immediately after a worker exits.
// Keep the Route alive across that exit and release it from the main thread:
// the release must remain a wrong-thread release even if Windows gives the
// next thread the worker's former identifier.
void TstSshLogRouter::routeOutlivingOwnerThreadIsStillWrongThread()
{
    QSemaphore routeTaken;
    std::unique_ptr<SshLogRouter::Route> route;

    // Construct the route on the worker, then publish it only after its
    // constructor has recorded the worker's per-thread identity.
    QThread* const owner = QThread::create([&route, &routeTaken] {
        route = std::make_unique<SshLogRouter::Route>(
            [](int, const char*, const char*) {});
        routeTaken.release();
    });
    owner->start();
    routeTaken.acquire();
    QVERIFY(owner->wait(60000));
    delete owner;

    QVERIFY(route != nullptr);
    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral(
            "route taken on another thread was released")));
    route->release();
    route.reset();
    QCOMPARE(SshLogRouter::activeRouteCount(), 0);
    resetLibsshLoggingBaseline();
}

// The concrete regression the router removes. The old code installed
// SshConnectionPool's own callback with the pool as libssh's user data and
// "restored" the previous one on the way out — but the previous one was
// normally none, and libssh refuses a null callback, so the pool's callback and
// the pointer to the pool stayed installed for the rest of the process. Every
// later libssh line on that thread, from any session, was appended to that one
// pool's transcript; after the pool was destroyed the same path wrote through a
// dangling pointer. With the router, a pool's route ends with its handshake and
// later libssh activity reaches nobody.
void TstSshLogRouter::
    libsshActivityAfterAHandshakeNeverReachesThePoolsTranscript()
{
    QTemporaryDir emptyHome;
    QVERIFY(emptyHome.isValid());
    const QByteArray realHome = qgetenv("HOME");
    QVERIFY(qputenv("HOME", QFile::encodeName(emptyHome.path())));
    const auto restoreHome = qScopeGuard([&realHome] {
        qputenv("HOME", realHome);
    });
    resetLibsshLoggingBaseline();

    SshConnectionPool pool;
    QVERIFY(!pool.connectToHost(QStringLiteral("127.0.0.1"), 1,
                                QStringLiteral("nobody")));
    const QString transcriptAfterHandshake = pool.diagnosticLog();
    QVERIFY(transcriptAfterHandshake.contains(QStringLiteral("libssh[")));

    // Somebody else raises the verbosity and drives libssh on this same thread.
    ssh_set_log_level(SSH_LOG_FUNCTIONS);
    emitLibsshLine(QStringLiteral("unrelated-libssh-activity"));
    ssh_session other = ssh_new();
    QVERIFY(other != nullptr);
    const char* const otherHost = "127.0.0.1";
    unsigned int otherPort = 1;
    ssh_options_set(other, SSH_OPTIONS_HOST, otherHost);
    ssh_options_set(other, SSH_OPTIONS_PORT, &otherPort);
    ssh_connect(other);
    ssh_free(other);

    QCOMPARE(pool.diagnosticLog(), transcriptAfterHandshake);

    resetLibsshLoggingBaseline();
}
// Keep one route alive until process shutdown. The route release then runs
// during static destruction, after Qt and test objects have gone away; its
// heap-backed route list must still be valid and dispatch must tolerate the
// callback remaining installed afterward.
void TstSshLogRouter::staticRouteAtProcessExitDoesNotUseDestroyedTls()
{
    static const auto route =
        std::make_unique<SshLogRouter::Route>(
            [](int, const char*, const char*) {});
    QVERIFY(route != nullptr);
    QVERIFY(SshLogRouter::activeRouteCount() > 0);
}

#endif // CH_HAVE_LIBSSH

QTEST_GUILESS_MAIN(TstSshLogRouter)

#include "tst_sshlogrouter.moc"
