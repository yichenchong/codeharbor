#include "SessionsModel.h"
#include <algorithm>

#include <limits>
#include <utility>

namespace ch {

namespace {
// internalId sentinel marking a top-level (group) index. Session indices instead
// store their parent group's row, which is always a valid non-negative index and
// so never collides with this value.
constexpr quintptr kTopLevel = std::numeric_limits<quintptr>::max();
} // namespace

SessionsModel::SessionsModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

void SessionsModel::setGroups(QVector<GroupRow> groups)
{
    beginResetModel();
    allGroups_ = std::move(groups);
    groups_.clear();
    groups_.reserve(allGroups_.size());
    for (const GroupRow &group : allGroups_) {
        if (!pinnedOnly_) {
            groups_.append(group);
            continue;
        }
        GroupRow filtered = group;
        filtered.sessions.erase(
            std::remove_if(filtered.sessions.begin(), filtered.sessions.end(),
                           [](const SessionRow &session) {
                               return !session.session.pinned;
                           }),
            filtered.sessions.end());
        if (!filtered.sessions.isEmpty())
            groups_.append(std::move(filtered));
    }
    endResetModel();
}

void SessionsModel::setPinnedOnly(bool pinnedOnly)
{
    if (pinnedOnly_ == pinnedOnly)
        return;
    pinnedOnly_ = pinnedOnly;
    setGroups(allGroups_);
    emit pinnedOnlyChanged();
}

void SessionsModel::updateTerminalStates(QVector<GroupRow> groups)
{
    // An agent-status change never alters the sidebar's structure, only the
    // per-terminal state that feeds each session's aggregate badge. Compare
    // against the unfiltered source because a pinned-only view intentionally
    // omits rows that still receive live state updates.
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
            if (newSessions.at(si).session.id.value != oldSessions.at(si).session.id.value) {
                setGroups(std::move(groups));
                return;
            }
        }
    }

    // A filtered model can gain or lose visible rows when a refresh changes a
    // session's pin bit. Rebuild it from the authoritative response rather than
    // attempting to emit row-local signals for indices that no longer exist.
    if (pinnedOnly_) {
        setGroups(std::move(groups));
        return;
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
    QVector<QModelIndex> changedRows;
    for (qsizetype gi = 0; gi < groups.size(); ++gi) {
        const QModelIndex groupIndex = index(static_cast<int>(gi), 0, QModelIndex());
        QVector<SessionRow> &sessions = allGroups_[gi].sessions;
        QVector<SessionRow> &incoming = groups[gi].sessions;
        for (qsizetype si = 0; si < sessions.size(); ++si) {
            const SessionRowState before = aggregateSessionState(sessions.at(si).terminals);
            sessions[si].terminals = std::move(incoming[si].terminals);
            if (before != aggregateSessionState(sessions.at(si).terminals))
                changedRows.append(index(static_cast<int>(si), 0, groupIndex));
        }
    }
    groups_ = allGroups_;
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
    };
}

} // namespace ch
