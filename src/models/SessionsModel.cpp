#include "SessionsModel.h"

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
    groups_ = std::move(groups);
    endResetModel();
}

SessionRowState SessionsModel::aggregateSessionState(const QVector<TerminalStatus> &terminals)
{
    bool anyError = false;
    bool anyWaitingInput = false;
    bool anyRunning = false;
    bool anyFinishedUnseen = false;
    bool anyConnected = false;

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
    }

    return aggregateRowState(anyError, anyWaitingInput, anyRunning, anyFinishedUnseen, anyConnected);
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
    };
}

} // namespace ch
