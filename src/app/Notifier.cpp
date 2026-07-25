#include "Notifier.h"

#include <QVariantList>
#include <QVariantMap>

#if CH_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

namespace ch {

namespace {
const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
const QString kIface = QStringLiteral("org.freedesktop.Notifications");
const QString kAppName = QStringLiteral("CodeHarbor");
const QString kAppIcon = QStringLiteral("utilities-terminal");
// Let the daemon pick its own default expiry rather than pinning one; agent
// attention is informational, not modal.
constexpr int kExpireTimeoutMs = 8000;
} // namespace

Notifier::Notifier(QObject* parent)
    : QObject(parent)
{
#if CH_HAVE_DBUS
    // isConnected() is false when there is no session bus at all (headless CI,
    // no DBUS_SESSION_BUS_ADDRESS); isServiceRegistered() is false when a bus
    // exists but nothing implements the notification spec. Both are silent
    // queries: neither warns nor blocks on a missing daemon.
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        if (QDBusConnectionInterface* iface = bus.interface()) {
            m_available = iface->isServiceRegistered(kService).value();
        }
    }
#endif
    m_sinceLast.start();
}

Notifier::~Notifier() = default;

void Notifier::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    emit enabledChanged(m_enabled);
}

void Notifier::setCoalesceWindowMs(int ms)
{
    m_coalesceWindowMs = ms;
}

void Notifier::notify(QString title, QString body)
{
    if (!m_enabled)
        return;

    const bool sameAsLast = m_haveLast && title == m_lastTitle && body == m_lastBody;
    const bool inWindow = m_coalesceWindowMs > 0
        && m_sinceLast.isValid()
        && m_sinceLast.elapsed() < m_coalesceWindowMs;

    if (sameAsLast && inWindow) {
        // Repeat inside the window: refresh the existing bubble in place when
        // the backend gave us an id to replace, otherwise drop it entirely.
        if (m_lastId != 0)
            deliver(title, body, m_lastId);
        emit notificationCoalesced(title, body);
        return;
    }

    m_lastTitle = title;
    m_lastBody = body;
    m_haveLast = true;
    m_sinceLast.restart();
    m_lastId = deliver(title, body, 0);
    emit notificationRaised(title, body);
}

unsigned int Notifier::deliver(const QString& title, const QString& body,
                               unsigned int replacesId)
{
    if (!m_available)
        return 0;

#if CH_HAVE_DBUS
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kIface,
                                                      QStringLiteral("Notify"));
    msg << kAppName
        << replacesId
        << kAppIcon
        << title
        << body
        << QStringList()      // actions
        << QVariantMap()      // hints
        << kExpireTimeoutMs;

    // Blocking here would stall the UI thread on a wedged daemon, so the call
    // is asynchronous; the assigned id is folded back in for later replacement.
    QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(msg);
    auto* watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, replacesId] {
                const QDBusPendingReply<unsigned int> reply = *watcher;
                // A failed Notify is not the user's problem: stay silent and
                // leave m_lastId alone so the next repeat just starts fresh.
                if (!reply.isError() && replacesId == 0)
                    m_lastId = reply.value();
                watcher->deleteLater();
            });
    // The id is not known synchronously; 0 means "no id yet". A repeat that
    // arrives before the reply is simply dropped rather than replaced.
    return replacesId;
#else
    Q_UNUSED(title);
    Q_UNUSED(body);
    Q_UNUSED(replacesId);
    return 0;
#endif
}

} // namespace ch
