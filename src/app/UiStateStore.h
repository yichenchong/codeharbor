#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QSettings;

namespace ch {

// Client-local UI state (SPEC 4.1): region widths and the per-Dev-Session
// selected pane, persisted via QSettings so they survive restarts. This is
// purely local presentation state and is never sent to the codeharbord host.
//
// Storage keys:
//   layout/sidebarWidth, layout/viewerWidth, layout/terminalWidth
//   selectedPane/<devSessionId>
//   session/active
class UiStateStore : public QObject {
    Q_OBJECT

public:
    // Empty iniPath -> native per-user store (org "CodeHarbor", app
    // "CodeHarbor"). Non-empty iniPath -> an explicit .ini file in IniFormat,
    // used by tests for a deterministic, isolated store.
    explicit UiStateStore(QString iniPath = QString(), QObject* parent = nullptr);
    ~UiStateStore() override;

    // Persist all three region widths at once (a handle move adjusts two
    // adjacent regions; storing the whole set keeps them consistent).
    Q_INVOKABLE void setRegionWidths(int sidebar, int viewer, int terminal);

    Q_INVOKABLE int sidebarWidth() const;  // default 260
    Q_INVOKABLE int viewerWidth() const;   // default 0 (fill remaining)
    Q_INVOKABLE int terminalWidth() const; // default 520

    Q_INVOKABLE void setSelectedPane(QString devSessionId, QString paneId);
    Q_INVOKABLE QString selectedPane(QString devSessionId) const;

    // The Dev Session the user was last working in, so a relaunch reopens it
    // instead of an empty shell. Client-local: which session is "current" is a
    // per-client presentation choice, not authoritative workspace state.
    Q_INVOKABLE void setActiveSession(QString devSessionId);
    Q_INVOKABLE QString activeSession() const;

private:
    std::unique_ptr<QSettings> m_settings;
};

} // namespace ch
