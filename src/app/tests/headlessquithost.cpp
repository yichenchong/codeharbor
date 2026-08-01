// Host process for tst_headlessquit: the smallest possible stand-in for the
// shipped GUI binary that headlessquit.cpp is LD_PRELOAD'ed into. It is a
// QGuiApplication under QT_QPA_PLATFORM=offscreen for the same reason the real
// gate is: the shim arms itself from a Qt pre-routine, which runs inside
// QCoreApplication's constructor — i.e. before QGuiApplication has finished
// building itself — so "can a timer be started that early in a GUI app" is part
// of what has to be proven, and a QCoreApplication host would not prove it.
//
// It reports through its exit code (see kExit* below) so the driver stays a
// plain process assertion.
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <Qt>

#include <cstdio>
#include <cstdlib>

namespace {

// Exit codes. 0 is "the shim quit us, and it did so the way it is supposed to".
constexpr int kExitClean = 0;
constexpr int kExitNeverQuit = 3;    // watchdog: no quit arrived at all
constexpr int kExitNotAppOwned = 4;  // quit arrived, but nothing was parented to the app
constexpr int kExitCrossThread = 5;  // something reached the app as a queued call
constexpr int kExitBadExecCode = 6;  // exec() returned non-zero

constexpr int kWatchdogMs = 5000;

// Records whether anything reached the application object as a queued
// cross-thread method call. That is precisely how the shim used to work: a
// detached thread sampled QCoreApplication::instance() and then posted "quit"
// to the pointer it had sampled, which is a use-after-free if the main thread
// destroys the application in between. Nothing in this program posts to the
// application, so any MetaCall here came from outside the app's own thread.
class MetaCallProbe : public QObject {
public:
    bool sawMetaCall = false;

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::MetaCall)
            sawMetaCall = true;
        return false;
    }
};

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    // The ownership claim, checked the moment the constructor returns: the shim
    // must have armed its quit as a DIRECT CHILD of the application, so it
    // cannot outlive the object it is going to touch. Nothing else in this
    // program parents anything to the application, so a direct-child QTimer can
    // only have come from the shim.
    const QList<QTimer*> owned =
        app.findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
    std::fprintf(stderr, "host: app-owned timers = %lld\n",
                 static_cast<long long>(owned.size()));

    MetaCallProbe probe;
    app.installEventFilter(&probe);

    // Stack objects with no parent, so they are never counted as app-owned.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::fprintf(stderr, "host: watchdog fired, the shim never quit us\n");
        std::exit(kExitNeverQuit);
    });
    watchdog.start(kWatchdogMs);

    const int rc = app.exec();
    if (rc != 0) {
        std::fprintf(stderr, "host: exec() returned %d\n", rc);
        return kExitBadExecCode;
    }
    if (owned.size() != 1) {
        std::fprintf(stderr, "host: quit was not delivered by an app-owned timer\n");
        return kExitNotAppOwned;
    }
    if (probe.sawMetaCall) {
        std::fprintf(stderr, "host: a queued call from another thread reached the app\n");
        return kExitCrossThread;
    }
    std::fprintf(stderr, "host: clean quit via app-owned timer\n");
    return kExitClean;
}
