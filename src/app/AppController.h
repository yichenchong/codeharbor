#pragma once

#include "CodeharbordClient.h"
#include "AgentStatusMonitor.h"
#include "Ids.h"
#include "SessionsModel.h"
#include "WorkspaceDb.h"
#include "WorkspaceTypes.h"
#include "UiStateStore.h"
#include "ServerProfiles.h"
#include "SessionLayouts.h"
#include "SessionBootstrap.h"
#include "SshConnectionPool.h"

#include <QObject>
#include <QPair>
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
    // Connection surface (workstream U integration). These are null until
    // setConnection() injects them, so a test constructing a bare AppController
    // keeps working exactly as before.
    Q_PROPERTY(ch::ServerProfiles* serverProfiles READ serverProfiles NOTIFY connectionChanged)
    Q_PROPERTY(ch::SessionLayouts* layouts READ layouts NOTIFY connectionChanged)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)
    Q_PROPERTY(QString connectionError READ connectionError NOTIFY connectionStateChanged)
    Q_PROPERTY(QString sshDiagnostics READ sshDiagnostics NOTIFY connectionDiagnosticsChanged)
    Q_PROPERTY(QString activeSessionId READ activeSessionId NOTIFY activeSessionChanged)
    Q_PROPERTY(QString activeSessionRepoRoot READ activeSessionRepoRoot NOTIFY activeSessionChanged)

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

    // Inject the connection spine. Ownership stays with main.cpp; any of these
    // may be null (tests). Mirrors setAgentMonitor's injection style rather than
    // widening the constructor, so existing construction sites are untouched.
    void setConnection(SshConnectionPool* pool, SessionBootstrap* bootstrap,
                       ServerProfiles* profiles, SessionLayouts* layouts);

    ServerProfiles* serverProfiles() const { return m_profiles; }
    SessionLayouts* layouts() const { return m_layouts; }
    QString connectionState() const { return m_connectionState; }
    QString connectionError() const { return m_connectionError; }
    QString sshDiagnostics() const;
    QString activeSessionId() const { return m_activeSessionId; }
    QString activeSessionRepoRoot() const;

    // The repository the client talks to; SessionLayouts is built over this.
    WorkspaceDb* workspaceDb() const { return m_db.get(); }

    // Connect to a stored profile. The serverId is then taken from the SERVER
    // (server.info), never from the local profile id: workspace rows are keyed
    // by it on the remote side, so a client-minted id would orphan the user's
    // real groups/sessions whenever a profile was re-added or a second machine
    // connected.
    Q_INVOKABLE void connectToProfile(QString profileId);
    Q_INVOKABLE void disconnectServer();

    // Answer a hostKeyPrompt. Accepting trusts the key and retries the connect;
    // there is no nested event loop anywhere in this flow (the first attempt is
    // refused, the user decides, and we reconnect), so the UI can never be
    // re-entered mid-handshake. A call with no prompt outstanding is ignored:
    // a stale sheet must not be able to declare a live connection disconnected.
    Q_INVOKABLE void resolveHostKey(bool accept);
    // Answer a credentialPrompt with the secret the user typed. `kind` keeps a
    // private-key passphrase separate from a server password, so the former is
    // never accidentally offered to password authentication. Empty cancels;
    // the secret is used for exactly one attempt and never logged.
    Q_INVOKABLE void submitCredential(QString secret, QString kind);
    void submitCredential(QString secret);

    // Oldest codeharbord this client can drive. 4 is where `server.info` began
    // reporting `serverId` (SPEC 3.5); against anything older the id comes back
    // empty, every workspace row is keyed to "" and the user gets a healthy SSH
    // session with a permanently empty sidebar. See adoptServerIdentity().
    static constexpr int kMinimumServerSchemaVersion = 4;

    // Make a Dev Session current: loads both region layouts and remembers it so
    // the next launch reopens the same session.
    Q_INVOKABLE void activateSession(QString devSessionId);

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
    void connectionChanged();
    void connectionStateChanged();
    void connectionDiagnosticsChanged();
    void activeSessionChanged();
    // An UNKNOWN host key was presented (a CHANGED key is refused outright by
    // the pool and never reaches here — SPEC 12.1). Answer with resolveHostKey().
    // `fingerprint` is for DISPLAY and is in OpenSSH's own form,
    // "SHA256:<base64 of the SHA-256 of the key, unpadded>", so the user can
    // compare it character for character with `ssh-keygen -lf` output.
    void hostKeyPrompt(QString host, QString keyType, QString fingerprint);
    // default keys could not authenticate `user` on `host`. `prompt` names the
    // requested credential and `kind` is `keyPassphrase` or `password`.
    // Answer with submitCredential().
    void credentialPrompt(QString user, QString host, QString prompt,
                          QString kind);

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

    // --- connection spine (injected, not owned) ---
    //
    // QPointer, not raw: main.cpp declares SessionLayouts and ServerProfiles
    // AFTER this controller (SessionLayouts is built over our WorkspaceDb), so
    // they are destroyed BEFORE it. Nothing dereferences them during that
    // window today, but a raw pointer makes that a property of the exact
    // declaration order in main() rather than of this class; a QPointer reads
    // back null instead of dangling, which is what the `if (m_layouts)` guards
    // everywhere already assume.
    QPointer<SshConnectionPool> m_pool;
    QPointer<SessionBootstrap> m_bootstrap;
    QPointer<ServerProfiles> m_profiles;
    QPointer<SessionLayouts> m_layouts;

    void setConnectionState(const QString& state, const QString& error = QString());
    // Ask the SERVER for its identity and adopt it as serverId (see
    // connectToProfile), then restore the last active session.
    void adoptServerIdentity();
    void restoreActiveSession();
    // Is `devSessionId` present in the last authoritative tree we read?
    bool sessionExists(const QString& devSessionId) const;
    // The active Dev Session vanished from the authoritative tree (deleted
    // here, or by another client): forget it everywhere it is remembered.
    // Returns true when it actually dropped one, so the caller can emit
    // activeSessionChanged exactly ONCE for the whole refresh rather than
    // thrashing it. Deliberately does NOT emit anything itself.
    bool dropActiveSessionIfGone();
    // Tear down the active-session context: the id and both SessionLayouts
    // region trees. `forget` also drops the remembered session for THIS server,
    // which is right when the session is GONE and wrong when it is merely
    // unreachable (a disconnect must still reopen it on the next connect).
    // Deliberately does NOT emit; callers coalesce activeSessionChanged.
    void clearActiveSession(bool forget);
    // The whole connect attempt. `acceptedFingerprint` is the single host key
    // the user approved for THIS attempt (empty for a first attempt); `secret`
    // is the one password or private-key passphrase typed after a prompt.
    // Both are captured by value and consumed once, so neither can survive to
    // an unrelated host.
    void startConnect(const QString& profileId, QString acceptedFingerprint,
                      QString secret,
                      SshConnectionPool::CredentialKind secretKind);
    // Install the pool's host-key and credential policies for ONE attempt.
    // `acceptedFingerprint` and `secret` are captured by value inside the two
    // callbacks, so they are spendable exactly once and cannot outlive the
    // attempt: startConnect() re-installs the same pair with both arguments
    // EMPTY as soon as connectAndWire() returns. That re-install is what stops
    // a secret libssh never asked for (a passphrase offered to a host that only
    // does password auth, say) from sitting in the pool's callback for the rest
    // of the process, and stops an approval the attempt never reached the key
    // check to spend from arming the next connect to an unrelated host.
    void installPoolCallbacks(QString acceptedFingerprint, QString secret,
                              SshConnectionPool::CredentialKind secretKind);

    QString m_connectionState = QStringLiteral("disconnected");
    QString m_connectionError;
    // A connect-time failure held back while an attempt is in flight, because it
    // may turn out to be the EXPECTED host-key refusal rather than a fault.
    QString m_heldConnectError;
    QString m_activeSessionId;
    // Guards against a second connect being started while one is in flight or a
    // host-key decision is pending. Stays set across the prompt: the attempt is
    // not over until resolveHostKey() answers it.
    bool m_connecting = false;
    // True only while WE are tearing the session down, so the pending-call
    // failures that teardown necessarily causes are not painted as faults.
    bool m_tearingDown = false;
    QString m_pendingProfileId;
    // Fingerprint of the unknown key the CURRENT attempt was refused over; the
    // pair below is the (host, keyType) shown alongside it. Non-empty together
    // with m_connecting means "a prompt we raised is outstanding".
    QString m_pendingFingerprint;
    QPair<QString, QString> m_pendingHostKeyInfo;
    // The host key the user approved for the attempt CHAIN in flight. A chain
    // is one m_connecting window: refuse -> ask -> retry, possibly twice (host
    // key, then credential). The approval has to outlive the single retry that
    // consumed it, because a connect refused at the AUTH stage never got as far
    // as persisting the key it just accepted — without this the user would be
    // asked to re-approve the same host key after typing their password.
    // Cleared with m_connecting, so it never spans two user-initiated connects.
    QString m_approvedFingerprint;
    // The in-flight attempt asked for a password/passphrase and was refused so
    // the user could be prompted. The strings describe it; the SECRET is never
    // held here, only passed through startConnect() into the pool callback that
    // consumes it.
    bool m_credentialRequested = false;
    QString m_credentialUser;
    QString m_credentialLabel;
    SshConnectionPool::CredentialKind m_credentialKind =
        SshConnectionPool::CredentialKind::KeyPassphrase;
};

} // namespace ch
