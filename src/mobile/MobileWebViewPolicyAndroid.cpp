#include "MobileWebViewPolicyAdapter.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QWindow>

namespace ch {
namespace {

class AndroidWebViewPolicyAdapter final : public MobileWebViewPolicyAdapter {
public:
    AndroidWebViewPolicyAdapter(QWindow *window, const QUrl &marker)
    {
        if (!window) {
            m_error = message("The Android window is unavailable for web-view protection.");
            return;
        }
        const QJniObject context = QNativeInterface::QAndroidApplication::context();
        if (!context.isValid()) {
            m_error = message("The Android activity is unavailable for web-view protection.");
            return;
        }
        const QJniObject markerString = QJniObject::fromString(marker.toString(QUrl::FullyEncoded));
        m_session = QJniObject("dev/codeharbor/mobile/GuardedWebView",
                               "(Landroid/content/Context;Ljava/lang/String;)V",
                               context.object<jobject>(), markerString.object<jstring>());
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions() || !m_session.isValid()) {
            m_error = message("The Android native web-view policy helper could not be created.");
        }
    }

    ~AndroidWebViewPolicyAdapter() override
    {
        // Java cancellation is immediate; any later Android UI job owns only its
        // Java session, never this C++ adapter or the destroyed QML owner.
        if (m_session.isValid()) {
            m_session.callMethod<void>("close", "()V");
            QJniEnvironment environment;
            environment.checkAndClearExceptions();
        }
    }

    MobileWebViewPolicyResult poll() override
    {
        if (!m_error.isEmpty())
            return { MobileWebViewPolicyState::Failed, m_error };
        if (!m_session.isValid()) {
            return { MobileWebViewPolicyState::Failed,
                     message("The Android native web-view policy session is unavailable.") };
        }
        m_session.callMethod<void>("requestCheck", "()V");
        const jint state = m_session.callMethod<jint>("getState", "()I");
        QJniEnvironment environment;
        if (environment.checkAndClearExceptions()) {
            m_error = message("Android could not report the native web-view protection state.");
            m_session.callMethod<void>("close", "()V");
            environment.checkAndClearExceptions();
            return { MobileWebViewPolicyState::Failed, m_error };
        }
        switch (state) {
        case 0:
            return { MobileWebViewPolicyState::Pending, {} };
        case 1:
            return { MobileWebViewPolicyState::Ready, {} };
        case 2: {
            const QJniObject error = m_session.callObjectMethod("getError", "()Ljava/lang/String;");
            m_error = error.toString();
            if (environment.checkAndClearExceptions() || m_error.isEmpty())
                m_error = message("Android could not install native web-view protection.");
            return { MobileWebViewPolicyState::Failed, m_error };
        }
        default:
            m_error = message("Android returned an invalid native web-view protection state.");
            m_session.callMethod<void>("close", "()V");
            environment.checkAndClearExceptions();
            return { MobileWebViewPolicyState::Failed, m_error };
        }
    }

private:
    static QString message(const char *source)
    {
        return QCoreApplication::translate("ch::MobileWebViewPolicy", source);
    }

    QJniObject m_session;
    QString m_error;
};

} // namespace

std::unique_ptr<MobileWebViewPolicyAdapter> createMobileWebViewPolicyAdapter(
    QWindow *window, const QUrl &marker)
{
    return std::make_unique<AndroidWebViewPolicyAdapter>(window, marker);
}

} // namespace ch
