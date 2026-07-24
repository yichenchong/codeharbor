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
    };

    explicit SessionsModel(QObject *parent = nullptr);

    // Replace the entire sidebar contents.
    void setGroups(QVector<GroupRow> groups);

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
