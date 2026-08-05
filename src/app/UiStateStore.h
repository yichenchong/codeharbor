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
// One entry here used to be load-bearing for correctness and is now cosmetic:
// the pane-id counter keeps layout slot labels stable and non-repeating. It no
// longer names a remote tmux session — the server does (SPEC 5.2) — so it can
// no longer hand two panes the same shell. See setNextPaneSuffix().
//
// Storage keys:
//   layout/sidebarWidth, layout/viewerWidth, layout/terminalWidth
//   sidebar/pinnedOnly
//   sidebar/showArchived
//   selectedPane/<devSessionId>
//   paneSuffix/<devSessionId>/<region>
//   session/<serverId>/active
//
// Only `region` is checked for a '/' (see setNextPaneSuffix()), and the
// asymmetry is deliberate rather than an oversight. `region` is a two-word
// vocabulary this client owns, so a separator in it can only be QML misuse and
// would silently address a key shape that is not the documented one. A Dev
// Session or server id is an OPAQUE server-minted value that this class only
// ever uses as a key fragment: every read builds the key the same way the
// matching write did, so a separator inside one merely nests the key one level
// deeper and still round-trips, and none of the shapes above can collide with
// another (`selectedPane/a/b` is a different key from `selectedPane/a`, and the
// only spelling that could reach `paneSuffix/a/b/c` two ways has the '/' in the
// region half, which is refused). Rejecting ids here would instead throw away
// state for sessions the server is perfectly happy with.
class UiStateStore : public QObject {
    Q_OBJECT

public:
    // Empty iniPath -> native per-user store (org "CodeHarbor", app
    // "CodeHarbor"). Non-empty iniPath -> an explicit .ini file in IniFormat,
    // used by tests for a deterministic, isolated store.
    explicit UiStateStore(const QString& iniPath = QString(),
                          QObject* parent = nullptr);
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
    Q_INVOKABLE void setSelectedPane(const QString& devSessionId,
                                     const QString& paneId);
    Q_INVOKABLE QString selectedPane(const QString& devSessionId) const;

    // The next "<region>-<n>" suffix to mint for one (Dev Session, region)
    // pair. Persisted so a pane id is not handed out twice within a Dev
    // Session — not after the pane holding it was closed, and not after a
    // restart either.
    //
    // This is about LABELS and layout bookkeeping, and NOTHING else. It is not
    // a safety mechanism and must not be read as one. A terminal pane's layout
    // pane id used to BE the name of its remote tmux session (the client built
    // "ch_<devSessionId>_<paneId>" and attached it with `tmux new-session -A`),
    // so a reused number silently re-attached a closed pane's old shell — and
    // this counter, being client-local, could never stop a SECOND machine from
    // re-minting the same number. Identity moved out of the label entirely: a
    // terminal is the `terminal_panes` row whose id its LAYOUT LEAF carries
    // (SplitNode::terminalPaneId), server-minted, never recycled and shared
    // through the stored tree, so every client agrees. A recycled number can no
    // longer address anybody's shell even in principle. The counter stays
    // because the numbers are what the user reads on a pane header and what
    // viewer pane ids are keyed by, and a Dev Session that shows two panes both
    // labelled "terminal-1" is its own kind of wrong.
    //
    // Per region as well as per Dev Session: the two regions number
    // independently ("viewer-1" and "terminal-1" coexist), and two Dev Sessions
    // must not consume each other's numbers.
    //
    // A key that is absent, or holds anything that is not a whole number of at
    // least 1, reads back as 1 - a fresh region starts at "<region>-1". A WRITE
    // below 1 is dropped for the same reason, so the two halves cannot disagree
    // about what is stored. Same reasoning as the widths above: one hand-edited
    // or half-written line must fall back to the documented start, never to 0
    // (which would mint a "<region>-0") and never to a number below one already
    // in the tree.
    // SessionLayouts additionally takes the maximum of this counter and the
    // highest suffix its loaded tree carries, so a Dev Session that predates
    // this counter - or one whose settings file was cleared - still cannot
    // label a new pane with a number a pane already on screen is wearing.
    //
    // An empty devSessionId or region, or a region containing '/', is not a
    // valid address. Reads answer the default and writes are dropped, so
    // nothing is ever parked under a bare "paneSuffix//<region>" key or a
    // nested region path.
    Q_INVOKABLE void setNextPaneSuffix(const QString& devSessionId,
                                       const QString& region, int suffix);
    Q_INVOKABLE int nextPaneSuffix(const QString& devSessionId,
                                   const QString& region) const;

    // The sessions-sidebar pin filter is presentation state, not a workspace
    // mutation: a second client may see the same pinned sessions but chooses
    // independently whether to hide the others. It persists so a restart does
    // not unexpectedly switch the user's view back to every session.
    //
    // Missing or unreadable values default to false. Showing all sessions is
    // the safe fallback: a malformed settings file must not make the workspace
    // appear empty by silently enabling a filter.
    Q_INVOKABLE void setPinnedOnly(bool pinnedOnly);
    Q_INVOKABLE bool pinnedOnly() const;

    // Whether archived sessions are shown is also client-local presentation
    // state. It is independent of which sessions are archived on the server,
    // and defaults to false so a normal sidebar does not fill with old work.
    Q_INVOKABLE void setShowArchived(bool showArchived);
    Q_INVOKABLE bool showArchived() const;

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
    Q_INVOKABLE void setActiveSession(const QString& serverId,
                                      const QString& devSessionId);
    Q_INVOKABLE QString activeSession(const QString& serverId) const;

private:
    std::unique_ptr<QSettings> m_settings;
};

} // namespace ch
