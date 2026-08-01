// Headless clean-quit shim for the live UI-shell gate (tst_liveshell).
//
// The shipped `codeharbor` binary is a GUI application: it leaves main() only
// when its event loop stops, which on a desktop happens when the user closes
// the last window. Under QT_QPA_PLATFORM=offscreen there is no window manager
// to deliver that close, and Qt installs no SIGTERM handler — signalling the
// process kills it outright (exit 143), skipping every destructor and the
// SessionBootstrap teardown that closes the remote session's channels. A gate
// that can only kill the app can never assert that the app shuts down cleanly.
//
// This shim is LD_PRELOAD'ed into the *unmodified* binary and, CH_QUIT_AFTER_MS
// after the application object comes up, asks the application to quit through
// its own event loop. The app then follows the exact path a window close takes:
// exec() returns, main()'s objects are destroyed, QSettings flushes, the
// process exits 0.
//
// LD_PRELOAD applies to every program exec'd with it — wrapper processes such
// as `env`, `timeout` or `sh`, and WebEngine's helper processes, all load this
// too. Only the process whose executable basename matches CH_QUIT_TARGET
// (default "codeharbor") acts; the rest return immediately. The trigger is
// deliberately NOT unset from the environment, because that would consume it
// inside a wrapper and leave the real target running forever.
//
// Built as a MODULE library used only as an LD_PRELOAD by the live gate; it is
// not linked into any shipped artifact.

// LD_PRELOAD, program_invocation_short_name and __attribute__((constructor)) are
// all Linux/glibc facilities (MSVC rejects the constructor attribute outright),
// so the shim is Linux-only. Off Linux this file compiles to an empty module:
// the ch_headlessquit target still exists, so the live gate's
// $<TARGET_FILE:ch_headlessquit> reference keeps configuring, and the relaunch
// path that would inject it QSKIPs without CH_LIVE_SSH anyway.
#ifdef __linux__

#include <QCoreApplication>
#include <QObject>
#include <QTimer>

#include <errno.h> // program_invocation_short_name

#include <cstdlib>
#include <cstring>

namespace {

// Read once in install(), before any QCoreApplication exists; read again only
// from armQuit() on the main thread. Never written concurrently.
int g_quitAfterMs = 0;

// Runs as a Qt pre-routine: QCoreApplication's constructor calls these from the
// main thread, after it has published itself through QCoreApplication::self and
// after it has created the thread's event dispatcher, so instance() is valid
// here and a timer can be started.
//
// LIFETIME, and why this is not the earlier background thread. The first
// version of this shim spawned a detached std::thread that polled
// QCoreApplication::instance() and then called
// QMetaObject::invokeMethod(app, "quit"). That is a use-after-free: nothing
// stops the main thread from destroying the application between the poll that
// reads the pointer and the dereference that uses it, and a second null check
// cannot help because the pointer it re-reads can go stale just as fast. The
// window is only closable by owning the lifetime rather than sampling it.
//
// So there is no second thread any more. The timer is created here, on the
// application's own thread, as a CHILD of the application object. Two
// consequences make the window not merely narrow but nonexistent: the timer
// cannot outlive its parent (~QObject destroys children, and the timer is
// stopped by its own destructor before the application's storage goes away),
// and every access to the application happens on the thread that would be
// doing the destroying, so there is no interleaving to lose.
void armQuit()
{
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr)
        return; // Not reachable from a pre-routine; costs nothing to say so.
    QTimer* timer = new QTimer(app);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, app, &QCoreApplication::quit);
    timer->start(g_quitAfterMs);
}

__attribute__((constructor)) void install()
{
    const char* spec = std::getenv("CH_QUIT_AFTER_MS");
    if (spec == nullptr || *spec == '\0')
        return;

    const char* target = std::getenv("CH_QUIT_TARGET");
    if (target == nullptr || *target == '\0')
        target = "codeharbor";
    if (program_invocation_short_name == nullptr
        || std::strcmp(program_invocation_short_name, target) != 0)
        return;

    const long ms = std::strtol(spec, nullptr, 10);
    if (ms <= 0)
        return;
    // The timer takes an int; clamp rather than let a silly environment value
    // wrap into a negative interval.
    g_quitAfterMs = ms > 3600000 ? 3600000 : static_cast<int>(ms);

    // Deferred to QCoreApplication's constructor: the app object is created
    // inside main(), long after this shim's constructor runs, so the arming has
    // to happen when the app announces itself rather than by polling for it.
    qAddPreRoutine(&armQuit);
}

} // namespace

#endif // __linux__
