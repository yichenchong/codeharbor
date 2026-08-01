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

public:
    enum Roles {
        NameRole = Qt::UserRole + 1, // session or group display name
        SubtitleRole,                // optional repository/branch subtitle
        RowStateRole,                // aggregate SessionRowState (int), sessions only
        IsGroupRole,                 // true for group rows, false for session rows
        CollapsedRole,               // group collapsed flag, groups only
        IdRole,                      // ch id string of the row (group or session)
        GroupIdRole,                 // containing group's id (own id for groups)
    };

    explicit SessionsModel(QObject *parent = nullptr);

    // Replace the entire sidebar contents.
    void setGroups(QVector<GroupRow> groups);

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
    // An empty set (session with no terminals) yields Disconnected.
    static SessionRowState aggregateSessionState(const QVector<TerminalStatus> &terminals);

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    QVector<GroupRow> groups_;
};

} // namespace ch
