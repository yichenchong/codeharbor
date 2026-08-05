#pragma once

#include "SessionState.h"
#include "WorkspaceTypes.h"

#include <QAbstractItemModel>
#include <QByteArray>
#include <QHash>
#include <QVector>

// Sidebar model (SPEC 4.2). A headless two-level tree exposing groups and their
// Dev Sessions, with an aggregate per-session row-state derived from each
// session's terminal states via ch::aggregateRowState.
namespace ch {

// Runtime connection/attention state of a single terminal, used only to compute
// a session's aggregate sidebar state. Not persisted (see TerminalPane).
struct TerminalStatus {
    TerminalId id;
    TerminalState connection = TerminalState::Unloaded;
    AgentState agent = AgentState::Unknown;
};

// A Dev Session as shown in the sidebar: its definition, an optional
// repository/branch subtitle, and the live state of its terminals.
struct SessionRow {
    DevSession session;
    QString subtitle;
    QVector<TerminalStatus> terminals;
};

// A sidebar group together with its ordered sessions.
struct GroupRow {
    Group group;
    QVector<SessionRow> sessions;
};

class SessionsModel : public QAbstractItemModel {
    Q_OBJECT
    // Both sidebar filters are set from QML (SessionsSidebar.qml assigns
    // `app.sessionsModel.pinnedOnly` / `.showArchived`), so BOTH must be
    // Q_PROPERTYs. A plain C++ setter is not reachable from a QML property
    // assignment: the write does not land, and depending on the object's QML
    // context it either throws and aborts the rest of the handler or is
    // silently dropped. pinnedOnly used to have only the setter, which left
    // the pin filter dead in the shipping sidebar.
    Q_PROPERTY(bool pinnedOnly READ pinnedOnly WRITE setPinnedOnly NOTIFY pinnedOnlyChanged)
    Q_PROPERTY(bool showArchived READ showArchived WRITE setShowArchived NOTIFY showArchivedChanged)
    Q_PROPERTY(bool hasSessions READ hasSessions NOTIFY sessionPresenceChanged)
    Q_PROPERTY(bool hasUnarchivedSessions READ hasUnarchivedSessions NOTIFY sessionPresenceChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1, // session or group display name
        SubtitleRole,                // optional repository/branch subtitle
        RowStateRole,                // aggregate SessionRowState (int), sessions only
        IsGroupRole,                 // true for group rows, false for session rows
        CollapsedRole,               // group collapsed flag, groups only
        IdRole,                      // ch id string of the row (group or session)
        GroupIdRole,                 // containing group's id (own id for groups)
        PinnedRole,                  // workspace-owned session pin state
        ArchivedRole,                // workspace-owned archived state
    };

    explicit SessionsModel(QObject *parent = nullptr);

    // Replace the entire sidebar contents.
    void setGroups(QVector<GroupRow> groups);

    // Whether the model exposes only pinned sessions and groups containing one.
    // This is a client-local presentation choice; the source rows remain intact
    // so switching the filter does not require another server read.
    bool pinnedOnly() const { return pinnedOnly_; }
    void setPinnedOnly(bool pinnedOnly);

    // Whether archived sessions are included in the visible tree. This is a
    // client-local presentation choice; the source rows remain intact.
    bool showArchived() const { return showArchived_; }
    void setShowArchived(bool showArchived);

    // Presence in the authoritative source tree, independent of the active
    // filters. These let the sidebar explain "all sessions are archived"
    // instead of presenting a filtered-empty workspace as a new one.
    bool hasSessions() const;
    bool hasUnarchivedSessions() const;

    // Incrementally refresh live per-terminal agent/connection state WITHOUT a
    // full model reset. `groups` must mirror the current structure (same group
    // and session ids in the same order); only each session's `terminals` are
    // adopted. For every session whose aggregate row-state actually changes, a
    // targeted dataChanged() for RowStateRole is emitted for just that row, so
    // the sidebar's delegates (and the id-tracked selection) survive untouched.
    // Every row is adopted BEFORE the first signal goes out, so a slot reading
    // any other row already sees its new value.
    // If the structure has drifted, this falls back to a full setGroups().
    void updateTerminalStates(QVector<GroupRow> groups);

    // Highest-priority sidebar state for a set of terminals (SPEC 4.2 precedence:
    // Error > WaitingForInput > Running > FinishedUnseen > Idle > Disconnected).
    // An empty set (a session with no terminals) yields Idle, NOT Disconnected:
    // ch::aggregateRowState reserves Disconnected for a pane that was live and
    // then reported the connection lost, and a session with nothing open has no
    // such loss to report (see SessionState.h).
    static SessionRowState aggregateSessionState(const QVector<TerminalStatus> &terminals);

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void pinnedOnlyChanged();
    void showArchivedChanged();
    void sessionPresenceChanged();

private:
    // Rebuild the filtered view from the authoritative tree under the current
    // filter values, wrapped in a model reset. Used by the two filter setters:
    // neither touches the source rows, so re-running setGroups() there would
    // deep-copy the whole tree only to move it straight back.
    void applyFilters();

    // Keep the authoritative refresh result separate from the filtered view:
    // toggling either client-local filter must not discard rows that should
    // reappear when that filter is turned off.
    QVector<GroupRow> allGroups_;
    QVector<GroupRow> groups_;
    bool pinnedOnly_ = false;
    bool showArchived_ = false;
};

} // namespace ch
