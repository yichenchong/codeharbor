#pragma once

#include <QObject>

#ifdef Q_OS_WIN
#  include <QAbstractNativeEventFilter>
#  include <QByteArray>
#  include <QPointer>
#  include <QRect>
#  include <windows.h>
#endif

class QQuickItem;
class QWindow;

namespace ch {

// Restores the small part of the Win32 window contract that Qt deliberately
// omits for Qt::FramelessWindowHint. The custom QML chrome stays visually
// frameless, while the shell still recognises the window as movable, resizable,
// and maximisable for Snap and the other native window gestures.
class WindowChromeNative final : public QObject
#ifdef Q_OS_WIN
    , public QAbstractNativeEventFilter
#endif
{
    Q_OBJECT

public:
    explicit WindowChromeNative(QObject *parent = nullptr);
    ~WindowChromeNative() override;

    // QML supplies the real QWindow and the QML maximise button. The helper is
    // intentionally a no-op on non-Windows platforms: startSystemMove() and
    // the compositor already own those behaviours there.
    Q_INVOKABLE void registerMaximizeButton(QObject *window, QObject *button);
    Q_INVOKABLE void clear();

#ifdef Q_OS_WIN
    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override;

private:
    void installShellStyles(QWindow *window);
    QRect maximizeButtonRect() const;
    void setNativeButtonState(bool hovered, bool down);

    QPointer<QWindow> m_window;
    QPointer<QQuickItem> m_button;
    HWND m_hwnd = nullptr;
    bool m_stylesInstalled = false;
    bool m_nativeDown = false;
#endif
};

} // namespace ch
