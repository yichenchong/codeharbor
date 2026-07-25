#pragma once

#include "CodeharbordClient.h"
#include "AgentStatusMonitor.h"
#include "Ids.h"
#include "SessionsModel.h"
#include "WorkspaceDb.h"
#include "WorkspaceTypes.h"
#include "UiStateStore.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <functional>
#include <optional>

namespace ch {


// Top-level application controller (SPEC 4.1, workstream U). Owns the client
// workspace repository (WorkspaceDb over the injected CodeharbordClient), the
// headless sidebar SessionsModel, and the client-local UiStateStore, and exposes
// async workspace mutations to QML as Q_INVOKABLEs. Every mutation is a
// WorkspaceDb round-trip; on success the sidebar model is refreshed from the
// authoritative server state, on RpcError the `error` signal carries the
// server-forwarded message verbatim (SPEC 10.3).
class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(ch::SessionsModel* sessionsModel READ sessionsModel CONSTANT)
    Q_PROPERTY(ch::UiStateStore* uiState READ uiState CONSTANT)
    Q_PROPERTY(QString serverId READ serverId WRITE setServerId NOTIFY serverIdChanged)

public:
    explicit AppController(CodeharbordClient* client, QObject* parent = nullptr);
    ~AppController() override;

    SessionsModel* sessionsModel() const { return m_sessionsModel; }
    UiStateStore* uiState() const { return m_uiState; }

    QString serverId() const { return m_serverId.value; }
    void setServerId(const QString& serverId);

    // Wire the live agent-status monitor (SPEC 6.4) whose per-terminal state is
    // merged into the sidebar on every rebuild. Ownership stays with the caller
    // (the orchestrator/main.cpp). Passing nullptr is safe and disables the
    // merge. Connects the monitor's agentStateChanged/unseenChanged signals to
    // rebuildRows() so a live agent event re-derives the badges from the last
    // known workspace tree.
    void setAgentMonitor(AgentStatusMonitor* monitor);

    // Pure mapping from the nested WorkspaceDb read shape to the flat sidebar
    // model rows. Subtitle is the basename of the session's repositoryRoot;
    // terminal status is left empty here (live terminal state is owned by the
    // terminal workstream and merged separately).
    static QVector<GroupRow> toGroupRows(const QVector<GroupNode>& nodes);

    // Reload the sidebar from the server for the current serverId.
    Q_INVOKABLE void refresh();

    // Group mutations.
    Q_INVOKABLE void createGroup(QString name);
    Q_INVOKABLE void renameGroup(QString id, QString name);
    Q_INVOKABLE void setGroupCollapsed(QString id, bool collapsed);
    Q_INVOKABLE void reorderGroups(QStringList orderedIds);

    // Session mutations.
    Q_INVOKABLE void createSession(QString groupId, QString name, QString repoRoot);
    Q_INVOKABLE void renameSession(QString id, QString name);
    Q_INVOKABLE void duplicateSession(QString id);
    Q_INVOKABLE void moveSession(QString id, QString groupId, int position);
    Q_INVOKABLE void deleteSession(QString id);
    Q_INVOKABLE void reorderSessions(QString groupId, QStringList orderedIds);

signals:
    void serverIdChanged();
    void error(QString message);
    void refreshed();

private:
    // Emit `error` from an optional RpcError; returns true when an error was
    // present (caller should not treat the op as successful).
    bool reportIfError(const std::optional<RpcError>& err);

    // Re-derive the sidebar rows from the last successful list() result cached in
    // m_lastNodes, merging live per-terminal agent state from m_agentMonitor
    // (the source of truth). Called on every successful refresh AND on every
    // agent event, so neither a workspace refresh nor an agent transition ever
    // wipes the other's contribution to the badges.
    void rebuildRows();

    // Build a WorkspaceDb callback that, once the async response arrives, is a
    // no-op if this controller was already destroyed (the shared client keeps
    // pending callbacks alive past our lifetime), emits `error` verbatim on
    // RpcError, and otherwise reloads the sidebar from authoritative server
    // state. `Payload...` matches the callback's leading result argument: empty
    // for an OkCallback, one std::optional<T> for a typed-result callback (the
    // payload is ignored — refresh() always re-reads the full tree).
    template <typename... Payload>
    std::function<void(Payload..., std::optional<RpcError>)> refreshOnSuccess()
    {
        QPointer<AppController> self(this);
        return [self](Payload..., std::optional<RpcError> err) {
            if (!self)
                return;
            if (self->reportIfError(err))
                return;
            self->refresh();
        };
    }

    CodeharbordClient* m_client = nullptr;
    std::unique_ptr<WorkspaceDb> m_db;
    SessionsModel* m_sessionsModel = nullptr;
    UiStateStore* m_uiState = nullptr;
    ServerId m_serverId;
    // Monotonic stamp so a stale (out-of-order) refresh result never overwrites
    // a newer one; see refresh().
    quint64 m_refreshGeneration = 0;
    // Live agent-status monitor (SPEC 6.4), not owned; set via setAgentMonitor.
    // When non-null its per-terminal state is merged into the sidebar rows.
    AgentStatusMonitor* m_agentMonitor = nullptr;
    // Cache of the most recent successful list() tree, so rebuildRows() can
    // re-derive rows (and re-merge agent state) without another server round-
    // trip when an agent event arrives.
    QVector<GroupNode> m_lastNodes;
};

} // namespace ch
