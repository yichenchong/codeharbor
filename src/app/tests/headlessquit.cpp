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
// after load, asks the application to quit through its own event loop. The app
// then follows the exact path a window close takes: exec() returns, main()'s
// objects are destroyed, QSettings flushes, the process exits 0.
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

#include <QCoreApplication>
#include <QMetaObject>
#include <Qt>

#include <errno.h> // program_invocation_short_name

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

void quitWhenUp(std::chrono::milliseconds delay)
{
    std::this_thread::sleep_for(delay);
    // The application object is created inside main(), long after this shim's
    // constructor runs, so wait for it rather than assuming it exists.
    for (int i = 0; i < 600 && QCoreApplication::instance() == nullptr; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr)
        return;
    // Queued: posting an event is the one cross-thread-safe way to reach the
    // GUI thread, and it makes the quit happen on the app's own event loop.
    QMetaObject::invokeMethod(app, "quit", Qt::QueuedConnection);
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
    std::thread(quitWhenUp, std::chrono::milliseconds(ms)).detach();
}

} // namespace
