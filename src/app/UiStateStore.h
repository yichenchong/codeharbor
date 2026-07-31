#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QSettings;

namespace ch {

// Client-local UI state (SPEC 4.1): region widths, the per-Dev-Session selected
// pane and the per-(Dev Session, region) pane-id counter, persisted via QSettings
// so they survive restarts. This is purely local presentation state and is never
// sent to the codeharbord host.
//
// One entry here is load-bearing rather than cosmetic: the pane-id counter is
// what stops a pane id - and therefore a remote tmux session name - from being
// handed out twice. See setNextPaneSuffix().
//
// Storage keys:
//   layout/sidebarWidth, layout/viewerWidth, layout/terminalWidth
//   selectedPane/<devSessionId>
//   session/<serverId>/active
//   paneSuffix/<devSessionId>/<region>
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

    // A width that was never stored, or whose stored text is not a whole number
    // (a hand-edited file, a line half-written when the machine went down),
    // reads back as the documented default rather than as 0 — a 0 here is a
    // region with no width at all, which is not a state the user can drag their
    // way out of. The viewer is the exception: 0 is its documented default and
    // means "fill whatever the sidebar and terminal leave".
    Q_INVOKABLE int sidebarWidth() const;  // default 260, never below 1
    Q_INVOKABLE int viewerWidth() const;   // default 0 (fill remaining)
    Q_INVOKABLE int terminalWidth() const; // default 520, never below 1

    // The pane the user was last working in within a Dev Session, so a split
    // command acts on it rather than on whatever the region lists first.
    //
    // An empty devSessionId is not a Dev Session, and is handled exactly as
    // setActiveSession() handles an empty serverId: reads return empty and
    // writes are dropped, so no value is ever parked under a bare
    // "selectedPane/" key that the next empty-id read would pick up.
    Q_INVOKABLE void setSelectedPane(QString devSessionId, QString paneId);
    Q_INVOKABLE QString selectedPane(QString devSessionId) const;

    // The next "<region>-<n>" suffix to mint for one (Dev Session, region)
    // pair. Persisted because a pane id must never be handed out twice within a
    // Dev Session - not after the pane holding it was closed, and not after a
    // restart either. A terminal pane's layout pane id IS the name of its remote
    // tmux session (src/terminal/TerminalController.cpp builds
    // "ch_<devSessionId>_<paneId>", attached with `tmux new-session -A`), so
    // handing the same id out twice silently re-attaches the closed pane's old
    // shell - its scrollback, its working directory and whatever is still
    // running in it - instead of starting the new shell the user asked for.
    //
    // Per region as well as per Dev Session: the two regions number
    // independently ("viewer-1" and "terminal-1" coexist), and two Dev Sessions
    // must not consume each other's numbers.
    //
    // A key that is absent, or holds anything that is not a whole number of at
    // least 1, reads back as 1 - a fresh region starts at "<region>-1". Same
    // reasoning as the widths above: one hand-edited or half-written line must
    // fall back to the documented start, never to 0 (which would mint a
    // "<region>-0") and never to a number below one already in the tree.
    // SessionLayouts additionally takes the maximum of this counter and the
    // highest suffix its loaded tree carries, so a Dev Session that predates
    // this counter - or one whose settings file was cleared - still cannot mint
    // an id that collides with a pane it already shows.
    //
    // An empty devSessionId is not a Dev Session and is handled exactly as
    // setSelectedPane()/setActiveSession() handle theirs: reads answer the
    // default and writes are dropped, so nothing is ever parked under a bare
    // "paneSuffix//<region>" key.
    Q_INVOKABLE void setNextPaneSuffix(QString devSessionId, QString region,
                                       int suffix);
    Q_INVOKABLE int nextPaneSuffix(QString devSessionId, QString region) const;

    // The Dev Session the user was last working in ON A GIVEN SERVER, so a
    // relaunch reopens it instead of an empty shell. Client-local: which
    // session is "current" is a per-client presentation choice, not
    // authoritative workspace state.
    //
    // Scoped by serverId because a Dev Session belongs to exactly one server: a
    // single global key meant that connecting to server B and relaunching tried
    // to reopen server A's session id, which does not exist on B — a phantom
    // restore (dead layout fetches, a terminal pane bound to nothing) on every
    // launch after a server switch.
    //
    // An empty serverId is not a server: reads return empty and writes are
    // dropped, so nothing is ever stored under a placeholder key during the
    // window before server.info has answered.
    Q_INVOKABLE void setActiveSession(QString serverId, QString devSessionId);
    Q_INVOKABLE QString activeSession(QString serverId) const;

private:
    std::unique_ptr<QSettings> m_settings;
};

} // namespace ch
