#include "WindowChromeNative.h"

#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest/QtTest>

class NativeButton : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool nativeHovered READ nativeHovered WRITE setNativeHovered NOTIFY nativeHoveredChanged)
    Q_PROPERTY(bool nativeDown READ nativeDown WRITE setNativeDown NOTIFY nativeDownChanged)

public:
    bool nativeHovered() const { return m_hovered; }
    bool nativeDown() const { return m_down; }
    void setNativeHovered(bool hovered)
    {
        if (m_hovered == hovered)
            return;
        m_hovered = hovered;
        emit nativeHoveredChanged();
    }
    void setNativeDown(bool down)
    {
        if (m_down == down)
            return;
        m_down = down;
        emit nativeDownChanged();
    }

signals:
    void nativeHoveredChanged();
    void nativeDownChanged();
    void clicked();

private:
    bool m_hovered = false;
    bool m_down = false;
};

class TstWindowChrome : public QObject {
    Q_OBJECT

private slots:
    void registrationObserverMayDestroyButton();
    void clearObserverMayDestroyButtonOrHelper();
    void releaseObserverMayDestroyHelper();
};

void TstWindowChrome::registrationObserverMayDestroyButton()
{
#ifdef Q_OS_WIN
    QQuickWindow window;
    ch::WindowChromeNative chrome;
    QPointer<NativeButton> button = new NativeButton;
    button->setNativeHovered(true);
    connect(button, &NativeButton::nativeHoveredChanged, this,
            [&] { delete button.data(); });

    chrome.registerMaximizeButton(&window, button);

    QVERIFY(button.isNull());
    chrome.clear();
#else
    QSKIP("The native chrome bridge is Windows-only");
#endif
}

void TstWindowChrome::clearObserverMayDestroyButtonOrHelper()
{
#ifdef Q_OS_WIN
    for (bool destroyHelper : {false, true}) {
        QQuickWindow window;
        QPointer<ch::WindowChromeNative> chrome = new ch::WindowChromeNative;
        QPointer<NativeButton> button = new NativeButton;
        chrome->registerMaximizeButton(&window, button);
        button->setNativeHovered(true);
        button->setNativeDown(true);
        connect(button, &NativeButton::nativeHoveredChanged, this, [&] {
            if (destroyHelper)
                delete chrome.data();
            else
                delete button.data();
        });

        chrome->clear();

        if (destroyHelper) {
            QVERIFY(chrome.isNull());
            QVERIFY(button);
            QVERIFY(!button->nativeHovered());
            QVERIFY(!button->nativeDown());
        } else {
            QVERIFY(button.isNull());
            QVERIFY(chrome);
        }
        delete chrome.data();
        delete button.data();
    }
#else
    QSKIP("The native chrome bridge is Windows-only");
#endif
}

void TstWindowChrome::releaseObserverMayDestroyHelper()
{
#ifdef Q_OS_WIN
    QQuickWindow window;
    QPointer<ch::WindowChromeNative> chrome = new ch::WindowChromeNative;
    NativeButton button;
    chrome->registerMaximizeButton(&window, &button);
    MSG message{};
    message.hwnd = reinterpret_cast<HWND>(window.winId());
    message.message = WM_NCLBUTTONDOWN;
    message.wParam = HTMAXBUTTON;
    qintptr result = 0;
    QVERIFY(chrome->nativeEventFilter(QByteArrayLiteral("windows_generic_MSG"),
                                      &message, &result));
    QVERIFY(button.nativeDown());
    QSignalSpy clicked(&button, &NativeButton::clicked);
    connect(&button, &NativeButton::nativeHoveredChanged, this,
            [&] { delete chrome.data(); });

    message.message = WM_NCLBUTTONUP;
    const bool handled = chrome->nativeEventFilter(
        QByteArrayLiteral("windows_generic_MSG"), &message, &result);

    QVERIFY(handled);
    QVERIFY(chrome.isNull());
    QVERIFY(!button.nativeHovered());
    QVERIFY(!button.nativeDown());
    QCOMPARE(clicked.count(), 0);
#else
    QSKIP("The native chrome bridge is Windows-only");
#endif
}

QTEST_MAIN(TstWindowChrome)
#include "tst_windowchrome.moc"
