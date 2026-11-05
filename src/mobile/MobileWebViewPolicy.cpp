#include "MobileWebViewPolicy.h"

#include <QQuickWindow>
#include <QUuid>

namespace ch {

namespace {

constexpr int kInstallTimeoutMs = 10000;
constexpr int kInstallPollMs = 25;
constexpr int kActivePollMs = 250;

#if !CH_HAVE_QTWEBVIEW || (!defined(Q_OS_ANDROID) && !defined(Q_OS_DARWIN))
class UnavailableWebViewPolicy final : public MobileWebViewPolicyAdapter {
public:
    MobileWebViewPolicyResult poll() override
    {
        return {MobileWebViewPolicyState::Failed,
                QStringLiteral("This platform has no enforceable native web navigation policy. "
                               "The address was not loaded.")};
    }
};
#endif

} // namespace

#if !CH_HAVE_QTWEBVIEW || (!defined(Q_OS_ANDROID) && !defined(Q_OS_DARWIN))
std::unique_ptr<MobileWebViewPolicyAdapter> createMobileWebViewPolicyAdapter(
    QWindow *, const QUrl &)
{
    return std::make_unique<UnavailableWebViewPolicy>();
}
#endif

MobileWebViewPolicy::MobileWebViewPolicy(QObject *parent)
    : QObject(parent)
    , m_marker(QUrl(QStringLiteral("about:blank#ch-policy-")
                    + QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    m_timer.setInterval(kInstallPollMs);
    connect(&m_timer, &QTimer::timeout, this, &MobileWebViewPolicy::poll);
}

MobileWebViewPolicy::~MobileWebViewPolicy()
{
    m_timer.stop();
    m_adapter.reset();
}

void MobileWebViewPolicy::attach(QQuickItem *view)
{
    if (m_attached) {
        if (m_view != view)
            fail(QStringLiteral("The native web view changed. Close this page and open it again."));
        return;
    }
    m_attached = true;
    m_view = view;
    if (!m_view) {
        fail(QStringLiteral("The native web view is unavailable. The address was not loaded."));
        return;
    }
    connect(view, &QObject::destroyed, this, [this] {
        fail(QStringLiteral("The native web view was closed."));
    });
    connect(view, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        if (m_adapter && window != m_window.data()) {
            fail(QStringLiteral("The native web view moved to another window. "
                                "Close this page and open it again."));
            return;
        }
        if (m_installing)
            poll();
    });
    m_installing = true;
    m_elapsed.start();
    emit statusChanged();
    m_timer.start();
    poll();
}

void MobileWebViewPolicy::fail(const QString &message)
{
    m_timer.stop();
    m_adapter.reset();
    m_ready = false;
    m_installing = false;
    m_error = message;
    emit statusChanged();
}

void MobileWebViewPolicy::poll()
{
    if (!m_installing && !m_ready)
        return;
    if (!m_view) {
        fail(QStringLiteral("The native web view is unavailable. The address was not loaded."));
        return;
    }
    if (m_installing && m_elapsed.elapsed() >= kInstallTimeoutMs) {
        fail(QStringLiteral("The native web navigation guard could not be installed in time. "
                            "The address was not loaded; close this page and try again."));
        return;
    }
    if (!m_adapter) {
        QQuickWindow *window = m_view->window();
        if (!window)
            return;
        m_window = window;
        connect(window, &QObject::destroyed, this, [this] {
            fail(QStringLiteral("The web view's window was closed."));
        });
        m_adapter = createMobileWebViewPolicyAdapter(window, m_marker);
        if (!m_adapter) {
            fail(QStringLiteral("The native web navigation guard is unavailable. "
                                "The address was not loaded."));
            return;
        }
    }
    const MobileWebViewPolicyResult result = m_adapter->poll();
    switch (result.state) {
    case MobileWebViewPolicyState::Pending:
        if (m_ready)
            fail(QStringLiteral("The native web navigation guard is no longer active."));
        return;
    case MobileWebViewPolicyState::Failed:
        fail(result.error.isEmpty()
                 ? QStringLiteral("The native web navigation guard could not be installed. "
                                  "The address was not loaded.")
                 : result.error);
        return;
    case MobileWebViewPolicyState::Ready:
        if (m_ready)
            return;
        m_timer.setInterval(kActivePollMs);
        m_installing = false;
        m_ready = true;
        emit statusChanged();
        return;
    }
}

} // namespace ch
