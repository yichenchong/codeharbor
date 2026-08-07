#include "WindowChromeNative.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QQuickItem>
#include <QWindow>

#ifdef Q_OS_WIN
#  include <QEvent>
#  include <QPlatformSurfaceEvent>
#  include <QtMath>
#  include <windowsx.h>
#endif

namespace ch {

WindowChromeNative::WindowChromeNative(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    if (auto *application = QCoreApplication::instance())
        application->installNativeEventFilter(this);
#endif
}

WindowChromeNative::~WindowChromeNative()
{
#ifdef Q_OS_WIN
    clear();
    if (auto *application = QCoreApplication::instance())
        application->removeNativeEventFilter(this);
#endif
}

void WindowChromeNative::registerMaximizeButton(QObject *window, QObject *button)
{
#ifdef Q_OS_WIN
    auto *nextWindow = qobject_cast<QWindow *>(window);
    auto *nextButton = qobject_cast<QQuickItem *>(button);
    if (!nextWindow || !nextButton) {
        clear();
        return;
    }

    if (m_window == nextWindow && m_button == nextButton) {
        installShellStyles(nextWindow);
        return;
    }

    clear();
    m_window = nextWindow;
    m_button = nextButton;
    // Watch the window itself, not just the message queue: the handle cached on
    // the next line stops being the window's handle if Windows recreates it,
    // and eventFilter() is where that is noticed.
    nextWindow->installEventFilter(this);
    m_hwnd = reinterpret_cast<HWND>(nextWindow->winId());
    installShellStyles(nextWindow);
    setNativeButtonState(false, false);
#else
    Q_UNUSED(window)
    Q_UNUSED(button)
#endif
}

void WindowChromeNative::clear()
{
#ifdef Q_OS_WIN
    setNativeButtonState(false, false);
    if (m_window)
        m_window->removeEventFilter(this);
    m_window = nullptr;
    m_button = nullptr;
    m_hwnd = nullptr;
    m_stylesInstalled = false;
    m_nativeDown = false;
#endif
}

#ifdef Q_OS_WIN

void WindowChromeNative::installShellStyles(QWindow *window)
{
    if (!window)
        return;

    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return;

    m_hwnd = hwnd;

    // Qt's Windows plugin intentionally starts a frameless top-level window as
    // WS_POPUP and leaves out the frame/maximise styles. That is exactly right
    // for drawing pixels without a native caption, but Windows consequently
    // excludes the window from Aero Snap and Snap Layouts. Restore only the
    // behavioural bits; leave WS_CAPTION absent so the QML chrome remains the
    // complete visible frame.
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    const LONG_PTR shellStyles = WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
    const LONG_PTR desired = (style | shellStyles) & ~WS_CAPTION;
    if (desired != style) {
        SetWindowLongPtr(hwnd, GWL_STYLE, desired);
        // FRAMECHANGED makes Windows recalculate the non-client contract. Qt's
        // frameless WM_NCCALCSIZE path still gives the scene the full client
        // rectangle, so adding WS_THICKFRAME does not move the QML content.
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                         | SWP_NOACTIVATE);
    }
    m_stylesInstalled = true;
}

// Keep the cached HWND honest across a native handle being recreated.
//
// A QWindow and its Win32 handle are not the same lifetime. Windows destroys
// and recreates the handle behind a living QWindow in ordinary circumstances —
// changing a window flag is enough — and the replacement is a different HWND.
// Everything in this class is keyed on the cached one: nativeEventFilter drops
// every message whose hwnd does not match, so a stale cache silently switches
// the maximise hit-test and the Windows 11 Snap Layouts flyout off for the rest
// of the session, and maximizeButtonRect() would ask Windows about a handle
// that no longer exists. The QPointer above covers the QWindow dying; it says
// nothing about the handle underneath it.
//
// The surface event is the honest place to notice, and the only cheap one.
// Re-reading winId() from inside the message filter is not an option: winId()
// CREATES the platform window when there is none, which is the last thing to do
// from inside a message loop. QEvent::PlatformSurface is delivered around
// exactly the two moments that matter, and at each of them the answer is
// already settled.
//
// This never consumes the event: it only observes, and Qt's own handling of a
// surface being created or destroyed must still run.
bool WindowChromeNative::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && event->type() == QEvent::PlatformSurface) {
        switch (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType()) {
        case QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed:
            // Let go of the handle while it is still valid rather than after.
            // Dropping m_stylesInstalled with it matters too: the hit-test
            // answers HTCLIENT on the strength of that flag, and a window with
            // no surface has no styles installed on anything.
            setNativeButtonState(false, false);
            m_hwnd = nullptr;
            m_stylesInstalled = false;
            m_nativeDown = false;
            break;
        case QPlatformSurfaceEvent::SurfaceCreated:
            // The surface exists by now, so winId() inside installShellStyles()
            // reads the new handle instead of creating one. The replacement
            // window does not carry the shell styles either, so re-caching the
            // handle and re-applying them is one call.
            installShellStyles(m_window);
            setNativeButtonState(false, false);
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

QRect WindowChromeNative::maximizeButtonRect() const
{
    if (!m_window || !m_button || !m_button->isVisible() || !m_hwnd)
        return {};

    POINT clientOrigin{0, 0};
    if (!ClientToScreen(m_hwnd, &clientOrigin))
        return {};

    const QPointF sceneTopLeft = m_button->mapToScene(QPointF(0, 0));
    const QPointF sceneBottomRight = m_button->mapToScene(
        QPointF(m_button->width(), m_button->height()));
    const qreal dpr = qMax<qreal>(m_window->devicePixelRatio(), 1.0);
    const QPoint topLeft(clientOrigin.x + qRound(sceneTopLeft.x() * dpr),
                         clientOrigin.y + qRound(sceneTopLeft.y() * dpr));
    const QSize size(qMax(0, qRound((sceneBottomRight.x() - sceneTopLeft.x()) * dpr)),
                     qMax(0, qRound((sceneBottomRight.y() - sceneTopLeft.y()) * dpr)));
    return QRect(topLeft, size);
}

void WindowChromeNative::setNativeButtonState(bool hovered, bool down)
{
    if (!m_button)
        return;

    // The native hit-test takes the pointer outside QML's normal mouse event
    // path. These explicit states preserve the same hover/pressed appearance
    // and leave the real AbstractButton signal as the one action route.
    m_button->setProperty("nativeHovered", hovered);
    m_button->setProperty("nativeDown", down);
}

bool WindowChromeNative::nativeEventFilter(const QByteArray &eventType, void *message,
                                           qintptr *result)
{
    if ((eventType != QByteArrayLiteral("windows_generic_MSG")
         && eventType != QByteArrayLiteral("windows_dispatcher_MSG"))
        || !message || !m_window || !m_button || !m_hwnd) {
        return false;
    }

    auto *msg = static_cast<MSG *>(message);
    if (msg->hwnd != m_hwnd)
        return false;

    switch (msg->message) {
    case WM_NCHITTEST: {
        const POINT screenPoint{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        const bool inMaximizeButton = maximizeButtonRect().contains(
            QPoint(screenPoint.x, screenPoint.y));
        setNativeButtonState(inMaximizeButton, m_nativeDown);

        if (inMaximizeButton) {
            // HTMAXBUTTON is what makes the Windows 11 Snap Layouts flyout
            // appear when the pointer rests over a custom maximise button.
            if (result)
                *result = HTMAXBUTTON;
            return true;
        }

        // WS_THICKFRAME is present for shell snapping, but native edge hit
        // tests must not steal the QML resize strips. startSystemResize() uses
        // the system command directly, so returning HTCLIENT here preserves
        // exactly one resize owner: Main.qml's eight grips.
        if (m_stylesInstalled) {
            if (result)
                *result = HTCLIENT;
            return true;
        }
        return false;
    }
    case WM_NCMOUSEMOVE:
        setNativeButtonState(msg->wParam == HTMAXBUTTON, m_nativeDown);
        return false;
    case WM_MOUSEMOVE:
        if (!m_nativeDown)
            setNativeButtonState(false, false);
        return false;
    case WM_NCMOUSELEAVE:
        setNativeButtonState(false, m_nativeDown);
        return false;
    case WM_NCLBUTTONDOWN:
        if (msg->wParam != HTMAXBUTTON)
            return false;
        m_nativeDown = true;
        setNativeButtonState(true, true);
        // Returning true prevents DefWindowProc from maximising the HWND behind
        // QML's back. The QML button is invoked on release below, preserving its
        // existing toggle and Accessible action semantics.
        return true;
    case WM_NCLBUTTONUP: {
        if (!m_nativeDown)
            return false;
        const bool clicked = msg->wParam == HTMAXBUTTON;
        m_nativeDown = false;
        setNativeButtonState(false, false);
        if (clicked)
            QMetaObject::invokeMethod(m_button, "clicked", Qt::DirectConnection);
        return true;
    }
    case WM_NCLBUTTONDBLCLK:
        if (msg->wParam != HTMAXBUTTON)
            return false;
        m_nativeDown = false;
        setNativeButtonState(false, false);
        QMetaObject::invokeMethod(m_button, "clicked", Qt::DirectConnection);
        return true;
    default:
        return false;
    }
}

#endif // Q_OS_WIN

} // namespace ch
