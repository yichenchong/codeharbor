#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

namespace ch {

// Real OS desktop notifications for agent attention (SPEC 6.2: a terminal
// transitioning into waiting_input or idle_unseen). AgentStatusMonitor emits a
// notify(title, body) hook; this class is the display layer that turns it into
// a bubble the user actually sees.
//
// Backend: on Linux the freedesktop.org spec's session-bus service
// org.freedesktop.Notifications is called directly over QtDBus. The dependency
// is optional at compile time behind CH_HAVE_DBUS (mirroring CH_HAVE_LIBSSH in
// src/ssh): built without QtDBus, or run without a session bus, or run on a bus
// with no notification daemon (exactly the headless CI case), available() is
// false and notify() is a silent no-op — it must never warn, block or fail.
//
// Coalescing policy (anti-spam). Only the most recent notification is tracked:
//   * notify() with a (title, body) different from the tracked pair, or with
//     the same pair but more than coalesceWindowMs (default 5000) after the
//     last one was raised, raises a NEW bubble and emits notificationRaised().
//   * notify() with the SAME (title, body) inside the window is coalesced and
//     emits notificationCoalesced() instead. If a backend is available and it
//     returned an id for the tracked bubble, the Notify call is re-issued with
//     that id as replaces_id, so the desktop updates/refreshes the existing
//     bubble in place rather than stacking a duplicate. Without an id (no
//     backend) the repeat is simply dropped.
// Every raise restarts the window, so a burst of identical events yields one
// bubble that stays fresh, and a distinct event always gets through.
class Notifier : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)

public:
    explicit Notifier(QObject* parent = nullptr);
    ~Notifier() override;

    // True iff a notification backend was found at construction: compiled with
    // QtDBus, a session bus is connected, and org.freedesktop.Notifications is
    // registered on it. Constant for the lifetime of the object.
    bool available() const { return m_available; }

    // Gate for the user-facing "notifications off" switch. Disabled notify()
    // returns immediately without raising, coalescing or touching state.
    Q_INVOKABLE void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    // Coalescing window in milliseconds; <= 0 disables coalescing entirely.
    Q_INVOKABLE void setCoalesceWindowMs(int ms);
    int coalesceWindowMs() const { return m_coalesceWindowMs; }

    // Raise a desktop notification. Safe to call with no backend.
    Q_INVOKABLE void notify(QString title, QString body);

signals:
    void enabledChanged(bool enabled);
    // A new bubble was raised (passed the enabled and coalescing gates).
    void notificationRaised(const QString& title, const QString& body);
    // An identical notification arrived inside the coalescing window and was
    // suppressed (or replaced the existing bubble in place).
    void notificationCoalesced(const QString& title, const QString& body);

private:
    // Hands (title, body) to the backend. `replacesId` is 0 for a new bubble or
    // a previously returned id to update in place. Returns the id the backend
    // assigned, or 0 when there is no backend / the call failed.
    //
    // The backend answers asynchronously, so a NEW bubble's id is folded into
    // m_lastId when the reply lands. That reply is only adopted while the raise
    // it belongs to is still the tracked one — see m_raiseSerial.
    unsigned int deliver(const QString& title, const QString& body,
                         unsigned int replacesId);

    bool m_available = false;
    bool m_enabled = true;
    int m_coalesceWindowMs = 5000;

    QString m_lastTitle;
    QString m_lastBody;
    bool m_haveLast = false;
    unsigned int m_lastId = 0;
    // Incremented once per RAISED bubble. Two distinct notifications in quick
    // succession each start an asynchronous Notify, and the replies may land in
    // either order: without this stamp the FIRST bubble's id could be adopted
    // as m_lastId after the SECOND has become the tracked pair, so a repeat of
    // the second notification would rewrite the first bubble with the second's
    // text. A reply whose serial is no longer current is discarded instead.
    quint64 m_raiseSerial = 0;
    QElapsedTimer m_sinceLast;
};

} // namespace ch
