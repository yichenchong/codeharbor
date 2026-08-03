#include "SessionsModel.h"

#include <QSet>
#include <algorithm>

#include <limits>
#include <utility>

namespace ch {

namespace {
// Both filters are independent predicates: showArchived broadens the set of
// sessions eligible for display, while pinnedOnly narrows it to pinned rows.
QVector<GroupRow> filteredGroups(const QVector<GroupRow> &source,
                                 bool pinnedOnly, bool showArchived)
{
    QVector<GroupRow> filtered;
    filtered.reserve(source.size());
    for (const GroupRow &group : source) {
        // Preserve a genuinely empty group; this is a workspace fact, not a
        // filter result, and existing sidebar semantics keep that group row.
        if (group.sessions.isEmpty()) {
            filtered.append(group);
            continue;
        }
        GroupRow visible = group;
        visible.sessions.erase(
            std::remove_if(visible.sessions.begin(), visible.sessions.end(),
                           [pinnedOnly, showArchived](const SessionRow &session) {
                               if (pinnedOnly && !session.session.pinned)
                                   return true;
                               return !showArchived && session.session.archived;
                           }),
            visible.sessions.end());
        if (!visible.sessions.isEmpty())
            filtered.append(std::move(visible));
    }
    return filtered;
}
} // namespace

namespace {
constexpr quintptr kTopLevel = std::numeric_limits<quintptr>::max();
} // namespace
SessionsModel::SessionsModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

void SessionsModel::setGroups(QVector<GroupRow> groups)
{
    const bool hadSessions = hasSessions();
    const bool hadUnarchivedSessions = hasUnarchivedSessions();
    beginResetModel();
    allGroups_ = std::move(groups);
    groups_ = filteredGroups(allGroups_, pinnedOnly_, showArchived_);
    endResetModel();
    if (hadSessions != hasSessions() || hadUnarchivedSessions != hasUnarchivedSessions())
        emit sessionPresenceChanged();
}

void SessionsModel::setPinnedOnly(bool pinnedOnly)
{
    if (pinnedOnly_ == pinnedOnly)
        return;
    pinnedOnly_ = pinnedOnly;
    setGroups(allGroups_);
    emit pinnedOnlyChanged();
}

void SessionsModel::setShowArchived(bool showArchived)
{
    if (showArchived_ == showArchived)
        return;
    showArchived_ = showArchived;
    setGroups(allGroups_);
    emit showArchivedChanged();
}

bool SessionsModel::hasSessions() const
{
    for (const GroupRow &group : allGroups_)
        if (!group.sessions.isEmpty())
            return true;
    return false;
}

bool SessionsModel::hasUnarchivedSessions() const
{
    for (const GroupRow &group : allGroups_)
        for (const SessionRow &session : group.sessions)
            if (!session.session.archived)
                return true;
    return false;
}

void SessionsModel::updateTerminalStates(QVector<GroupRow> groups)
{
    // An agent-status change never alters the sidebar's structure, only the
    // per-terminal state that feeds each session's aggregate badge. Compare
    // against the unfiltered source because either filter can omit rows that
    // still receive live state updates.
    if (groups.size() != allGroups_.size()) {
        setGroups(std::move(groups));
        return;
    }
    for (qsizetype gi = 0; gi < groups.size(); ++gi) {
        if (groups.at(gi).group.id.value != allGroups_.at(gi).group.id.value
            || groups.at(gi).sessions.size() != allGroups_.at(gi).sessions.size()) {
            setGroups(std::move(groups));
            return;
        }
        const QVector<SessionRow> &newSessions = groups.at(gi).sessions;
        const QVector<SessionRow> &oldSessions = allGroups_.at(gi).sessions;
        for (qsizetype si = 0; si < newSessions.size(); ++si) {
            const DevSession &incoming = newSessions.at(si).session;
            const DevSession &existing = oldSessions.at(si).session;
            if (incoming.id.value != existing.id.value) {
                setGroups(std::move(groups));
                return;
            }
            // A pin or archive bit that MOVED can add or remove a visible row,
            // so the filtered view has to be rebuilt. Only the bits matter: a
            // reset on every terminal-state update would throw away delegate
            // state and the id-tracked selection many times a second, which is
            // exactly what the targeted path below exists to avoid.
            if (incoming.pinned != existing.pinned
                || incoming.archived != existing.archived) {
                setGroups(std::move(groups));
                return;
            }
        }
    }

    // Structure matches: adopt the new terminal state in place, then emit a
    // targeted dataChanged() (RowStateRole only) for each session row whose
    // aggregate state actually changed. No reset, so delegates and the
    // id-tracked selection persist.
    //
    // Every row is updated BEFORE the first signal goes out. Emitting from
    // inside the mutation loop would show a receiver a half-updated model, and
    // a receiver that reacted by calling setGroups() would free the two vectors
    // the loop is still walking, leaving it iterating over released memory.
    // Indices are taken from the FILTERED vector, because that is what the view
    // is showing: with a filter active the visible row numbers do not line up
    // with the authoritative tree's, and signalling an unfiltered index would
    // repaint the wrong row.
    QSet<QString> changedSessions;
    for (qsizetype gi = 0; gi < groups.size(); ++gi) {
        QVector<SessionRow> &sessions = allGroups_[gi].sessions;
        QVector<SessionRow> &incoming = groups[gi].sessions;
        for (qsizetype si = 0; si < sessions.size(); ++si) {
            const SessionRowState before = aggregateSessionState(sessions.at(si).terminals);
            sessions[si].terminals = std::move(incoming[si].terminals);
            if (before != aggregateSessionState(sessions.at(si).terminals))
                changedSessions.insert(sessions.at(si).session.id.value);
        }
    }
    groups_ = filteredGroups(allGroups_, pinnedOnly_, showArchived_);
    if (changedSessions.isEmpty())
        return;
    QVector<QModelIndex> changedRows;
    for (qsizetype gi = 0; gi < groups_.size(); ++gi) {
        const QModelIndex groupIndex = index(static_cast<int>(gi), 0, QModelIndex());
        const QVector<SessionRow> &visible = groups_.at(gi).sessions;
        for (qsizetype si = 0; si < visible.size(); ++si) {
            if (changedSessions.contains(visible.at(si).session.id.value))
                changedRows.append(index(static_cast<int>(si), 0, groupIndex));
        }
    }
    for (const QModelIndex &row : std::as_const(changedRows))
        emit dataChanged(row, row, {RowStateRole});
}

SessionRowState SessionsModel::aggregateSessionState(const QVector<TerminalStatus> &terminals)
{
    bool anyError = false;
    bool anyWaitingInput = false;
    bool anyRunning = false;
    bool anyFinishedUnseen = false;
    bool anyConnected = false;
    bool anyDisconnected = false;

    for (const TerminalStatus &t : terminals) {
        if (t.connection == TerminalState::Error || t.agent == AgentState::Error)
            anyError = true;
        if (t.agent == AgentState::WaitingInput)
            anyWaitingInput = true;
        if (t.agent == AgentState::Running || t.agent == AgentState::Starting)
            anyRunning = true;
        if (t.agent == AgentState::IdleUnseen)
            anyFinishedUnseen = true;
        if (t.connection == TerminalState::Ready)
            anyConnected = true;
        if (t.connection == TerminalState::Disconnected)
            anyDisconnected = true;
    }

    return aggregateRowState(anyError, anyWaitingInput, anyRunning,
                             anyFinishedUnseen, anyConnected, anyDisconnected);
}

QModelIndex SessionsModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};
    if (!parent.isValid())
        return createIndex(row, column, kTopLevel);
    // parent is a group; sessions store the parent group's row as internalId.
    return createIndex(row, column, static_cast<quintptr>(parent.row()));
}

QModelIndex SessionsModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == kTopLevel)
        return {};
    return createIndex(static_cast<int>(child.internalId()), 0, kTopLevel);
}

int SessionsModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return static_cast<int>(groups_.size());
    if (parent.internalId() != kTopLevel)
        return 0; // sessions have no children
    if (parent.row() < 0 || parent.row() >= groups_.size())
        return 0;
    return static_cast<int>(groups_.at(parent.row()).sessions.size());
}

int SessionsModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QVariant SessionsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    if (index.internalId() == kTopLevel) {
        if (index.row() < 0 || index.row() >= groups_.size())
            return {};
        const Group &group = groups_.at(index.row()).group;
        switch (role) {
        case Qt::DisplayRole:
        case NameRole:
            return group.name;
        case IsGroupRole:
            return true;
        case CollapsedRole:
            return group.collapsed;
        case IdRole:
        case GroupIdRole:
            return group.id.value;
        default:
            return {};
        }
    }

    const int groupRow = static_cast<int>(index.internalId());
    if (groupRow < 0 || groupRow >= groups_.size())
        return {};
    const GroupRow &groupEntry = groups_.at(groupRow);
    if (index.row() < 0 || index.row() >= groupEntry.sessions.size())
        return {};
    const SessionRow &session = groupEntry.sessions.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return session.session.name;
    case SubtitleRole:
        return session.subtitle;
    case RowStateRole:
        return static_cast<int>(aggregateSessionState(session.terminals));
    case IsGroupRole:
        return false;
    case IdRole:
        return session.session.id.value;
    case GroupIdRole:
        return groupEntry.group.id.value;
    case PinnedRole:
        return session.session.pinned;
    case ArchivedRole:
        return session.session.archived;
    default:
        return {};
    }
}

QHash<int, QByteArray> SessionsModel::roleNames() const
{
    // Every role data() actually serves must appear here, otherwise QML cannot
    // reach it by name. Qt::DisplayRole is included because data() answers it
    // (aliased to the row's name) and QAbstractItemModel's default roleNames(),
    // which does name it, is replaced wholesale by this override.
    return {
        {Qt::DisplayRole, "display"},
        {NameRole, "name"},
        {SubtitleRole, "subtitle"},
        {RowStateRole, "rowState"},
        {IsGroupRole, "isGroup"},
        {CollapsedRole, "collapsed"},
        {IdRole, "itemId"},
        {GroupIdRole, "groupId"},
        {PinnedRole, "pinned"},
        {ArchivedRole, "archived"},
    };
}

} // namespace ch
