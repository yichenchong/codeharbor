#include <QtTest/QtTest>

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLockFile>
#include <QPair>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVariantMap>

#include <cstring>
#include <functional>

#include "GroupPaletteService.h"
#include "AppController.h"
#include "SessionLayouts.h"
#include "UiStateStore.h"
#include "WorkspaceDb.h"
#include "WorkspaceTypes.h"
#include "CodeharbordClient.h"
#include "SessionsModel.h"
#include "AgentStatusMonitor.h"
#include "SessionState.h"
#include "TerminalFactory.h"
#include "TerminalController.h"

using namespace ch;

namespace {

// Minimal in-process QIODevice standing in for the RPC transport. The app test
// target deliberately does not link Qt6::Network, so QLocalSocket is out; this
// captures the client's writes verbatim (takeSent) and injects server->client
// frames via deliver(). Opened Unbuffered so the client's write() reaches
// writeData() immediately and readyRead dispatch is synchronous (no event
// loop), letting a test control response ordering exactly.
class FakeTransport : public QIODevice {
public:
    explicit FakeTransport(QObject* parent = nullptr) : QIODevice(parent)
    {
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return m_incoming.size() + QIODevice::bytesAvailable();
    }

    // Inject one server->client frame and dispatch it synchronously.
    void deliver(const QByteArray& frame)
    {
        m_incoming.append(frame);
        emit readyRead();
    }

    // Consume everything the client has written since the last call.
    QByteArray takeSent()
    {
        const QByteArray sent = m_sent;
        m_sent.clear();
        return sent;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_incoming.size());
        if (n > 0) {
            std::memcpy(data, m_incoming.constData(), static_cast<size_t>(n));
            m_incoming.remove(0, n);
        }
        return n;
    }

    qint64 writeData(const char* data, qint64 len) override
    {
        m_sent.append(data, len);
        return len;
    }

private:
    QByteArray m_incoming;
    QByteArray m_sent;
};

// Parse the single JSON-RPC request the client just wrote.
QJsonObject takeRequest(FakeTransport& transport)
{
    const QByteArray sent = transport.takeSent();
    const qsizetype newline = sent.indexOf('\n');
    const QByteArray line = newline >= 0 ? sent.left(newline) : sent;
    return QJsonDocument::fromJson(line).object();
}

// A workspace.list success frame carrying exactly one group of the given name.
QByteArray listResultFrame(int id, const QString& groupName)
{
    const QJsonArray groups{QJsonObject{{"id", groupName}, {"name", groupName}}};
    const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", id}, {"result", groups}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// A workspace.list success frame carrying one group with one session that owns a
// single terminal pane, so the AppController caches terminalPanes that
// rebuildRows() can merge live agent state onto. `harness` is what the row
// stores for that pane; the default is a row with no harness at all, which is
// what every pane created before SPEC 6.6's mint carried.
QByteArray listWithTerminalFrame(int id, const QString& groupName,
                                 const QString& sessionId,
                                 const QString& terminalId,
                                 const QString& harness = QString())
{
    QJsonObject terminal{{"id", terminalId}, {"devSessionId", sessionId}};
    if (!harness.isEmpty())
        terminal.insert(QStringLiteral("harness"), harness);
    const QJsonObject session{{"id", sessionId},
                              {"name", sessionId},
                              {"terminalPanes", QJsonArray{terminal}}};
    const QJsonObject group{{"id", groupName},
                            {"name", groupName},
                            {"sessions", QJsonArray{session}}};
    const QJsonObject resp{{"jsonrpc", "2.0"},
                           {"id", id},
                           {"result", QJsonArray{group}}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// One `terminal_panes` row in the shape remote/src/workspace.ts returns it,
// which is what both updateTerminalPane and resolveTerminalPane answer with.
QByteArray terminalPaneResultFrame(int id, const QString& rowId,
                                   const QString& sessionId,
                                   const QString& harness)
{
    const QJsonObject row{{"id", rowId},
                          {"serverId", "srv-1"},
                          {"devSessionId", sessionId},
                          {"name", rowId},
                          {"workingDirectory", "/repo"},
                          {"tmuxTarget", QStringLiteral("ch_%1_%2").arg(sessionId, rowId)},
                          {"harness", harness},
                          {"position", 0}};
    const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", id}, {"result", row}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// As above, but the session also carries a repositoryRoot. activeSessionRepoRoot
// is derived from it, so a test can change it server-side between refreshes.
QByteArray listWithRepoRootFrame(int id, const QString& groupName,
                                 const QString& sessionId,
                                 const QString& repoRoot)
{
    const QJsonObject session{{"id", sessionId},
                              {"name", sessionId},
                              {"repositoryRoot", repoRoot}};
    const QJsonObject group{{"id", groupName},
                            {"name", groupName},
                            {"sessions", QJsonArray{session}}};
    const QJsonObject resp{{"jsonrpc", "2.0"},
                           {"id", id},
                           {"result", QJsonArray{group}}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}
QByteArray listWithSessionFrame(int id, const QString& groupName,
                                const QString& sessionId, bool archived,
                                bool pinned = false)
{
    const QJsonObject session{{"id", sessionId},
                              {"name", sessionId},
                              {"archived", archived},
                              {"pinned", pinned}};
    const QJsonObject group{{"id", groupName},
                            {"name", groupName},
                            {"sessions", QJsonArray{session}}};
    const QJsonObject resp{{"jsonrpc", "2.0"},
                           {"id", id},
                           {"result", QJsonArray{group}}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// One framed (newline-terminated) AgentEvent JSONL line for the monitor.
QByteArray agentEventLine(const QString& state, const QString& dev,
                          const QString& term)
{
    const QJsonObject o{{"version", 1},
                        {"timestamp", "2026-07-25T00:00:00.000Z"},
                        {"harness", "generic"},
                        {"devSessionId", dev},
                        {"terminalId", term},
                        {"state", state},
                        {"event", "tick"}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

// A JSON-RPC error frame for the given id.
QByteArray errorFrame(int id, int code, const QString& message)
{
    const QJsonObject err{{"code", code}, {"message", message}};
    const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", id}, {"error", err}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// Every JSON-RPC request id the client has written since the last call, in
// order. activateSession() fires TWO getLayout requests through SessionLayouts,
// so takeRequest()'s first-line-only parse is not enough on its own.
QVector<int> takeRequestIds(FakeTransport& transport)
{
    QVector<int> ids;
    const QList<QByteArray> lines = transport.takeSent().split('\n');
    for (const QByteArray& line : lines) {
        if (line.trimmed().isEmpty())
            continue;
        ids.push_back(QJsonDocument::fromJson(line)
                          .object()
                          .value(QStringLiteral("id"))
                          .toInt());
    }
    return ids;
}

// A workspace.getLayout/setLayout success frame: a SessionLayout row whose tree
// is a single leaf (only "tree" is read back).
QByteArray layoutLeafFrame(int id, const QString& paneId)
{
    const QJsonObject tree{{"type", "leaf"}, {"paneId", paneId}};
    const QJsonObject row{{"id", "layout-1"}, {"tree", tree}};
    const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", id}, {"result", row}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// A `server.info` success frame.
QByteArray serverInfoFrame(int id, int schemaVersion, const QString& serverId,
                           const QString& version)
{
    QJsonObject info{{"name", "codeharbord"},
                     {"version", version},
                     {"schemaVersion", schemaVersion}};
    if (!serverId.isEmpty())
        info.insert(QStringLiteral("serverId"), serverId);
    const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", id}, {"result", info}};
    return QJsonDocument(resp).toJson(QJsonDocument::Compact) + '\n';
}

// A bootstrap whose handshake never leaves the process. connectPool() is where
// libssh would run the auth ladder, so `duringConnect` is the seam a test uses
// to be the pool: it fires exactly where SshConnectionPool::authenticate()
// would reach the credential callback AppController installed.
class FakeBootstrap : public SessionBootstrap {
public:
    using SessionBootstrap::SessionBootstrap;

    bool connectOk = false;
    int connectCalls = 0;
    QString identityFile;
    std::function<void()> duringConnect;
    // adoptServerIdentity() hangs off wired(); a subclass may emit its own.
    void fireWired() { emit wired(); }

protected:
    bool probeEndpoint(const QString&, quint16, QString*) override
    {
        return true;
    }

    bool connectPool(const QString&, quint16, const QString&,
                     const QString& identity) override
    {
        identityFile = identity;
        ++connectCalls;
        if (duringConnect)
            duringConnect();
        return connectOk;
    }
};

// Everything AppController's connection surface needs, over throwaway paths.
struct ConnectFixture {
    QTemporaryDir dir;
    SshConnectionPool pool;
    CodeharbordClient client;
    AgentStatusMonitor monitor;
    FakeBootstrap boot{&pool, &client, &monitor};
    ServerProfiles profiles{dir.filePath(QStringLiteral("servers.ini"))};
    AppController controller{&client};
    QString profileId;

    ConnectFixture()
    {
        boot.setKnownHostsPath(dir.filePath(QStringLiteral("known_hosts")));
        controller.setConnection(&pool, &boot, &profiles, nullptr);
        profileId = profiles.addProfile(
            {{QStringLiteral("name"), QStringLiteral("box")},
             {QStringLiteral("host"), QStringLiteral("127.0.0.1")},
             {QStringLiteral("port"), 22},
             {QStringLiteral("user"), QStringLiteral("yichen")},
             {QStringLiteral("identityFile"),
              QStringLiteral("/home/yichen/.ssh/id_ed25519")},
             {QStringLiteral("nodePath"), QStringLiteral("/usr/bin/node")},
             {QStringLiteral("repoRoot"), QStringLiteral("/srv/codeharbor")}});
    }

    // Every byte this fixture persists. The credential tests assert a secret
    // appears in none of it.
    QByteArray allPersistedBytes() const
    {
        QByteArray blob;
        QDirIterator it(dir.path(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile file(it.next());
            if (file.open(QIODevice::ReadOnly))
                blob += file.readAll();
        }
        return blob;
    }
};

// A throwaway host-key blob and the base64 SHA-256 fingerprint AppController
// derives from it. The controller shows the fingerprint and later matches the
// user's approval against exactly this value, so the test has to compute it the
// same way rather than hardcoding a string.
const QByteArray kHostKeyBlob = QByteArrayLiteral("ssh-ed25519 test key material");

QString hostKeyFingerprint()
{
    return QString::fromLatin1(
        QCryptographicHash::hash(kHostKeyBlob, QCryptographicHash::Sha256)
            .toBase64(QByteArray::OmitTrailingEquals));
}

// The same value as the user is shown it: OpenSSH prints a fingerprint as
// "SHA256:" followed by exactly the base64 above, and the approval dialog asks
// the user to compare the two, so the prompt must carry the prefix.
QString displayedHostKeyFingerprint()
{
    return QStringLiteral("SHA256:") + hostKeyFingerprint();
}

// Stand exactly where SshConnectionPool::verifyHostKey() consults the installed
// policy for a host it has never trusted, and record the decision.
SshConnectionPool::HostKeyDecision offerUnknownHostKey(SshConnectionPool& pool)
{
    return pool.hostKeyCallback()(QStringLiteral("box.example"),
                                  QStringLiteral("ssh-ed25519"), kHostKeyBlob,
                                  KnownHosts::Verdict::Unknown);
}

// TerminalFactory::resolveTarget() refuses unless the SSH pool reports
// Connected, which no unit test can reach. Opening that one gate (and nothing
// else) is what lets a test drive a pane's real identity binding and its real
// output hook; tst_terminalfactory uses the same subclass for the same reason.
class OfflineFactory : public TerminalFactory {
public:
    using TerminalFactory::TerminalFactory;
    bool connected() const override { return true; }
};

} // namespace

class TstAppController : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void toGroupRowsMapsNestedNodes();
    void toGroupRowsEmptyIsEmpty();
    void groupPaletteResolvesStableColoursAndDegradesToNoTint();
    void uiStateStorePersistsAcrossInstances();
    void uiStateStoreDocumentedDefaults();
    void toGroupRowsSubtitleHandlesTrailingSlashAndEmpty();
    void uiStateStoreDistinctAndSpecialIds();
    void uiStateStoreRegionWidthsPersistWithoutPerCallSync();
    void uiStateStoreWritesSchemaVersion();
    void setServerIdRefreshesForNewServer();
    void setServerIdUnchangedDoesNotRefresh();
    void refreshUpdatesModelAndEmitsRefreshed();
    void staleRefreshResultDoesNotClobberNewer();
    void refreshErrorEmitsErrorVerbatim();
    void mutationSuccessChainsRefresh();
    void deleteGroupSendsRequestAndRefreshes();
    void deleteGroupErrorEmitsErrorWithoutRefresh();
    void deleteSessionErrorEmitsErrorWithoutRefresh();
    void archiveSessionSendsArchivedAndRefreshes();
    void archiveSessionErrorEmitsErrorWithoutRefresh();
    void setTerminalPaneHarnessWritesTheRowAndRefreshes();
    void setTerminalPaneHarnessRefusesAnUnknownValue();
    void terminalPaneHarnessReadsTheAuthoritativeTree();
    void aGenericPaneFromTheTreeReachesRunningInTheSidebar();
    void refreshResultAfterControllerDestroyedIsNoop();
    void agentMonitorMergesStateIntoSidebar();
    void terminalConnectionStateMergesIntoSidebar();
    void refreshDoesNotWipeAgentDerivedState();
    void markSeenClearsFinishedUnseenBadge();
    void activationRejectsUnknownOrArchivedSessions();
    void vanishedActiveSessionIsRetiredEverywhere();
    void staleOrFailedRefreshNeverRetiresActiveSession();
    // The sidebar's filters hide rows; they must never narrow what the
    // controller BELIEVES exists on the server.
    void sidebarFiltersDoNotNarrowTheAuthoritativeTree();
    // Archiving the currently open session must retire its layout and
    // per-server remembered id before the row is hidden.
    void archivingActiveSessionRetiresItThroughChainedRefresh();
    void deletingActiveSessionRetiresItThroughChainedRefresh();
    void disconnectRetiresActiveSessionButStillRemembersIt();
    void refreshWithoutATransportIsASilentNoOp();
    void serverSideRepoRootChangeNotifiesActiveSession();
    void switchingServerDropsTheActiveSessionButKeepsItRemembered();

    void hostKeyPromptParksTheAttemptAndAcceptRetriesWithItPinned();
    void rejectedHostKeyEndsTheAttemptAndLeavesNothingPinned();
    void hostKeyApprovalSurvivesTheCredentialRetryInTheSameChain();

    // Authentication prompts must distinguish a private-key passphrase from a
    // server password; neither can be retried as the other.
    void credentialCallbackIsInstalledAndParksInsteadOfBlocking();
    void passphraseIsNeverOfferedAsServerPassword();
    void submittedSecretIsSpentOnceAndNeverPersistedOrLogged();
    void cancellingTheCredentialPromptAbandonsTheAttemptCleanly();
    // A server requiring BOTH a key and a password takes two prompts and three
    // attempts; the third has to carry both secrets, each to its own method.
    void twoMethodServerChainCarriesBothSecretsWithoutCrossingThem();
    void connectWhileParkedLeavesTheChainsSecretsIntact();
    void submitCredentialWithAnUnknownKindIsIgnored();
    void serverOlderThanTheSchemaFloorIsRefusedWithBothVersions();
    void serverAtTheSchemaFloorIsAdoptedNormally();
    void uiStateStoreIgnoresCorruptWidths();
    void uiStateStoreRejectsUnreadableSidebarFilters();
    void uiStateStoreRejectsAnEmptyDevSessionId();
    // A profile save that could not take its interprocess lock has to reach the
    // user, not stop at a signal nothing is connected to.
    void aDegradedProfileSaveReachesTheShellsErrorToast();
    // server.info is the one server round-trip that did not route its failure
    // through the teardown gate, and it is in flight for exactly the window a
    // user's Disconnect covers.
    void serverInfoFailureDuringDisconnectIsNotPaintedAsAFault();
    // A server at the schema floor that still names no identity is as
    // undrivable as one that is too old, and must be refused the same way.
    void serverThatReportsNoIdentityIsRefusedRatherThanLeftConnected();
    // The user-facing "update the server" action: it has to arm the bootstrap
    // and then actually dial, and its failure has to reach the user even though
    // the connect it rides on succeeds.
    void upgradingTheRemoteServiceArmsTheBootstrapAndConnects();
    void anUpgradeWithNoServerChosenSaysSoInsteadOfDialling();
    void anUpgradeThatDidNotHappenIsReportedEvenWhenTheConnectSucceeds();
    void anAbandonedUpgradeDoesNotAmbushTheNextOrdinaryConnect();
    // Drag-reordering is the one pair of mutations that sends a whole ordered
    // list rather than a single field, and order is the entire payload.
    void reorderingSendsTheOrderedIdsInOrderAndRefreshes();
};

// Two GroupNodes with sessions map to GroupRows preserving order, with the
// session subtitle set to the basename of repositoryRoot.
void TstAppController::toGroupRowsMapsNestedNodes()
{
    GroupNode g1;
    g1.group.id = GroupId{QStringLiteral("g1")};
    g1.group.name = QStringLiteral("Work");

    SessionNode s1;
    s1.session.id = DevSessionId{QStringLiteral("s1")};
    s1.session.name = QStringLiteral("codeharbor");
    s1.session.repositoryRoot = QStringLiteral("/home/u/proj");

    SessionNode s2;
    s2.session.id = DevSessionId{QStringLiteral("s2")};
    s2.session.name = QStringLiteral("docs");
    s2.session.repositoryRoot = QStringLiteral("/home/u/manual");
    g1.sessions = {s1, s2};

    GroupNode g2;
    g2.group.id = GroupId{QStringLiteral("g2")};
    g2.group.name = QStringLiteral("Personal");

    SessionNode s3;
    s3.session.id = DevSessionId{QStringLiteral("s3")};
    s3.session.name = QStringLiteral("dotfiles");
    s3.session.repositoryRoot = QStringLiteral("/home/u/config");
    g2.sessions = {s3};

    const QVector<GroupRow> rows = AppController::toGroupRows({g1, g2});

    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).group.name, QStringLiteral("Work"));
    QCOMPARE(rows.at(1).group.name, QStringLiteral("Personal"));

    QCOMPARE(rows.at(0).sessions.size(), 2);
    QCOMPARE(rows.at(0).sessions.at(0).session.name, QStringLiteral("codeharbor"));
    QCOMPARE(rows.at(0).sessions.at(0).subtitle, QStringLiteral("proj"));
    QCOMPARE(rows.at(0).sessions.at(1).subtitle, QStringLiteral("manual"));
    QVERIFY(rows.at(0).sessions.at(0).terminals.isEmpty());

    QCOMPARE(rows.at(1).sessions.size(), 1);
    QCOMPARE(rows.at(1).sessions.at(0).session.name, QStringLiteral("dotfiles"));
    QCOMPARE(rows.at(1).sessions.at(0).subtitle, QStringLiteral("config"));
}

void TstAppController::toGroupRowsEmptyIsEmpty()
{
    QVERIFY(AppController::toGroupRows({}).isEmpty());
}

// The one palette surface QML actually calls (SessionsSidebar.qml). "plain" is
// the deliberate no-tint look, an unrecognised name degrades to the same, and a
// resolvable palette answers a stable colour for a given group name.
void TstAppController::groupPaletteResolvesStableColoursAndDegradesToNoTint()
{
    GroupPaletteService service;

    // No colour at all, so the sidebar keeps its untinted look rather than
    // showing whatever the first slot happens to be.
    QVERIFY(!service
                 .colorFor(QStringLiteral("build"), QStringLiteral("plain"),
                           AppSettings::kDefaultPaletteSize)
                 .isValid());
    QVERIFY(!service
                 .colorFor(QStringLiteral("build"),
                           QStringLiteral("no-such-palette"),
                           AppSettings::kDefaultPaletteSize)
                 .isValid());

    // A resolvable palette answers a real colour, and the same group name
    // answers the SAME colour every time - including after the service's cache
    // has been invalidated by a different size and then asked for the first
    // size again.
    const QColor first =
        service.colorFor(QStringLiteral("build"), QStringLiteral("tokyonight"),
                         AppSettings::kDefaultPaletteSize);
    QVERIFY(first.isValid());
    QCOMPARE(service.colorFor(QStringLiteral("build"),
                              QStringLiteral("tokyonight"),
                              AppSettings::kDefaultPaletteSize),
             first);
    service.colorFor(QStringLiteral("build"), QStringLiteral("tokyonight"),
                     AppSettings::kMaxPaletteSize);
    QCOMPARE(service.colorFor(QStringLiteral("build"),
                              QStringLiteral("tokyonight"),
                              AppSettings::kDefaultPaletteSize),
             first);

    // The smallest size the preference allows is the seed itself. The generator
    // refuses a request that adds nothing, so this is the one size that must be
    // served from the seed directly - and it must still resolve.
    QVERIFY(service
                .colorFor(QStringLiteral("build"), QStringLiteral("tokyonight"),
                          AppSettings::kMinPaletteSize)
                .isValid());
}

// A fresh store over the same .ini file reads back exactly what a previous
// instance wrote — proving persistence via QSettings' flush on destruction
// (setRegionWidths no longer sync()s per call, to avoid handle-drag jank).
void TstAppController::uiStateStorePersistsAcrossInstances()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("ui.ini"));

    {
        UiStateStore store(iniPath);
        store.setRegionWidths(200, 0, 400);
        store.setSelectedPane(QStringLiteral("s1"), QStringLiteral("p9"));
        store.setShowArchived(true);
        // The pin filter is the exact twin of showArchived above and had no
        // coverage at all: emptying setPinnedOnly()/pinnedOnly() broke nothing,
        // and a filter that silently forgets itself reopens the sidebar in a
        // mode the user did not choose.
        store.setPinnedOnly(true);
    }

    UiStateStore reopened(iniPath);
    QCOMPARE(reopened.sidebarWidth(), 200);
    QCOMPARE(reopened.viewerWidth(), 0);
    QCOMPARE(reopened.terminalWidth(), 400);
    QCOMPARE(reopened.selectedPane(QStringLiteral("s1")), QStringLiteral("p9"));
    QVERIFY(reopened.showArchived());
    QVERIFY(reopened.pinnedOnly());
}

// Documented defaults when nothing has been written.
void TstAppController::uiStateStoreDocumentedDefaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("empty.ini"));

    UiStateStore store(iniPath);
    QCOMPARE(store.sidebarWidth(), 260);
    QCOMPARE(store.viewerWidth(), 0);
    QCOMPARE(store.terminalWidth(), 520);
    QVERIFY(!store.showArchived());
    QVERIFY(!store.pinnedOnly());
    QVERIFY(store.selectedPane(QStringLiteral("unknown")).isEmpty());
}

// repositoryRoot basenames: a trailing slash (or several), a "." segment, and
// relative paths still yield the final path component; an empty or root path
// yields an empty subtitle. Guards the QFileInfo trailing-slash pitfall
// (QFileInfo("/a/b/").fileName() == "").
void TstAppController::toGroupRowsSubtitleHandlesTrailingSlashAndEmpty()
{
    GroupNode g;
    g.group.id = GroupId{QStringLiteral("g")};

    const QVector<QPair<QString, QString>> cases = {
        {QStringLiteral("/home/u/proj/"), QStringLiteral("proj")},
        {QStringLiteral("/home/u/proj//"), QStringLiteral("proj")},
        {QStringLiteral("/home/u/./proj"), QStringLiteral("proj")},
        {QStringLiteral("relative/"), QStringLiteral("relative")},
        {QString(), QString()},
        {QStringLiteral("/"), QString()},
    };
    for (const auto& c : cases) {
        SessionNode s;
        s.session.id = DevSessionId{c.first};
        s.session.repositoryRoot = c.first;
        g.sessions.push_back(s);
    }

    const QVector<GroupRow> rows = AppController::toGroupRows({g});
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).sessions.size(), cases.size());
    for (qsizetype i = 0; i < cases.size(); ++i)
        QCOMPARE(rows.at(0).sessions.at(i).subtitle, cases.at(i).second);
}

// Distinct devSessionIds address independent panes (no key collision), and ids
// containing separator/special characters round-trip intact across a reopen.
void TstAppController::uiStateStoreDistinctAndSpecialIds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("panes.ini"));

    {
        UiStateStore store(iniPath);
        store.setSelectedPane(QStringLiteral("s1"), QStringLiteral("viewer"));
        store.setSelectedPane(QStringLiteral("s2"), QStringLiteral("terminal"));
        // Separator/space-bearing ids must not collide or corrupt the key.
        store.setSelectedPane(QStringLiteral("srv/grp:has space"),
                              QStringLiteral("editor"));
    }

    UiStateStore reopened(iniPath);
    QCOMPARE(reopened.selectedPane(QStringLiteral("s1")), QStringLiteral("viewer"));
    QCOMPARE(reopened.selectedPane(QStringLiteral("s2")), QStringLiteral("terminal"));
    QCOMPARE(reopened.selectedPane(QStringLiteral("srv/grp:has space")),
             QStringLiteral("editor"));
}

// setRegionWidths no longer calls QSettings::sync() on every invocation (a
// handle drag fires it repeatedly; a synchronous disk write per pixel caused
// jank). Simulate a drag with many writes, then prove the final values still
// persist to a fresh instance via the destructor flush — no explicit sync.
void TstAppController::uiStateStoreRegionWidthsPersistWithoutPerCallSync()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("drag.ini"));

    {
        UiStateStore store(iniPath);
        // Rapid intermediate writes, as during a handle drag.
        for (int w = 180; w <= 300; ++w)
            store.setRegionWidths(w, 0, 640 - w);
        // Final settled widths.
        store.setRegionWidths(300, 0, 340);
        // No explicit sync() here: the destructor at end of scope flushes.
    }

    UiStateStore reopened(iniPath);
    QCOMPARE(reopened.sidebarWidth(), 300);
    QCOMPARE(reopened.viewerWidth(), 0);
    QCOMPARE(reopened.terminalWidth(), 340);
}

void TstAppController::uiStateStoreWritesSchemaVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("schema.ini"));
    {
        UiStateStore store(iniPath);
        store.setSelectedPane(QStringLiteral("session-1"),
                              QStringLiteral("pane-1"));
    }

    {
        QSettings raw(iniPath, QSettings::IniFormat);
        QCOMPARE(raw.value(QStringLiteral("meta/schemaVersion")).toInt(), 1);
    }

    // A pure READ never stamps anything: merely opening the store on a legacy
    // file (or on a machine where the user changes nothing) must leave the file
    // exactly as it was.
    const QString virginPath = dir.filePath(QStringLiteral("virgin.ini"));
    {
        UiStateStore store(virginPath);
        QCOMPARE(store.sidebarWidth(), 260);
        QVERIFY(store.selectedPane(QStringLiteral("session-1")).isEmpty());
    }
    {
        QSettings raw(virginPath, QSettings::IniFormat);
        QVERIFY(!raw.contains(QStringLiteral("meta/schemaVersion")));
    }

    // A file a NEWER build already migrated carries a HIGHER version. Stamping
    // it back down to ours would erase the only record that the newer schema
    // ever touched it, and that build would then re-migrate converted data.
    const QString futurePath = dir.filePath(QStringLiteral("future.ini"));
    {
        QSettings raw(futurePath, QSettings::IniFormat);
        raw.setValue(QStringLiteral("meta/schemaVersion"), 99);
        raw.sync();
    }
    {
        UiStateStore store(futurePath);
        store.setSelectedPane(QStringLiteral("session-1"),
                              QStringLiteral("pane-1"));
    }
    {
        QSettings raw(futurePath, QSettings::IniFormat);
        QCOMPARE(raw.value(QStringLiteral("meta/schemaVersion")).toInt(), 99);
        // ...and the write itself still landed.
        QCOMPARE(raw.value(QStringLiteral("selectedPane/session-1")).toString(),
                 QStringLiteral("pane-1"));
    }
}

// setServerId to a new value must reload the sidebar from that server: nothing
// else re-drives refresh() on a server change, so the property setter must.
void TstAppController::setServerIdRefreshesForNewServer()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.setServerId(QStringLiteral("srv-x"));

    const QJsonObject req = takeRequest(transport);
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));
    QCOMPARE(req.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-x"));

    QSignalSpy refreshedSpy(&controller, &AppController::refreshed);
    transport.deliver(listResultFrame(req.value(QStringLiteral("id")).toInt(),
                                      QStringLiteral("Alpha")));

    QCOMPARE(refreshedSpy.count(), 1);
    SessionsModel* model = controller.sessionsModel();
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->data(model->index(0, 0), SessionsModel::NameRole).toString(),
             QStringLiteral("Alpha"));
}

// Re-setting the current serverId is a no-op: no serverIdChanged, no refresh.
void TstAppController::setServerIdUnchangedDoesNotRefresh()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.setServerId(QStringLiteral("srv-x"));
    QVERIFY(!transport.takeSent().isEmpty()); // the first set drove a refresh

    controller.setServerId(QStringLiteral("srv-x"));
    QVERIFY(transport.takeSent().isEmpty()); // unchanged -> no second refresh
}

// A plain refresh() maps the server tree into the model and signals refreshed().
void TstAppController::refreshUpdatesModelAndEmitsRefreshed()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    QSignalSpy refreshedSpy(&controller, &AppController::refreshed);
    controller.refresh();
    transport.deliver(listResultFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("Only")));

    QCOMPARE(refreshedSpy.count(), 1);
    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
}

// Concurrent mutations chain several refreshes; the client routes responses by
// id, so replies can arrive out of order. The newest refresh must win no matter
// the arrival order — here the stale reply is delivered LAST and must be dropped
// (without the generation guard it would clobber the model with stale rows).
void TstAppController::staleRefreshResultDoesNotClobberNewer()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.refresh();
    const int firstId =
        takeRequest(transport).value(QStringLiteral("id")).toInt();
    controller.refresh();
    const int secondId =
        takeRequest(transport).value(QStringLiteral("id")).toInt();
    QVERIFY(firstId != secondId);

    // Deliver the newer request's result first, then the older/stale one.
    transport.deliver(listResultFrame(secondId, QStringLiteral("Fresh")));
    transport.deliver(listResultFrame(firstId, QStringLiteral("Stale")));

    SessionsModel* model = controller.sessionsModel();
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->data(model->index(0, 0), SessionsModel::NameRole).toString(),
             QStringLiteral("Fresh"));
}

// A refresh RpcError is forwarded verbatim via error() and leaves the model as-is.
void TstAppController::refreshErrorEmitsErrorVerbatim()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    QSignalSpy errorSpy(&controller, &AppController::error);
    controller.refresh();
    const int id = takeRequest(transport).value(QStringLiteral("id")).toInt();
    transport.deliver(errorFrame(id, -32000, QStringLiteral("kaboom")));

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toString(), QStringLiteral("kaboom"));
    QCOMPARE(controller.sessionsModel()->rowCount(), 0);
}

// A successful mutation chains an authoritative refresh that updates the model.
void TstAppController::mutationSuccessChainsRefresh()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.createGroup(QStringLiteral("New"));
    const QJsonObject createReq = takeRequest(transport);
    QCOMPARE(createReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.createGroup"));

    // Ack the create; the controller must then chain a workspace.list refresh.
    const QJsonObject created{{"id", "g1"}, {"name", "New"}};
    const QJsonObject ack{{"jsonrpc", "2.0"},
                          {"id", createReq.value(QStringLiteral("id")).toInt()},
                          {"result", created}};
    transport.deliver(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n');

    const QJsonObject listReq = takeRequest(transport);
    QCOMPARE(listReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));
    transport.deliver(listResultFrame(
        listReq.value(QStringLiteral("id")).toInt(), QStringLiteral("New")));

    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
}
void TstAppController::deleteGroupSendsRequestAndRefreshes()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.deleteGroup(QStringLiteral("g1"));
    const QJsonObject deleteReq = takeRequest(transport);
    QCOMPARE(deleteReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.deleteGroup"));
    QCOMPARE(deleteReq.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("id"))
                 .toString(),
             QStringLiteral("g1"));

    const QJsonObject ack{{"jsonrpc", "2.0"},
                          {"id", deleteReq.value(QStringLiteral("id")).toInt()},
                          {"result", true}};
    transport.deliver(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n');

    const QJsonObject refreshReq = takeRequest(transport);
    QCOMPARE(refreshReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));
    transport.deliver(listResultFrame(
        refreshReq.value(QStringLiteral("id")).toInt(), QStringLiteral("remaining")));
    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
    QCOMPARE(controller.sessionsModel()
                 ->data(controller.sessionsModel()->index(0, 0), SessionsModel::NameRole)
                 .toString(),
             QStringLiteral("remaining"));
}

void TstAppController::deleteGroupErrorEmitsErrorWithoutRefresh()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.refresh();
    const QJsonObject listReq = takeRequest(transport);
    transport.deliver(listResultFrame(
        listReq.value(QStringLiteral("id")).toInt(), QStringLiteral("g1")));
    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
    transport.takeSent();

    QSignalSpy errors(&controller, &AppController::error);
    controller.deleteGroup(QStringLiteral("g1"));
    const int deleteId = takeRequest(transport).value(QStringLiteral("id")).toInt();
    transport.deliver(errorFrame(deleteId, -32000, QStringLiteral("group is locked")));

    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.at(0).at(0).toString(), QStringLiteral("group is locked"));
    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
    QVERIFY(transport.takeSent().isEmpty());
}

void TstAppController::deleteSessionErrorEmitsErrorWithoutRefresh()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.refresh();
    const QJsonObject listReq = takeRequest(transport);
    transport.deliver(listWithSessionFrame(
        listReq.value(QStringLiteral("id")).toInt(), QStringLiteral("g1"),
        QStringLiteral("s1"), false));
    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
    transport.takeSent();

    QSignalSpy errors(&controller, &AppController::error);
    controller.deleteSession(QStringLiteral("s1"));
    const int deleteId = takeRequest(transport).value(QStringLiteral("id")).toInt();
    transport.deliver(errorFrame(deleteId, -32000, QStringLiteral("session is locked")));

    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.at(0).at(0).toString(), QStringLiteral("session is locked"));
    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
    QVERIFY(transport.takeSent().isEmpty());
}
// Archiving and unarchiving use the existing workspace.updateSession field,
// then rebuild the sidebar from the server's authoritative list. The default
// model filter hides the archived row until the unarchive refresh restores it.
void TstAppController::archiveSessionSendsArchivedAndRefreshes()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.archiveSession(QStringLiteral("s1"));
    const QJsonObject archiveReq = takeRequest(transport);
    QCOMPARE(archiveReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.updateSession"));
    const QJsonObject archiveParams = archiveReq.value(QStringLiteral("params")).toObject();
    QCOMPARE(archiveParams.value(QStringLiteral("id")).toString(), QStringLiteral("s1"));
    QCOMPARE(archiveParams.value(QStringLiteral("archived")).toBool(), true);

    const QJsonObject archivedAck{{"jsonrpc", "2.0"},
                                  {"id", archiveReq.value(QStringLiteral("id")).toInt()},
                                  {"result", QJsonObject{{"id", "s1"},
                                                         {"name", "s1"},
                                                         {"archived", true}}}};
    transport.deliver(QJsonDocument(archivedAck).toJson(QJsonDocument::Compact) + '\n');
    const QJsonObject archiveRefresh = takeRequest(transport);
    QCOMPARE(archiveRefresh.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));
    transport.deliver(listWithSessionFrame(
        archiveRefresh.value(QStringLiteral("id")).toInt(), QStringLiteral("g"),
        QStringLiteral("s1"), true));
    QCOMPARE(controller.sessionsModel()->rowCount(), 0);
    QVERIFY(controller.sessionsModel()->hasSessions());

    controller.unarchiveSession(QStringLiteral("s1"));
    const QJsonObject unarchiveReq = takeRequest(transport);
    const QJsonObject unarchiveParams = unarchiveReq.value(QStringLiteral("params")).toObject();
    QCOMPARE(unarchiveParams.value(QStringLiteral("archived")).toBool(), false);
    const QJsonObject unarchiveAck{{"jsonrpc", "2.0"},
                                   {"id", unarchiveReq.value(QStringLiteral("id")).toInt()},
                                   {"result", QJsonObject{{"id", "s1"},
                                                          {"name", "s1"},
                                                          {"archived", false}}}};
    transport.deliver(QJsonDocument(unarchiveAck).toJson(QJsonDocument::Compact) + '\n');
    const QJsonObject unarchiveRefresh = takeRequest(transport);
    transport.deliver(listWithSessionFrame(
        unarchiveRefresh.value(QStringLiteral("id")).toInt(), QStringLiteral("g"),
        QStringLiteral("s1"), false));
    QCOMPARE(controller.sessionsModel()->rowCount(), 1);
    const QModelIndex group = controller.sessionsModel()->index(0, 0);
    const QModelIndex session = controller.sessionsModel()->index(0, 0, group);
    QCOMPARE(controller.sessionsModel()->data(session, SessionsModel::ArchivedRole).toBool(),
             false);
}

void TstAppController::archiveSessionErrorEmitsErrorWithoutRefresh()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);
    QSignalSpy errors(&controller, &AppController::error);

    controller.archiveSession(QStringLiteral("s1"));
    const int id = takeRequest(transport).value(QStringLiteral("id")).toInt();
    transport.deliver(errorFrame(id, -32000, QStringLiteral("archive denied")));

    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.at(0).at(0).toString(), QStringLiteral("archive denied"));
    QVERIFY(transport.takeSent().isEmpty());
}

// Setting a pane's harness is one workspace.updateTerminalPane carrying just
// that field, and the refresh it chains is not cosmetic: refresh()'s harness
// walk is the only thing that re-registers the pane with the agent monitor, so
// without it the new value would change nothing the user can see.
void TstAppController::setTerminalPaneHarnessWritesTheRowAndRefreshes()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);
    QSignalSpy errors(&controller, &AppController::error);

    controller.setTerminalPaneHarness(QStringLiteral("term-1"),
                                      QStringLiteral("claude-code"));
    const QJsonObject update = takeRequest(transport);
    QCOMPARE(update.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.updateTerminalPane"));
    const QJsonObject params = update.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("id")).toString(),
             QStringLiteral("term-1"));
    QCOMPARE(params.value(QStringLiteral("harness")).toString(),
             QStringLiteral("claude-code"));

    transport.deliver(terminalPaneResultFrame(
        update.value(QStringLiteral("id")).toInt(), QStringLiteral("term-1"),
        QStringLiteral("sess-1"), QStringLiteral("claude-code")));
    QCOMPARE(takeRequest(transport).value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));
    QCOMPARE(errors.count(), 0);
}

// The four wire values plus "" are the whole vocabulary (ch::detail::
// isHarnessWire). Anything else is refused where the user can be told, rather
// than stored: the server would take the string and the monitor would then
// never match it, so the pane would go quiet with nothing to explain why.
void TstAppController::setTerminalPaneHarnessRefusesAnUnknownValue()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);
    QSignalSpy errors(&controller, &AppController::error);

    controller.setTerminalPaneHarness(QStringLiteral("term-1"),
                                      QStringLiteral("codex"));
    QCOMPARE(errors.count(), 1);
    QVERIFY(errors.at(0).at(0).toString().contains(QStringLiteral("codex")));
    QVERIFY(transport.takeSent().isEmpty());

    // The empty string is legal and IS sent: it is how a pane is put back to
    // being a plain shell that stays quiet.
    controller.setTerminalPaneHarness(QStringLiteral("term-1"), QString());
    const QJsonObject update = takeRequest(transport);
    QCOMPARE(update.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.updateTerminalPane"));
    QVERIFY(update.value(QStringLiteral("params")).toObject()
                .value(QStringLiteral("harness")).toString().isEmpty());
    QCOMPARE(errors.count(), 1);
}

// The getter reports what the last authoritative tree holds, and answers for a
// pane that tree has never heard of instead of asserting: the caller is the UI
// asking about a pane another client may have closed.
void TstAppController::terminalPaneHarnessReadsTheAuthoritativeTree()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    QVERIFY(controller.terminalPaneHarness(QStringLiteral("term-1")).isEmpty());

    controller.refresh();
    transport.deliver(listWithTerminalFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("G"), QStringLiteral("sess-1"), QStringLiteral("term-1"),
        QStringLiteral("oh-my-pi")));

    QCOMPARE(controller.terminalPaneHarness(QStringLiteral("term-1")),
             QStringLiteral("oh-my-pi"));
    QVERIFY(controller.terminalPaneHarness(QStringLiteral("gone")).isEmpty());
}

// A late response after the controller is destroyed must be a no-op: the shared
// client keeps the pending callback alive past our lifetime, and the QPointer
// guard on every callback makes the delayed dispatch safe (no use-after-free).
void TstAppController::refreshResultAfterControllerDestroyedIsNoop()
{
    FakeTransport transport;
    CodeharbordClient client;
    client.setTransport(&transport);

    int pendingId = 0;
    {
        AppController controller(&client);
        controller.refresh();
        pendingId =
            takeRequest(transport).value(QStringLiteral("id")).toInt();
        // controller goes out of scope here with the list request pending.
    }

    // Must not touch the destroyed controller.
    transport.deliver(listResultFrame(pendingId, QStringLiteral("Late")));
    QVERIFY(true); // reaching here without a crash is the assertion
}

// A live agent state fed to the monitor must surface in the sidebar: after a
// refresh populates a session with a terminal pane, an agentStateChanged event
// re-derives the row and its aggregate reflects the new AgentState. Before any
// event the terminal's agent is Unknown and its connection is Unloaded, so the
// aggregate is neutral Idle rather than a claimed disconnect.
void TstAppController::agentMonitorMergesStateIntoSidebar()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    // A real monitor over its own in-process transport (no SSH needed).
    FakeTransport agentTransport;
    AgentStatusMonitor monitor;
    monitor.setTransport(&agentTransport);
    controller.setAgentMonitor(&monitor);

    controller.refresh();
    transport.deliver(listWithTerminalFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("G"), QStringLiteral("sess-1"), QStringLiteral("term-1")));

    SessionsModel* model = controller.sessionsModel();
    auto sessionState = [model]() {
        const QModelIndex group = model->index(0, 0);
        const QModelIndex session = model->index(0, 0, group);
        return model->data(session, SessionsModel::RowStateRole).toInt();
    };
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Idle));

    // Feed a running state for that (session, terminal); the monitor's
    // agentStateChanged drives rebuildRows() and the badge appears.
    agentTransport.deliver(agentEventLine(QStringLiteral("running"),
                                          QStringLiteral("sess-1"),
                                          QStringLiteral("term-1")));
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Running));
}
// A connected terminal must reach the same sidebar aggregate that a real
// TerminalFactory emits. The old controller only merged agent state, so this
// test stayed Disconnected even after the pane was Ready.
void TstAppController::terminalConnectionStateMergesIntoSidebar()
{
    CodeharbordClient client;
    AppController controller(&client);
    // Seed the server key without issuing a request: setServerId() is allowed
    // to run before a transport is wired, and the subsequent refresh is the
    // one request this test answers.
    controller.setServerId(QStringLiteral("srv-1"));

    TerminalFactory factory(nullptr);
    factory.setServerId(QStringLiteral("srv-1"));
    controller.setTerminalFactory(&factory);

    FakeTransport transport;
    client.setTransport(&transport);
    controller.refresh();
    const QJsonObject request = takeRequest(transport);
    transport.deliver(listWithTerminalFrame(
        request.value(QStringLiteral("id")).toInt(), QStringLiteral("G"),
        QStringLiteral("sess-1"), QStringLiteral("term-1")));

    SessionsModel* model = controller.sessionsModel();
    const auto sessionState = [model]() {
        const QModelIndex group = model->index(0, 0);
        const QModelIndex session = model->index(0, 0, group);
        return model->data(session, SessionsModel::RowStateRole).toInt();
    };
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Idle));

    // Ready is a live pane and must not be mistaken for a missing/disconnected
    // terminal. This assertion fails against the old agent-only merge.
    emit factory.terminalStateChanged(QStringLiteral("srv-1"),
                                      QStringLiteral("sess-1"),
                                      QStringLiteral("term-1"),
                                      TerminalState::Ready);
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Idle));

    // Once that live pane actually loses its channel, Disconnected is correct.
    emit factory.terminalStateChanged(QStringLiteral("srv-1"),
                                      QStringLiteral("sess-1"),
                                      QStringLiteral("term-1"),
                                      TerminalState::Disconnected);
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Disconnected));

    // A queued event stamped with the previous server cannot repaint this one.
    emit factory.terminalStateChanged(QStringLiteral("old-server"),
                                      QStringLiteral("sess-1"),
                                      QStringLiteral("term-1"),
                                      TerminalState::Ready);
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Disconnected));
}

// A subsequent refresh with the SAME tree must not wipe the agent-derived
// badge: rebuildRows() always re-reads the monitor (the source of truth), so
// the row state persists across a workspace refresh.
void TstAppController::refreshDoesNotWipeAgentDerivedState()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    FakeTransport agentTransport;
    AgentStatusMonitor monitor;
    monitor.setTransport(&agentTransport);
    controller.setAgentMonitor(&monitor);

    controller.refresh();
    transport.deliver(listWithTerminalFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("G"), QStringLiteral("sess-1"), QStringLiteral("term-1")));

    agentTransport.deliver(agentEventLine(QStringLiteral("waiting_input"),
                                          QStringLiteral("sess-1"),
                                          QStringLiteral("term-1")));

    SessionsModel* model = controller.sessionsModel();
    auto sessionState = [model]() {
        const QModelIndex group = model->index(0, 0);
        const QModelIndex session = model->index(0, 0, group);
        return model->data(session, SessionsModel::RowStateRole).toInt();
    };
    QCOMPARE(sessionState(),
             static_cast<int>(SessionRowState::WaitingForInput));

    // Another full refresh with an identical tree; without re-reading the
    // monitor this would reset terminals to empty and wipe the badge.
    controller.refresh();
    transport.deliver(listWithTerminalFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("G"), QStringLiteral("sess-1"), QStringLiteral("term-1")));

    QCOMPARE(sessionState(),
             static_cast<int>(SessionRowState::WaitingForInput));
}

// THE regression this whole change exists for. A pane the UI mints is stored
// with harness `generic`, and SPEC 6.6 then derives its state from terminal
// output alone - no agent hook, no wire events. Before the fix the mint sent no
// harness at all, so refresh()'s walk registered nothing, the activity clock
// never started, and the sidebar row for the session said Idle no matter what
// the shell was doing. Driven through the production route end to end: the tree
// registers the harness, and the pane's own bytes reach the monitor through
// TerminalFactory's outputReceived hook.
void TstAppController::aGenericPaneFromTheTreeReachesRunningInTheSidebar()
{
    CodeharbordClient client;
    AppController controller(&client);
    // Seeded before the transport, so the only request in flight is the
    // refresh below (see terminalConnectionStateMergesIntoSidebar).
    controller.setServerId(QStringLiteral("srv-1"));

    FakeTransport agentTransport;
    AgentStatusMonitor monitor;
    monitor.setTransport(&agentTransport);
    controller.setAgentMonitor(&monitor);

    FakeTransport transport;
    client.setTransport(&transport);
    controller.refresh();
    transport.deliver(listWithTerminalFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("G"), QStringLiteral("sess-1"), QStringLiteral("term-1"),
        QStringLiteral("generic")));

    SessionsModel* model = controller.sessionsModel();
    const auto sessionState = [model]() {
        const QModelIndex group = model->index(0, 0);
        const QModelIndex session = model->index(0, 0, group);
        return model->data(session, SessionsModel::RowStateRole).toInt();
    };
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Idle));

    // The pane attaches. Only a pane the monitor knows to be generic moves to
    // Starting, and nothing but the tree has registered anything yet, so this
    // is the harness arriving through refresh()'s walk. (The attach itself is
    // made by hand: the real one needs an SSH channel. Same call, same
    // argument - tst_terminalfactory stands in the same way.)
    monitor.noteTerminalAttached(QStringLiteral("sess-1"),
                                 QStringLiteral("term-1"));
    QCOMPARE(monitor.stateFor(QStringLiteral("sess-1"), QStringLiteral("term-1")),
             static_cast<int>(AgentState::Starting));

    // A real factory over the controller's own repository, resolving the pane
    // exactly as opening it does, so the output hook below is the production
    // one and not a hand-made call into the monitor.
    OfflineFactory factory(nullptr);
    factory.setWorkspace(controller.workspaceDb());
    factory.setServerId(QStringLiteral("srv-1"));
    factory.setAgentMonitor(&monitor);
    controller.setTerminalFactory(&factory);

    QObject pane;
    TerminalController* paneController = factory.create(&pane);
    QVERIFY(factory.resolveTarget(paneController, QStringLiteral("sess-1"),
                                  QStringLiteral("terminal-1"),
                                  QStringLiteral("term-1"),
                                  QStringLiteral("/repo")));
    const QJsonObject resolve = takeRequest(transport);
    QCOMPARE(resolve.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.resolveTerminalPane"));
    transport.deliver(terminalPaneResultFrame(
        resolve.value(QStringLiteral("id")).toInt(), QStringLiteral("term-1"),
        QStringLiteral("sess-1"), QStringLiteral("generic")));

    // The shell prints. That is the only evidence SPEC 6.6 has, and it is
    // enough: the sidebar row must read Running, not Idle.
    paneController->ingestOutput(QByteArrayLiteral("$ make\n"));
    QCOMPARE(monitor.stateFor(QStringLiteral("sess-1"), QStringLiteral("term-1")),
             static_cast<int>(AgentState::Running));
    QCOMPARE(sessionState(), static_cast<int>(SessionRowState::Running));
}

// MANDATORY (markSeen semantics): a terminal reaching idle_unseen puts the row
// in FinishedUnseen (the blue "unseen completion" badge). Once the user views
// the session, markSeen(dev) clears the monitor's per-session unseen flag and
// its unseenChanged signal re-drives rebuildRows(). The monitor still reports
// the terminal's raw agent state as IdleUnseen, so without the AppController
// downgrade the row would stay stuck in FinishedUnseen and the badge would
// never clear. rebuildRows() must downgrade IdleUnseen -> Idle for the row when
// hasUnseen(dev) is false, so the FinishedUnseen badge is cleared. The terminal
// itself remains Unloaded in this harness, which is neutral Idle rather than a
// claimed disconnect.
void TstAppController::markSeenClearsFinishedUnseenBadge()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    FakeTransport agentTransport;
    AgentStatusMonitor monitor;
    monitor.setTransport(&agentTransport);
    controller.setAgentMonitor(&monitor);

    controller.refresh();
    transport.deliver(listWithTerminalFrame(
        takeRequest(transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("G"), QStringLiteral("sess-1"), QStringLiteral("term-1")));

    SessionsModel* model = controller.sessionsModel();
    auto sessionState = [model]() {
        const QModelIndex group = model->index(0, 0);
        const QModelIndex session = model->index(0, 0, group);
        return model->data(session, SessionsModel::RowStateRole).toInt();
    };

    // idle_unseen -> row FinishedUnseen (badge shown), session flagged unseen.
    agentTransport.deliver(agentEventLine(QStringLiteral("idle_unseen"),
                                          QStringLiteral("sess-1"),
                                          QStringLiteral("term-1")));
    QCOMPARE(sessionState(),
             static_cast<int>(SessionRowState::FinishedUnseen));
    QVERIFY(monitor.hasUnseen(QStringLiteral("sess-1")));

    // markSeen(dev) -> rebuild -> badge cleared. The monitor still holds the
    // terminal at IdleUnseen, but the row must no longer be FinishedUnseen.
    monitor.markSeen(QStringLiteral("sess-1"));
    QVERIFY(!monitor.hasUnseen(QStringLiteral("sess-1")));
    QVERIFY(sessionState() != static_cast<int>(SessionRowState::FinishedUnseen));
    QCOMPARE(sessionState(),
             static_cast<int>(SessionRowState::Idle));
    // The monitor's per-terminal raw state is unchanged (only the row is
    // downgraded); this is what proves the fix lives in rebuildRows, not the
    // monitor.
    QCOMPARE(monitor.stateFor(QStringLiteral("sess-1"), QStringLiteral("term-1")),
             static_cast<int>(AgentState::IdleUnseen));
}

// Activation is a user-facing boundary, so a stale delegate must not load a
// layout for an id missing from the last authoritative tree or for an archived
// row hidden by the sidebar.
void TstAppController::activationRejectsUnknownOrArchivedSessions()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);

    controller.refresh();
    const QJsonObject request = takeRequest(transport);
    transport.deliver(listWithSessionFrame(
        request.value(QStringLiteral("id")).toInt(), QStringLiteral("g"),
        QStringLiteral("archived"), true));

    QSignalSpy activeSpy(&controller, &AppController::activeSessionChanged);
    controller.activateSession(QStringLiteral("missing"));
    controller.activateSession(QStringLiteral("archived"));

    QCOMPARE(controller.activeSessionId(), QString());
    QCOMPARE(activeSpy.count(), 0);
    QVERIFY(transport.takeSent().isEmpty());
}

// AppController's own UiStateStore is the REAL per-user QSettings (it is
// constructed with an empty ini path, exactly as main.cpp leaves it). The
// active-session cases below drive it for real, so redirect QSettings at the
// process level rather than writing into the developer's actual config.
void TstAppController::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

namespace {

// Shared setup for the active-session cases: a controller wired to a real
// SessionLayouts over its own WorkspaceDb (the main.cpp shape), serverId set
// BEFORE the transport so the setter's refresh() no-ops, with session "s1"
// active and BOTH region layouts loaded and editable.
struct ActiveSessionFixture {
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller{&client};
    SessionLayouts layouts{controller.workspaceDb(), controller.uiState()};

    ActiveSessionFixture()
    {
        controller.setConnection(nullptr, nullptr, nullptr, &layouts);
        controller.setServerId(QStringLiteral("srv"));
        // Hermetic: a previous run must not leave a remembered session behind.
        controller.uiState()->setActiveSession(QStringLiteral("srv"), QString());
        client.setTransport(&transport);
        transport.takeSent();
    }

    // Answer one workspace.list with a tree that holds group "g" + session "s1".
    void deliverTreeWithS1()
    {
        controller.refresh();
        transport.deliver(listWithTerminalFrame(
            takeRequest(transport).value(QStringLiteral("id")).toInt(),
            QStringLiteral("g"), QStringLiteral("s1"), QStringLiteral("t1")));
    }

    // Answer one workspace.list with group "g" and NO sessions: s1 is gone.
    void deliverTreeWithoutS1()
    {
        controller.refresh();
        transport.deliver(listResultFrame(
            takeRequest(transport).value(QStringLiteral("id")).toInt(),
            QStringLiteral("g")));
    }

    // Make s1 current and resolve both of its getLayout requests, so the
    // layouts are genuinely loaded (valid trees, canEdit() satisfied).
    void activateAndLoadS1()
    {
        controller.activateSession(QStringLiteral("s1"));
        const QVector<int> layoutIds = takeRequestIds(transport);
        for (int id : layoutIds)
            transport.deliver(layoutLeafFrame(id, QStringLiteral("viewer-1")));
    }
};

} // namespace

// The active Dev Session can be deleted out from under the shell by ANOTHER
// client: it simply stops appearing in the authoritative tree. Nothing else in
// the controller ever clears m_activeSessionId (restoreActiveSession only ever
// fills it in, and early-returns while it is non-empty), so before this it
// stayed "active" forever: the terminal region kept its dead devSessionId,
// SessionLayouts kept passing canEdit() and would keep writing
// workspace.setLayout rows for a Dev Session the server had deleted, and
// UiStateStore kept offering it to the next launch.
void TstAppController::vanishedActiveSessionIsRetiredEverywhere()
{
    ActiveSessionFixture f;
    f.deliverTreeWithS1();
    f.activateAndLoadS1();

    // Precondition: fully live. Layout edits for s1 are accepted and persisted.
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.layouts.devSessionId(), QStringLiteral("s1"));
    QVERIFY(!f.layouts.viewerTree().isNull());
    QVERIFY(!f.layouts.terminalTree().isNull());
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s1"));
    f.transport.takeSent();

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);
    f.deliverTreeWithoutS1();

    // Retired everywhere, and exactly once - not thrashed per refresh.
    QCOMPARE(f.controller.activeSessionId(), QString());
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(f.layouts.devSessionId(), QString());
    QVERIFY(f.layouts.viewerTree().isNull());
    QVERIFY(f.layouts.terminalTree().isNull());
    // Forgotten for THIS server only, so the next launch does not restore a
    // phantom; another server's memory is untouched.
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QString());

    // The corruption this guards: with no Dev Session selected, SessionLayouts
    // refuses the edit instead of writing setLayout under the dead id.
    QSignalSpy layoutErrorSpy(&f.layouts, &SessionLayouts::error);
    f.transport.takeSent();
    f.layouts.splitPane(QStringLiteral("viewer"), QStringLiteral("viewer-1"),
                        QStringLiteral("horizontal"));
    QCOMPARE(layoutErrorSpy.count(), 1);
    QVERIFY(f.transport.takeSent().isEmpty());

    // Idempotent: a second identical refresh is a no-op, no further signal.
    f.deliverTreeWithoutS1();
    QCOMPARE(activeSpy.count(), 1);
}

// The retirement above must fire ONLY on an authoritative answer. A superseded
// (stale-generation) list that happens to lack the session, and an RpcError -
// which means "we do not know", not "it is gone" - must both leave the active
// session completely alone. Getting this wrong turns a transient server hiccup
// into a silently closed Dev Session.
void TstAppController::staleOrFailedRefreshNeverRetiresActiveSession()
{
    ActiveSessionFixture f;
    f.deliverTreeWithS1();
    f.activateAndLoadS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    f.transport.takeSent();

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);

    // Two refreshes in flight; the OLDER one comes back without s1, after the
    // newer one already re-affirmed it. The generation guard must drop it
    // before it can retire anything.
    f.controller.refresh();
    const int staleId = takeRequest(f.transport).value(QStringLiteral("id")).toInt();
    f.controller.refresh();
    const int freshId = takeRequest(f.transport).value(QStringLiteral("id")).toInt();
    QVERIFY(staleId != freshId);
    f.transport.deliver(listWithTerminalFrame(freshId, QStringLiteral("g"),
                                              QStringLiteral("s1"),
                                              QStringLiteral("t1")));
    f.transport.deliver(listResultFrame(staleId, QStringLiteral("g")));

    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.layouts.devSessionId(), QStringLiteral("s1"));
    QCOMPARE(activeSpy.count(), 0);

    // An outright RPC failure is not evidence of deletion either.
    f.transport.takeSent();
    f.controller.refresh();
    f.transport.deliver(errorFrame(
        takeRequest(f.transport).value(QStringLiteral("id")).toInt(), -32000,
        QStringLiteral("workspace unavailable")));

    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.layouts.devSessionId(), QStringLiteral("s1"));
    QCOMPARE(activeSpy.count(), 0);
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s1"));
}

// The sidebar can hide rows two ways - "only pinned" and hiding archived ones -
// and both are presentation choices on THIS machine. Neither may change what the
// controller believes the server holds, because two decisions read that belief:
// whether the open Dev Session still exists, and how many sessions a group
// deletion would destroy. The refresh used to pass the pin filter to the server,
// so with the filter on an unpinned session looked deleted and a group looked
// smaller than it was.
void TstAppController::sidebarFiltersDoNotNarrowTheAuthoritativeTree()
{
    ActiveSessionFixture f;
    f.deliverTreeWithS1();
    f.activateAndLoadS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));

    // Hide everything: s1 is neither pinned nor shown as archived.
    f.controller.sessionsModel()->setPinnedOnly(true);
    f.controller.uiState()->setShowArchived(false);
    f.transport.takeSent();

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);
    f.controller.refresh();
    const QJsonObject listReq = takeRequest(f.transport);

    // The request itself must not carry a filter.
    const QJsonObject params =
        listReq.value(QStringLiteral("params")).toObject();
    QVERIFY2(!params.value(QStringLiteral("pinnedOnly")).toBool(),
             "the sidebar's pin filter must not be sent to the server");

    f.transport.deliver(listWithSessionFrame(
        listReq.value(QStringLiteral("id")).toInt(), QStringLiteral("g"),
        QStringLiteral("s1"), false, false));

    // The row is hidden from the sidebar...
    QCOMPARE(f.controller.sessionsModel()->rowCount(), 0);
    // ...but the session is still open, still loaded, and still remembered.
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.layouts.devSessionId(), QStringLiteral("s1"));
    QCOMPARE(activeSpy.count(), 0);
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s1"));
    // ...and a group deletion would still report the session it destroys.
    QCOMPARE(f.controller.sessionCountForGroup(QStringLiteral("g")), 1);
}

// The same retirement must happen on THIS client's own deletion, which reaches
// it by a different route: deleteSession chains refreshOnSuccess -> refresh(),
// so the tree that no longer holds the session is the chained one.
void TstAppController::deletingActiveSessionRetiresItThroughChainedRefresh()
{
    ActiveSessionFixture f;
    f.deliverTreeWithS1();
    f.activateAndLoadS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    f.transport.takeSent();

    f.controller.deleteSession(QStringLiteral("s1"));
    const int deleteId =
        takeRequest(f.transport).value(QStringLiteral("id")).toInt();
    const QJsonObject ack{{"jsonrpc", "2.0"}, {"id", deleteId}, {"result", true}};
    f.transport.deliver(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n');

    // The success chained a refresh; answer it with the post-delete tree.
    f.transport.deliver(listResultFrame(
        takeRequest(f.transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("g")));

    QCOMPARE(f.controller.activeSessionId(), QString());
    QCOMPARE(f.layouts.devSessionId(), QString());
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QString());
}
// Archiving an open session retires the active layout immediately after the
// server acknowledges the mutation. The follow-up authoritative refresh then
// hides the archived row without leaving panes or a remembered active id.
void TstAppController::archivingActiveSessionRetiresItThroughChainedRefresh()
{
    ActiveSessionFixture f;
    f.deliverTreeWithS1();
    f.activateAndLoadS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QVERIFY(!f.layouts.viewerTree().isNull());
    f.transport.takeSent();

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);
    f.controller.archiveSession(QStringLiteral("s1"));
    const QJsonObject archiveReq = takeRequest(f.transport);
    QCOMPARE(archiveReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.updateSession"));
    QCOMPARE(archiveReq.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("archived")).toBool(),
             true);
    const QJsonObject ack{{"jsonrpc", "2.0"},
                          {"id", archiveReq.value(QStringLiteral("id")).toInt()},
                          {"result", QJsonObject{{"id", "s1"},
                                                 {"name", "s1"},
                                                 {"archived", true}}}};
    f.transport.deliver(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n');

    QCOMPARE(f.controller.activeSessionId(), QString());
    QCOMPARE(f.layouts.devSessionId(), QString());
    QVERIFY(f.layouts.viewerTree().isNull());
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QString());
    QCOMPARE(activeSpy.count(), 1);

    const QJsonObject refreshReq = takeRequest(f.transport);
    f.transport.deliver(listWithSessionFrame(
        refreshReq.value(QStringLiteral("id")).toInt(), QStringLiteral("g"),
        QStringLiteral("s1"), true));
    QCOMPARE(f.controller.sessionsModel()->rowCount(), 0);
    QVERIFY(f.controller.sessionsModel()->hasSessions());
}

// Disconnect must land on a clear empty state, not a half-live shell still
// pointing at a Dev Session it can no longer reach. Leaving it active is not
// cosmetic: SessionLayouts keeps the devSessionId, keeps passing canEdit(),
// and a Split command after Disconnect mutates and republishes a tree whose
// setLayout fails - and since a reconnect never reloads a session that is
// still active, the next edit that DOES land writes that divergent tree over
// the real one. The session is unreachable, NOT gone, so unlike a deletion the
// remembered id must survive and be reopened by the reconnect.
void TstAppController::disconnectRetiresActiveSessionButStillRemembersIt()
{
    ActiveSessionFixture f;
    SshConnectionPool pool;
    AgentStatusMonitor monitor;
    SessionBootstrap bootstrap(&pool, &f.client, &monitor);
    f.controller.setConnection(&pool, &bootstrap, nullptr, &f.layouts);

    f.deliverTreeWithS1();
    f.activateAndLoadS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QVERIFY(!f.layouts.viewerTree().isNull());
    f.transport.takeSent();

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);
    f.controller.disconnectServer();

    QCOMPARE(f.controller.activeSessionId(), QString());
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(f.layouts.devSessionId(), QString());
    QVERIFY(f.layouts.viewerTree().isNull());
    QVERIFY(f.layouts.terminalTree().isNull());
    // The distinction from a deletion: the server still HAS this session, so
    // the next connect must reopen it.
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s1"));

    // A layout edit is now refused outright instead of diverging locally.
    QSignalSpy layoutErrorSpy(&f.layouts, &SessionLayouts::error);
    f.transport.takeSent();
    f.layouts.splitPane(QStringLiteral("viewer"), QStringLiteral("viewer-1"),
                        QStringLiteral("horizontal"));
    QCOMPARE(layoutErrorSpy.count(), 1);
    QVERIFY(f.transport.takeSent().isEmpty());

    // Reconnect: adoptServerIdentity re-drives refresh(), whose `refreshed`
    // runs restoreActiveSession. Because the id was cleared, it reopens s1 -
    // and reopening reloads BOTH regions from the server rather than reusing
    // the tree we were holding when the link went down.
    f.deliverTreeWithS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.layouts.devSessionId(), QStringLiteral("s1"));
    QCOMPARE(takeRequestIds(f.transport).size(), 2); // one getLayout per region
}

// refresh() with no transport bound is DOCUMENTED to return without asking
// anything. That is the whole cold-start experience: the shell comes up before
// the user has connected to any server, and a workspace.list issued there would
// fail instantly with a synthetic "no transport bound" and paint an error toast
// over an app that has done nothing wrong. Nothing may be sent, no error may be
// raised, and `refreshed` (which drives restoreActiveSession) must not fire on
// a tree that was never read.
void TstAppController::refreshWithoutATransportIsASilentNoOp()
{
    CodeharbordClient client;  // no setTransport()
    AppController controller(&client);
    QSignalSpy errorSpy(&controller, &AppController::error);
    QSignalSpy refreshedSpy(&controller, &AppController::refreshed);

    controller.refresh();
    // Setting a server id also drives a refresh; it must be just as quiet.
    controller.setServerId(QStringLiteral("srv-cold"));

    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(refreshedSpy.count(), 0);
    QCOMPARE(controller.sessionsModel()->rowCount(), 0);
}

// activeSessionRepoRoot is a Q_PROPERTY read off the cached workspace tree but
// notified by activeSessionChanged, which a plain refresh does not otherwise
// emit. Somebody editing the Dev Session's repository root (here or from
// another client) must therefore still re-notify, or every binding on it - the
// terminal region's working directory above all - stays pinned to the old path
// for the rest of the run. Equally, a refresh that changes nothing must NOT
// emit: activeSessionChanged tears down and rebuilds the panes bound to it.
void TstAppController::serverSideRepoRootChangeNotifiesActiveSession()
{
    ActiveSessionFixture f;

    f.controller.refresh();
    f.transport.deliver(listWithRepoRootFrame(
        takeRequest(f.transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("g"), QStringLiteral("s1"), QStringLiteral("/srv/old")));
    f.controller.activateSession(QStringLiteral("s1"));
    f.transport.takeSent();
    QCOMPARE(f.controller.activeSessionRepoRoot(), QStringLiteral("/srv/old"));

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);

    // Same tree again: nothing moved, so nothing may be notified.
    f.controller.refresh();
    f.transport.deliver(listWithRepoRootFrame(
        takeRequest(f.transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("g"), QStringLiteral("s1"), QStringLiteral("/srv/old")));
    QCOMPARE(activeSpy.count(), 0);

    // The repository root moved on the server.
    f.controller.refresh();
    f.transport.deliver(listWithRepoRootFrame(
        takeRequest(f.transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("g"), QStringLiteral("s1"), QStringLiteral("/srv/new")));
    QCOMPARE(f.controller.activeSessionRepoRoot(), QStringLiteral("/srv/new"));
    QCOMPARE(activeSpy.count(), 1);
    // Still active: a repo-root edit is not a deletion.
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
}

// A Dev Session belongs to exactly one server. Switching serverId must drop the
// active session and both region layout trees immediately - carrying them over
// would show the previous server's panes against the new server's workspace and
// pair the OLD devSessionId with the NEW serverId on the next layout write. But
// the session is not GONE, so the PREVIOUS server's memory of it must survive:
// switching back has to reopen it.
void TstAppController::switchingServerDropsTheActiveSessionButKeepsItRemembered()
{
    ActiveSessionFixture f;
    f.deliverTreeWithS1();
    f.activateAndLoadS1();
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QVERIFY(!f.layouts.viewerTree().isNull());
    f.transport.takeSent();

    // Hermetic: a previous run must not leave srv-2 remembering a session.
    f.controller.uiState()->setActiveSession(QStringLiteral("srv-2"), QString());

    QSignalSpy activeSpy(&f.controller, &AppController::activeSessionChanged);
    QSignalSpy serverSpy(&f.controller, &AppController::serverIdChanged);
    f.controller.setServerId(QStringLiteral("srv-2"));

    QCOMPARE(serverSpy.count(), 1);
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(f.controller.activeSessionId(), QString());
    QCOMPARE(f.layouts.devSessionId(), QString());
    QVERIFY(f.layouts.viewerTree().isNull());
    QVERIFY(f.layouts.terminalTree().isNull());
    // The layouts repository is re-keyed in lockstep, so a later setLayout
    // cannot land under the previous server's key.
    QCOMPARE(f.layouts.serverId(), QStringLiteral("srv-2"));
    // Forgotten for nobody: switching back must reopen s1.
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s1"));

    // The switch reloads the sidebar from the NEW server, and that tree has no
    // s1, so nothing is restored under srv-2.
    f.transport.deliver(listResultFrame(
        takeRequest(f.transport).value(QStringLiteral("id")).toInt(),
        QStringLiteral("other")));
    QCOMPARE(f.controller.activeSessionId(), QString());
}

// SPEC 12.1: an unknown host key gets a fingerprint prompt, which means the
// attempt in flight must be REFUSED (libssh is mid-handshake; a dialog there
// would re-enter the UI) and retried once the user answers. The retry must pin
// the very key that was shown, or the user is asked the same question forever.
void TstAppController::hostKeyPromptParksTheAttemptAndAcceptRetriesWithItPinned()
{
    ConnectFixture f;
    QVERIFY2(!f.pool.hostKeyCallback(),
             "nothing should be installed before a connect is started");

    QVector<SshConnectionPool::HostKeyDecision> decisions;
    f.boot.duringConnect = [&f, &decisions] {
        QVERIFY(f.pool.hostKeyCallback());
        decisions << offerUnknownHostKey(f.pool);
    };

    QSignalSpy promptSpy(&f.controller, &AppController::hostKeyPrompt);
    QSignalSpy errorSpy(&f.controller, &AppController::error);
    f.controller.connectToProfile(f.profileId);

    QCOMPARE(decisions.size(), 1);
    QVERIFY2(decisions.at(0) == SshConnectionPool::HostKeyDecision::Reject,
             "an unknown key was trusted without asking the user");
    QCOMPARE(promptSpy.count(), 1);
    QCOMPARE(promptSpy.at(0).at(0).toString(), QStringLiteral("box.example"));
    QCOMPARE(promptSpy.at(0).at(1).toString(), QStringLiteral("ssh-ed25519"));
    QCOMPARE(promptSpy.at(0).at(2).toString(), displayedHostKeyFingerprint());
    QCOMPARE(f.controller.connectionState(), QStringLiteral("hostkey"));
    // The refusal is the EXPECTED outcome, so it must not surface as a fault
    // while the app is simply waiting on the user's answer.
    QCOMPARE(errorSpy.count(), 0);

    // A parked prompt swallows a second Connect: starting one underneath it
    // would swap the pending profile/fingerprint out from under resolveHostKey().
    const int callsBefore = f.boot.connectCalls;
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(f.boot.connectCalls, callsBefore);
    QCOMPARE(promptSpy.count(), 1);
    QCOMPARE(f.controller.connectionState(), QStringLiteral("hostkey"));

    // Accept: exactly ONE retry, and this time the same key is trusted.
    f.controller.resolveHostKey(true);
    QCOMPARE(f.boot.connectCalls, callsBefore + 1);
    QCOMPARE(decisions.size(), 2);
    QVERIFY2(decisions.at(1) == SshConnectionPool::HostKeyDecision::Accept,
             "the accepted fingerprint was not pinned for the retry");
    // One question, one answer: no second prompt for the key just approved.
    QCOMPARE(promptSpy.count(), 1);
}

// Rejecting is an answer too: the parked attempt ends, nothing is retried, a
// stale sheet answering again cannot redial, and no approval is left armed for
// the next connect.
void TstAppController::rejectedHostKeyEndsTheAttemptAndLeavesNothingPinned()
{
    ConnectFixture f;
    QVector<SshConnectionPool::HostKeyDecision> decisions;
    f.boot.duringConnect = [&f, &decisions] {
        decisions << offerUnknownHostKey(f.pool);
    };
    QSignalSpy promptSpy(&f.controller, &AppController::hostKeyPrompt);
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 1);

    const int callsBefore = f.boot.connectCalls;
    f.controller.resolveHostKey(false);
    QCOMPARE(f.boot.connectCalls, callsBefore);  // no retry
    QCOMPARE(f.controller.connectionState(), QStringLiteral("disconnected"));

    // A stale sheet answering twice must not start anything.
    f.controller.resolveHostKey(true);
    QCOMPARE(f.boot.connectCalls, callsBefore);

    // The next real connect starts UNPINNED: the same key is refused and put to
    // the user again, proving the rejected fingerprint was not left armed.
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(f.boot.connectCalls, callsBefore + 1);
    QCOMPARE(decisions.size(), 2);
    QVERIFY2(decisions.at(1) == SshConnectionPool::HostKeyDecision::Reject,
             "a rejected fingerprint was still armed on the next connect");
    QCOMPARE(promptSpy.count(), 2);
}

// The two prompts chain: host key first, then credentials. An attempt that got
// as far as AUTH never reached the code that writes a newly trusted key to
// known_hosts, so the credential retry meets the SAME key as unknown all over
// again — and must trust it on the strength of the approval already given
// rather than asking a second time.
void TstAppController::hostKeyApprovalSurvivesTheCredentialRetryInTheSameChain()
{
    ConnectFixture f;
    QVector<SshConnectionPool::HostKeyDecision> decisions;
    f.boot.duringConnect = [&f, &decisions] {
        decisions << offerUnknownHostKey(f.pool);
    };
    QSignalSpy hostKeySpy(&f.controller, &AppController::hostKeyPrompt);
    QSignalSpy credentialSpy(&f.controller, &AppController::credentialPrompt);
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(hostKeySpy.count(), 1);

    // Attempt 2: the key is accepted, and libssh then asks to unlock the key.
    f.boot.duringConnect = [&f, &decisions] {
        decisions << offerUnknownHostKey(f.pool);
        f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
    };
    f.controller.resolveHostKey(true);
    QCOMPARE(decisions.size(), 2);
    QVERIFY(decisions.at(1) == SshConnectionPool::HostKeyDecision::Accept);
    QCOMPARE(credentialSpy.count(), 1);
    QCOMPARE(f.controller.connectionState(), QStringLiteral("credential"));

    // Attempt 3: the passphrase is in hand, and the key must still be trusted.
    f.boot.duringConnect = [&f, &decisions] {
        decisions << offerUnknownHostKey(f.pool);
    };
    f.controller.submitCredential(QStringLiteral("unlock-me"),
                                  QStringLiteral("keyPassphrase"));
    QCOMPARE(decisions.size(), 3);
    QVERIFY2(decisions.at(2) == SshConnectionPool::HostKeyDecision::Accept,
             "the host-key approval did not survive the credential retry, so "
             "the user would be asked to re-approve the same key");
    QCOMPARE(hostKeySpy.count(), 1);
}

// SshConnectionPool authenticates agent -> configured/default key -> requested
// credential. The controller must park the blocking libssh handshake and ask
// afterwards, with private-key passphrases and server passwords kept separate.
void TstAppController::credentialCallbackIsInstalledAndParksInsteadOfBlocking()
{
    ConnectFixture f;
    QVERIFY(!f.profileId.isEmpty());

    QVERIFY2(!f.pool.credentialCallback(),
             "nothing should be installed before a connect is started");

    bool asked = false;
    f.boot.duringConnect = [&f, &asked] {
        // Stand where SshConnectionPool::authenticate() requests a key unlock.
        QVERIFY(f.pool.credentialCallback());
        asked = true;
        const auto reply = f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
        QVERIFY(reply.secret.isEmpty());
        QVERIFY(reply.promptRequested);
    };

    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);
    QSignalSpy errorSpy(&f.controller, &AppController::error);
    f.controller.connectToProfile(f.profileId);

    QVERIFY(asked);
    QCOMPARE(promptSpy.count(), 1);
    QCOMPARE(promptSpy.at(0).at(0).toString(), QStringLiteral("yichen"));
    QCOMPARE(promptSpy.at(0).at(1).toString(), QStringLiteral("127.0.0.1"));
    QCOMPARE(promptSpy.at(0).at(2).toString(),
             QStringLiteral("Private-key passphrase"));
    QCOMPARE(promptSpy.at(0).at(3).toString(),
             QStringLiteral("keyPassphrase"));
    QCOMPARE(f.controller.connectionState(), QStringLiteral("credential"));
    QCOMPARE(f.boot.identityFile, QStringLiteral("/home/yichen/.ssh/id_ed25519"));
    // Being asked for a password is not a fault; an error toast here would tell
    // the user something broke while the app is simply waiting on them.
    QCOMPARE(errorSpy.count(), 0);
}

// A private-key passphrase has a local security boundary. If it cannot unlock
// the configured/default key, a later password attempt needs a second prompt;
// the original passphrase MUST NOT be sent to the SSH server as a password.
void TstAppController::passphraseIsNeverOfferedAsServerPassword()
{
    static const QString kPassphrase = QStringLiteral("private-key-only");
    ConnectFixture f;
    f.boot.duringConnect = [&f] {
        f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
    };
    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 1);

    QString keySecret;
    bool passwordPrompted = false;
    f.boot.duringConnect = [&f, &keySecret, &passwordPrompted] {
        const auto keyReply = f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
        keySecret = keyReply.secret;
        QVERIFY(!keyReply.promptRequested);

        // Simulate a rejected key unlock. The only legal next step is a fresh
        // password prompt, never a replay of keySecret as a server password.
        const auto passwordReply = f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::Password);
        QVERIFY(passwordReply.secret.isEmpty());
        passwordPrompted = passwordReply.promptRequested;
    };
    f.controller.submitCredential(kPassphrase, QStringLiteral("keyPassphrase"));

    QCOMPARE(keySecret, kPassphrase);
    QVERIFY(passwordPrompted);
    QCOMPARE(promptSpy.count(), 2);
    QCOMPARE(promptSpy.at(1).at(2).toString(), QStringLiteral("Password"));
    QCOMPARE(promptSpy.at(1).at(3).toString(), QStringLiteral("password"));
    QCOMPARE(f.controller.connectionState(), QStringLiteral("credential"));
}

// The secret is spent on exactly one attempt and then gone: not replayed by the
// reconnect ladder running through the same installed callback, not written to
// any file the app owns, and not logged. A passphrase landing in the config
// file would be a worse defect than the bug this fixes.
void TstAppController::submittedSecretIsSpentOnceAndNeverPersistedOrLogged()
{
    static const QString kSecret = QStringLiteral("correct-horse-battery-42");
    static QStringList captured;
    captured.clear();
    QtMessageHandler previous = qInstallMessageHandler(
        [](QtMsgType, const QMessageLogContext&, const QString& text) {
            captured << text;
        });
    const auto restoreHandler =
        qScopeGuard([previous] { qInstallMessageHandler(previous); });

    ConnectFixture f;
    f.boot.duringConnect = [&f] {
        f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
    };
    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 1);

    // The retry: this time the callback has the answer in hand.
    QStringList handedOver;
    f.boot.duringConnect = [&f, &handedOver] {
        const auto keyReply = f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
        QVERIFY(keyReply.secret.isEmpty());
        QVERIFY(!keyReply.promptRequested);
        handedOver << f.pool.credentialCallback()(
                          QStringLiteral("yichen"),
                          SshConnectionPool::CredentialKind::Password)
                          .secret;
    };
    QSignalSpy errorSpy(&f.controller, &AppController::error);
    f.controller.submitCredential(kSecret, QStringLiteral("password"));

    QCOMPARE(handedOver, QStringList{kSecret});

    // ONE SHOT. The callback outlives the attempt (SessionBootstrap's reconnect
    // ladder re-handshakes through it with nobody waiting), so a secret still
    // sitting in the capture would be replayed at whatever host it dials next.
    const auto spent = f.pool.credentialCallback()(
        QStringLiteral("yichen"), SshConnectionPool::CredentialKind::Password);
    QVERIFY(spent.secret.isEmpty());
    QVERIFY(!spent.promptRequested);
    // ...and that replay attempt must not arm a prompt nobody would answer.
    QCOMPARE(promptSpy.count(), 1);

    // A WRONG secret (connectOk stayed false) fails cleanly and says so, rather
    // than wedging the state machine or silently re-asking forever.
    QCOMPARE(f.controller.connectionState(), QStringLiteral("failed"));
    QCOMPARE(errorSpy.count(), 1);
    // Not wedged: the next connect really starts a new handshake.
    f.boot.duringConnect = {};
    f.boot.connectCalls = 0;
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(f.boot.connectCalls, 1);

    // Nothing the app persisted contains it — servers.ini above all, which is
    // the file a profile field would have landed in.
    const QByteArray persisted = f.allPersistedBytes();
    QVERIFY(!persisted.isEmpty());  // the profile store really was written
    QVERIFY2(!persisted.contains(kSecret.toUtf8()),
             "the secret reached a file on disk");
    // ...and no profile field carries it either, whatever the store looks like.
    const QVariantMap stored = f.profiles.profile(f.profileId);
    for (const QVariant& value : stored)
        QVERIFY(value.toString() != kSecret);

    for (const QString& line : captured)
        QVERIFY2(!line.contains(kSecret), qPrintable(line));
}

// Cancelling is an answer too: the parked attempt ends, nothing is retried, and
// the controller is left able to connect again.
void TstAppController::cancellingTheCredentialPromptAbandonsTheAttemptCleanly()
{
    ConnectFixture f;
    f.boot.duringConnect = [&f] {
        f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
    };
    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 1);

    const int callsBefore = f.boot.connectCalls;
    f.controller.submitCredential(QString());
    QCOMPARE(f.boot.connectCalls, callsBefore);  // no retry
    QCOMPARE(f.controller.connectionState(), QStringLiteral("disconnected"));

    // A stale sheet answering twice must not redial anything.
    f.controller.submitCredential(QStringLiteral("too-late"));
    QCOMPARE(f.boot.connectCalls, callsBefore);

    // And the next real connect is unaffected.
    f.boot.duringConnect = {};
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(f.boot.connectCalls, callsBefore + 1);
}

// A server with `AuthenticationMethods publickey,password` accepts the key and
// then demands a password as well. The client cannot satisfy that in one
// attempt: the passphrase is typed before anybody knows a password is also
// wanted. So the chain runs prompt -> retry -> prompt -> retry, and the LAST
// attempt has to arrive holding both secrets — the earlier passphrase included,
// because that attempt has to unlock the key all over again. Losing it is
// exactly how such a server became impossible to connect to.
void TstAppController::twoMethodServerChainCarriesBothSecretsWithoutCrossingThem()
{
    static const QString kPassphrase = QStringLiteral("unlock-the-key-9f");
    static const QString kPassword = QStringLiteral("the-account-password-3c");
    ConnectFixture f;
    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);

    // Attempt 1: the key is encrypted, so the pool asks for its passphrase and
    // the attempt is refused so the user can be asked.
    f.boot.duringConnect = [&f] {
        const auto reply = f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
        QVERIFY(reply.promptRequested);
    };
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 1);
    QCOMPARE(promptSpy.at(0).at(3).toString(), QStringLiteral("keyPassphrase"));

    // Attempt 2: the key unlocks and the server reports partial success, so the
    // pool goes on to the password rung — which must raise a SECOND prompt
    // rather than replaying the passphrase at it.
    QString passphraseSeen;
    QString passwordSeenTooEarly;
    f.boot.duringConnect = [&f, &passphraseSeen, &passwordSeenTooEarly] {
        const auto keyReply = f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
        passphraseSeen = keyReply.secret;
        const auto passwordReply = f.pool.credentialCallback()(
            QStringLiteral("yichen"), SshConnectionPool::CredentialKind::Password);
        passwordSeenTooEarly = passwordReply.secret;
        QVERIFY(passwordReply.promptRequested);
    };
    f.controller.submitCredential(kPassphrase, QStringLiteral("keyPassphrase"));
    QCOMPARE(passphraseSeen, kPassphrase);
    QVERIFY2(passwordSeenTooEarly.isEmpty(),
             "the private-key passphrase was offered to password auth");
    QCOMPARE(promptSpy.count(), 2);
    QCOMPARE(promptSpy.at(1).at(3).toString(), QStringLiteral("password"));
    QCOMPARE(f.controller.connectionState(), QStringLiteral("credential"));

    // Attempt 3, the one that used to be impossible: BOTH secrets are in hand,
    // and each is handed only to the method it belongs to.
    QString finalPassphrase;
    QString finalPassword;
    f.boot.duringConnect = [&f, &finalPassphrase, &finalPassword] {
        finalPassphrase = f.pool
                              .credentialCallback()(
                                  QStringLiteral("yichen"),
                                  SshConnectionPool::CredentialKind::KeyPassphrase)
                              .secret;
        finalPassword = f.pool
                            .credentialCallback()(
                                QStringLiteral("yichen"),
                                SshConnectionPool::CredentialKind::Password)
                            .secret;
    };
    f.controller.submitCredential(kPassword, QStringLiteral("password"));
    QCOMPARE(finalPassphrase, kPassphrase);
    QCOMPARE(finalPassword, kPassword);
    // No third question: both credential kinds have been answered in this chain,
    // so a rung that asks again must be met with silence instead of a prompt
    // loop the user can never get out of.
    QCOMPARE(promptSpy.count(), 2);

    // The chain is over (connectOk stayed false, so it failed) and both secrets
    // are gone with it: nothing may be replayed at the next host dialled.
    QCOMPARE(f.controller.connectionState(), QStringLiteral("failed"));
    const auto spentKey = f.pool.credentialCallback()(
        QStringLiteral("yichen"),
        SshConnectionPool::CredentialKind::KeyPassphrase);
    const auto spentPassword = f.pool.credentialCallback()(
        QStringLiteral("yichen"), SshConnectionPool::CredentialKind::Password);
    QVERIFY(spentKey.secret.isEmpty());
    QVERIFY(spentPassword.secret.isEmpty());
    QVERIFY(!spentKey.promptRequested);
    QVERIFY(!spentPassword.promptRequested);

    // Neither secret reached disk, and neither reached the SSH log the user can
    // open from the connection sheet.
    const QByteArray persisted = f.allPersistedBytes();
    QVERIFY(!persisted.contains(kPassphrase.toUtf8()));
    QVERIFY(!persisted.contains(kPassword.toUtf8()));
    QVERIFY(!f.controller.sshDiagnostics().contains(kPassphrase));
    QVERIFY(!f.controller.sshDiagnostics().contains(kPassword));

    // A fresh user-initiated connect starts a clean chain: the questions are
    // asked again rather than assumed already answered.
    f.boot.duringConnect = [&f] {
        const auto reply = f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
        QVERIFY(reply.promptRequested);
    };
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 3);
}

// A stray second click on Connect while a credential sheet is up must be
// REFUSED and must otherwise change nothing. The refusal was always there, but
// the state it was protecting was thrown away before the guard ever ran: the
// handler wiped the chain's gathered secrets and its "already asked" flags
// first, so a user who clicked Connect again while the password box was open
// lost the private-key passphrase they had already typed. The next attempt then
// arrived with the key locked again - and on a server that demands a key AND a
// password (OpenSSH `AuthenticationMethods publickey,password`) that chain can
// never complete, which is exactly the dead end the chain exists to avoid.
void TstAppController::connectWhileParkedLeavesTheChainsSecretsIntact()
{
    static const QString kPassphrase = QStringLiteral("unlock-the-key-aa");
    static const QString kPassword = QStringLiteral("account-password-bb");
    ConnectFixture f;
    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);

    // Attempt 1: the key is encrypted; ask for the passphrase.
    f.boot.duringConnect = [&f] {
        f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
    };
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 1);

    // Attempt 2: the passphrase unlocks the key, the server then wants a
    // password as well, so a SECOND prompt goes up. The passphrase now lives in
    // the chain, which is what the stray Connect below used to destroy.
    f.boot.duringConnect = [&f] {
        f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
        f.pool.credentialCallback()(QStringLiteral("yichen"),
                                    SshConnectionPool::CredentialKind::Password);
    };
    f.controller.submitCredential(kPassphrase, QStringLiteral("keyPassphrase"));
    QCOMPARE(promptSpy.count(), 2);
    QCOMPARE(f.controller.connectionState(), QStringLiteral("credential"));

    // THE STRAY CLICK. Nothing may be dialled and nothing may be forgotten.
    const int callsBefore = f.boot.connectCalls;
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(f.boot.connectCalls, callsBefore);
    QCOMPARE(promptSpy.count(), 2);
    QCOMPARE(f.controller.connectionState(), QStringLiteral("credential"));

    // Attempt 3 must still hold BOTH secrets, each offered only to its own
    // method, and must not ask a third question.
    QString finalPassphrase;
    QString finalPassword;
    f.boot.duringConnect = [&f, &finalPassphrase, &finalPassword] {
        finalPassphrase =
            f.pool
                .credentialCallback()(
                    QStringLiteral("yichen"),
                    SshConnectionPool::CredentialKind::KeyPassphrase)
                .secret;
        finalPassword =
            f.pool
                .credentialCallback()(QStringLiteral("yichen"),
                                      SshConnectionPool::CredentialKind::Password)
                .secret;
    };
    f.controller.submitCredential(kPassword, QStringLiteral("password"));

    QVERIFY2(finalPassphrase == kPassphrase,
             "a refused second Connect wiped the passphrase the chain had "
             "already gathered");
    QCOMPARE(finalPassword, kPassword);
    QCOMPARE(promptSpy.count(), 2);
}

// `kind` comes from QML. A value neither branch recognises is a bug on that
// side, and guessing at it would offer the typed secret to the wrong
// authentication method - a private-key passphrase sent to a remote host's
// password endpoint. The submission is dropped whole: nothing is dialled, the
// prompt stays parked, and the user's next (correct) answer still works.
void TstAppController::submitCredentialWithAnUnknownKindIsIgnored()
{
    ConnectFixture f;
    f.boot.duringConnect = [&f] {
        f.pool.credentialCallback()(
            QStringLiteral("yichen"),
            SshConnectionPool::CredentialKind::KeyPassphrase);
    };
    QSignalSpy promptSpy(&f.controller, &AppController::credentialPrompt);
    f.controller.connectToProfile(f.profileId);
    QCOMPARE(promptSpy.count(), 1);

    const int callsBefore = f.boot.connectCalls;
    f.controller.submitCredential(QStringLiteral("typed-by-the-user"),
                                  QStringLiteral("totp"));
    QCOMPARE(f.boot.connectCalls, callsBefore);            // nothing dialled
    QCOMPARE(f.controller.connectionState(),
             QStringLiteral("credential"));                // still parked

    // The prompt was not consumed: a well-formed answer still drives the retry
    // and still reaches only the method it was typed for.
    QString seenPassphrase;
    QString seenPassword;
    f.boot.duringConnect = [&f, &seenPassphrase, &seenPassword] {
        seenPassphrase =
            f.pool
                .credentialCallback()(
                    QStringLiteral("yichen"),
                    SshConnectionPool::CredentialKind::KeyPassphrase)
                .secret;
        seenPassword =
            f.pool
                .credentialCallback()(QStringLiteral("yichen"),
                                      SshConnectionPool::CredentialKind::Password)
                .secret;
    };
    f.controller.submitCredential(QStringLiteral("the-real-passphrase"),
                                  QStringLiteral("keyPassphrase"));
    QCOMPARE(f.boot.connectCalls, callsBefore + 1);
    QCOMPARE(seenPassphrase, QStringLiteral("the-real-passphrase"));
    QVERIFY(seenPassword.isEmpty());
}

// The server.info result's schemaVersion field was parsed and never checked. A client one
// release ahead of its codeharbord got an empty serverId, keyed the workspace to
// "", and showed an EMPTY SIDEBAR over a healthy SSH session with no
// explanation. Version skew is the default state under manual deployment.
void TstAppController::serverOlderThanTheSchemaFloorIsRefusedWithBothVersions()
{
    FakeTransport transport;
    ConnectFixture f;
    f.client.setTransport(&transport);
    QSignalSpy errorSpy(&f.controller, &AppController::error);

    f.boot.fireWired();
    const QJsonObject request = takeRequest(transport);
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QStringLiteral("server.info"));

    // schema 3: the release before serverId existed, so it reports none.
    transport.deliver(serverInfoFrame(request.value(QStringLiteral("id")).toInt(),
                                      3, QString(), QStringLiteral("0.0.9")));

    QCOMPARE(errorSpy.count(), 1);
    const QString message = errorSpy.at(0).at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("3")), qPrintable(message));
    QVERIFY2(message.contains(QString::number(
                 AppController::kMinimumServerSchemaVersion)),
             qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("0.0.9")), qPrintable(message));

    // Refused, not silently continued: no identity adopted, and crucially no
    // workspace.list for the empty serverId — that call IS the empty sidebar.
    QCOMPARE(f.controller.serverId(), QString());
    QVERIFY(takeRequestIds(transport).isEmpty());

    // The link is dropped rather than left half-alive, one event-loop turn
    // later (we were inside the client's own response callback).
    QTRY_COMPARE(f.controller.connectionState(), QStringLiteral("failed"));
    QCOMPARE(f.controller.connectionError(), message);
}

// Control: a server AT the floor is adopted and drives the sidebar as before,
// so the gate refuses old servers rather than all of them.
void TstAppController::serverAtTheSchemaFloorIsAdoptedNormally()
{
    FakeTransport transport;
    ConnectFixture f;
    f.client.setTransport(&transport);
    QSignalSpy errorSpy(&f.controller, &AppController::error);

    f.boot.fireWired();
    const QJsonObject request = takeRequest(transport);
    transport.deliver(
        serverInfoFrame(request.value(QStringLiteral("id")).toInt(),
                        AppController::kMinimumServerSchemaVersion,
                        QStringLiteral("srv-1"), QStringLiteral("1.0.0")));

    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(f.controller.serverId(), QStringLiteral("srv-1"));
    const QJsonObject listRequest = takeRequest(transport);
    QCOMPARE(listRequest.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));
}

// A settings file is plain text a human can edit and a crash can truncate. Every
// width read has to survive that, because QVariant::toInt() answers 0 for
// anything it cannot parse and 0 is not a width — it is a region that has
// vanished, with no handle left on screen to drag it back. The documented
// default is the only safe answer.
void TstAppController::uiStateStoreIgnoresCorruptWidths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("corrupt.ini"));

    {
        QSettings raw(iniPath, QSettings::IniFormat);
        raw.setValue(QStringLiteral("layout/sidebarWidth"), QStringLiteral("wide"));
        raw.setValue(QStringLiteral("layout/viewerWidth"), QStringLiteral("-40"));
        // A line the writer never finished.
        raw.setValue(QStringLiteral("layout/terminalWidth"), QString());
        raw.sync();
    }

    UiStateStore store(iniPath);
    QCOMPARE(store.sidebarWidth(), 260);
    QCOMPARE(store.viewerWidth(), 0);
    QCOMPARE(store.terminalWidth(), 520);

    // A FRACTIONAL width, on a region whose default is not 0 so the assertion
    // can tell the two failure modes apart: QVariant::toInt() answers 13 here
    // (it rounds), and a 13-pixel terminal region is not something the user can
    // drag back open. This line used to sit on layout/sidebarWidth immediately
    // after the "wide" above, silently replacing it, so neither spelling was
    // really being checked - "wide" was dead, and 12.5 was landing on the
    // sidebar, whose 260 default the -40/0 cases below already cover.
    {
        QSettings raw(iniPath, QSettings::IniFormat);
        raw.setValue(QStringLiteral("layout/terminalWidth"), 12.5);
        raw.sync();
    }

    UiStateStore fractional(iniPath);
    QCOMPARE(fractional.terminalWidth(), 520);

    // A zero is corrupt for the two regions whose width IS their presence on
    // screen, and legitimate for the viewer, where it means "fill the rest".
    {
        QSettings raw(iniPath, QSettings::IniFormat);
        raw.setValue(QStringLiteral("layout/sidebarWidth"), 0);
        raw.setValue(QStringLiteral("layout/terminalWidth"), -1);
        raw.setValue(QStringLiteral("layout/viewerWidth"), 0);
        raw.sync();
    }
    UiStateStore reread(iniPath);
    QCOMPARE(reread.sidebarWidth(), 260);
    QCOMPARE(reread.terminalWidth(), 520);
    QCOMPARE(reread.viewerWidth(), 0);

    // Honest values are still honoured, including deliberately narrow ones.
    {
        UiStateStore writer(iniPath);
        writer.setRegionWidths(1, 0, 3);
    }
    UiStateStore narrow(iniPath);
    QCOMPARE(narrow.sidebarWidth(), 1);
    QCOMPARE(narrow.terminalWidth(), 3);

    // Writes are normalised too: a transient layout value below the documented
    // minima must not be persisted only to be replaced by a different value on
    // the next launch.
    {
        UiStateStore writer(iniPath);
        writer.setRegionWidths(0, -4, 0);
    }
    UiStateStore normalised(iniPath);
    QCOMPARE(normalised.sidebarWidth(), 1);
    QCOMPARE(normalised.viewerWidth(), 0);
    QCOMPARE(normalised.terminalWidth(), 1);
}

// The two sidebar filters are booleans in the same hand-editable text file, and
// the wrong repair rule here is worse than the wrong number above: Qt's own
// QVariant::toBool() answers TRUE for any non-empty text, so a line reading
// `pinnedOnly=yes` would switch the pin filter on. With it on, every unpinned
// Dev Session disappears, and the user is looking at what seems to be an empty
// workspace with nothing on screen explaining why. Only the four documented
// spellings are accepted; anything else means "show everything".
void TstAppController::uiStateStoreRejectsUnreadableSidebarFilters()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("filters.ini"));

    const auto writeRaw = [&iniPath](const QString& spelling) {
        QSettings raw(iniPath, QSettings::IniFormat);
        raw.setValue(QStringLiteral("sidebar/pinnedOnly"), spelling);
        raw.setValue(QStringLiteral("sidebar/showArchived"), spelling);
        raw.sync();
    };

    // What IS accepted, including the textual forms an ini editor produces, a
    // capitalisation this client never writes, and surrounding whitespace.
    const QVector<QPair<QString, bool>> readable = {
        {QStringLiteral("true"), true},   {QStringLiteral("TRUE"), true},
        {QStringLiteral(" 1 "), true},    {QStringLiteral("false"), false},
        {QStringLiteral("0"), false},
    };
    for (const auto& spelling : readable) {
        writeRaw(spelling.first);
        UiStateStore store(iniPath);
        QVERIFY2(store.pinnedOnly() == spelling.second,
                 qPrintable(QStringLiteral("pinnedOnly=%1").arg(spelling.first)));
        QVERIFY2(store.showArchived() == spelling.second,
                 qPrintable(QStringLiteral("showArchived=%1").arg(spelling.first)));
    }

    // Everything else falls back to "show everything" - never to "hide things".
    const QStringList unreadable = {
        QStringLiteral("yes"), QStringLiteral("on"),   QStringLiteral("2"),
        QStringLiteral("-1"),  QStringLiteral("null"), QString(),
    };
    for (const QString& spelling : unreadable) {
        writeRaw(spelling);
        UiStateStore store(iniPath);
        QVERIFY2(!store.pinnedOnly(),
                 qPrintable(QStringLiteral("pinnedOnly=%1 must not hide rows")
                                .arg(spelling)));
        QVERIFY2(!store.showArchived(),
                 qPrintable(QStringLiteral("showArchived=%1 must not be trusted")
                                .arg(spelling)));
    }
}

// The empty devSessionId is not a Dev Session, and is treated exactly as
// setActiveSession() treats an empty serverId. Without the guard every write
// made before a session was picked landed on the bare "selectedPane/" key, and
// the next such read handed that stale pane id back as if it belonged to
// whatever the user opened next.
void TstAppController::uiStateStoreRejectsAnEmptyDevSessionId()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("emptyid.ini"));

    {
        UiStateStore store(iniPath);
        store.setSelectedPane(QString(), QStringLiteral("terminal-4"));
        QVERIFY(store.selectedPane(QString()).isEmpty());
        // A real session is unaffected by the neighbouring no-op.
        store.setSelectedPane(QStringLiteral("s1"), QStringLiteral("viewer-2"));
        QCOMPARE(store.selectedPane(QStringLiteral("s1")),
                 QStringLiteral("viewer-2"));
        // Empty and path-like regions are not addressable settings keys.
        store.setNextPaneSuffix(QStringLiteral("s1"), QString(), 9);
        store.setNextPaneSuffix(QStringLiteral("s1"),
                                QStringLiteral("viewer/evil"), 9);
        QCOMPARE(store.nextPaneSuffix(QStringLiteral("s1"), QString()), 1);
        QCOMPARE(store.nextPaneSuffix(QStringLiteral("s1"),
                                      QStringLiteral("viewer/evil")),
                 1);
        store.setNextPaneSuffix(QStringLiteral("s1"),
                                QStringLiteral("viewer"), 3);
        QCOMPARE(store.nextPaneSuffix(QStringLiteral("s1"),
                                      QStringLiteral("viewer")),
                 3);
        // The write side carries the same floor the read side enforces: a
        // counter stored below 1 would be repaired to 1 on the way out, which
        // silently forgets every id already spent and hands the next pane a
        // label a live pane is already wearing.
        store.setNextPaneSuffix(QStringLiteral("s1"),
                                QStringLiteral("viewer"), 0);
        store.setNextPaneSuffix(QStringLiteral("s1"),
                                QStringLiteral("viewer"), -7);
        QCOMPARE(store.nextPaneSuffix(QStringLiteral("s1"),
                                      QStringLiteral("viewer")),
                 3);
        // No Dev Session, no counter: the two regions of the NEXT session must
        // not inherit a number parked under the bare key.
        store.setNextPaneSuffix(QString(), QStringLiteral("viewer"), 11);
        QCOMPARE(store.nextPaneSuffix(QString(), QStringLiteral("viewer")), 1);
        QCOMPARE(store.nextPaneSuffix(QStringLiteral("s2"),
                                      QStringLiteral("viewer")),
                 1);
        // The same rule for the remembered session, scoped by server: an empty
        // serverId is the window before server.info has answered, and a value
        // parked there would be reopened against whichever server connects.
        store.setActiveSession(QString(), QStringLiteral("ghost"));
        QVERIFY(store.activeSession(QString()).isEmpty());
        store.setActiveSession(QStringLiteral("srv-a"), QStringLiteral("s1"));
        QCOMPARE(store.activeSession(QStringLiteral("srv-a")),
                 QStringLiteral("s1"));
        QVERIFY(store.activeSession(QStringLiteral("srv-b")).isEmpty());
    }

    UiStateStore reopened(iniPath);
    QVERIFY(reopened.selectedPane(QString()).isEmpty());
    QCOMPARE(reopened.selectedPane(QStringLiteral("s1")),
             QStringLiteral("viewer-2"));

    // Nothing was parked under the placeholder key on disk either.
    QFile file(iniPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString ini = QString::fromUtf8(file.readAll());
    QVERIFY2(!ini.contains(QStringLiteral("terminal-4")), qPrintable(ini));
}

// ServerProfiles serialises its saves against a second copy of the application
// with a lock file, and when it cannot take that lock it saves anyway and says
// so. "Says so" is only true if something is listening: an emitted signal with
// no connection is discarded in silence, and the user would be told nothing.
// This pins the whole path — blocked lock, degraded save, AppController::error,
// which is what Main.qml's toast shows.
void TstAppController::aDegradedProfileSaveReachesTheShellsErrorToast()
{
    ConnectFixture f;
    QSignalSpy errors(&f.controller, &AppController::error);

    // Hold the store's lock so the next save cannot have it. The suffix is the
    // store's own (NOT plain ".lock", which belongs to QSettings' sync).
    QLockFile blocker(f.dir.filePath(QStringLiteral("servers.ini.merge-lock")));
    QVERIFY(blocker.tryLock(5000));

    const QString id = f.profiles.addProfile(
        {{QStringLiteral("name"), QStringLiteral("second box")},
         {QStringLiteral("host"), QStringLiteral("10.0.0.9")},
         {QStringLiteral("port"), 22},
         {QStringLiteral("user"), QStringLiteral("yichen")}});
    blocker.unlock();

    // Saved, and reported exactly once.
    QVERIFY(!id.isEmpty());
    QCOMPARE(f.profiles.profile(id).value(QStringLiteral("host")).toString(),
             QStringLiteral("10.0.0.9"));
    QCOMPARE(errors.count(), 1);

    const QString shown = errors.first().first().toString();
    // Tone: it must read as a saved-but-unprotected notice. A message the user
    // reads as a failure gets the profile retyped, which is worse than silence.
    QVERIFY2(shown.startsWith(QStringLiteral("Server profile saved")), qPrintable(shown));
    QVERIFY2(shown.contains(QStringLiteral("safeguard")), qPrintable(shown));
    // ...carrying the cause ServerProfiles supplied, which names the holder.
    QVERIFY2(shown.contains(QString::number(QCoreApplication::applicationPid())),
             qPrintable(shown));
    // Nothing in it depends on rich text: the toast's Label is PlainText.
    QVERIFY2(!shown.contains(QLatin1Char('<')), qPrintable(shown));

    // A save that gets the lock is silent, and re-arms the report.
    f.profiles.updateProfile(id, {{QStringLiteral("host"), QStringLiteral("10.0.0.10")}});
    QCOMPARE(errors.count(), 1);

    // Re-injecting the same store must not stack a second connection, or every
    // future notice would appear twice.
    f.controller.setConnection(&f.pool, &f.boot, &f.profiles, nullptr);
    QVERIFY(blocker.tryLock(5000));
    f.profiles.updateProfile(id, {{QStringLiteral("host"), QStringLiteral("10.0.0.11")}});
    blocker.unlock();
    QCOMPARE(errors.count(), 2);
}

// A user-initiated Disconnect fails every in-flight RPC by design: the client
// answers each pending call with a synthetic "transport closed with request
// pending". reportIfError() exists to swallow exactly that window, because
// painting it red tells the user something broke when they are the one who
// pressed Disconnect. `server.info` was the lone call that bypassed the gate
// and emitted straight to the toast - and it is in flight for precisely that
// window, since adoptServerIdentity() issues it on every wire and reconnect.
void TstAppController::serverInfoFailureDuringDisconnectIsNotPaintedAsAFault()
{
    FakeTransport transport;
    ConnectFixture f;
    f.client.setTransport(&transport);

    // A failed connect leaves the bootstrap in State::Failed, so the teardown
    // below really does transition it (and emit) rather than no-op.
    f.controller.connectToProfile(f.profileId);

    // Now put a server.info on the wire and leave it unanswered.
    f.boot.fireWired();
    QCOMPARE(takeRequest(transport).value(QStringLiteral("method")).toString(),
             QStringLiteral("server.info"));

    // The real teardown drops the RPC channel device, which is what fails the
    // pending call. This fixture's transport is a plain QIODevice the bootstrap
    // does not own, so drop it at the same point in the same sequence.
    QObject::connect(&f.boot, &SessionBootstrap::stateChanged, &f.client,
                     [&f](SessionBootstrap::State state) {
                         if (state == SessionBootstrap::State::Disconnected)
                             f.client.setTransport(nullptr);
                     });

    QSignalSpy errorSpy(&f.controller, &AppController::error);
    f.controller.disconnectServer();

    // Named so a regression reads as "Disconnect showed the user an error"
    // rather than a bare count mismatch.
    const QString painted =
        errorSpy.isEmpty() ? QString() : errorSpy.at(0).at(0).toString();
    QVERIFY2(painted.isEmpty(), qPrintable(painted));
    QCOMPARE(f.controller.connectionState(), QStringLiteral("disconnected"));
}

// The workspace is keyed by the SERVER's own id. A server that answers
// server.info without one cannot be driven at all: adopting "" would key every
// group and session to nothing, which is the same permanently-empty sidebar
// over a healthy SSH session that the schema floor already refuses. It used to
// emit a toast and then leave the shell sitting there reporting "connected",
// with the toast gone a few seconds later and no way to tell what happened.
void TstAppController::serverThatReportsNoIdentityIsRefusedRatherThanLeftConnected()
{
    FakeTransport transport;
    ConnectFixture f;
    f.client.setTransport(&transport);
    QSignalSpy errorSpy(&f.controller, &AppController::error);

    f.boot.fireWired();
    const QJsonObject request = takeRequest(transport);
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QStringLiteral("server.info"));

    // Current schema, but no serverId field at all.
    transport.deliver(
        serverInfoFrame(request.value(QStringLiteral("id")).toInt(),
                        AppController::kMinimumServerSchemaVersion, QString(),
                        QStringLiteral("1.0.0")));

    QCOMPARE(errorSpy.count(), 1);
    const QString message = errorSpy.at(0).at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("identity")), qPrintable(message));

    // Nothing adopted, and crucially no workspace.list for the empty id - that
    // call IS the empty sidebar.
    QCOMPARE(f.controller.serverId(), QString());
    QVERIFY(takeRequestIds(transport).isEmpty());

    // Refused, not left half-alive: the link is dropped one event-loop turn
    // later (we were inside the client's own response callback) and the failure
    // is a durable property the connection footer can show, not a vanished
    // toast.
    QTRY_COMPARE(f.controller.connectionState(), QStringLiteral("failed"));
    QCOMPARE(f.controller.connectionError(), message);
}

// "Update server" has to do two things or it does nothing: arm the bootstrap's
// one-shot install request, and then actually dial. Arming without connecting
// leaves the request to ambush whatever the user connects to next; connecting
// without arming is an ordinary connect, which by design never replaces an
// installation the client did not make.
void TstAppController::upgradingTheRemoteServiceArmsTheBootstrapAndConnects()
{
    ConnectFixture f;
    f.boot.connectOk = true;
    bool armedAtDialTime = false;
    f.boot.duringConnect = [&f, &armedAtDialTime] {
        armedAtDialTime = f.boot.remoteUpgradeRequested();
    };

    f.controller.upgradeRemoteService(f.profileId);

    QCOMPARE(f.boot.connectCalls, 1);
    QVERIFY2(armedAtDialTime,
             "the connect ran without the upgrade request armed");
}

// With no profile chosen there is nothing to update, and dialling "" would
// report "No such server profile", which describes neither what the user did
// nor what to do next.
void TstAppController::anUpgradeWithNoServerChosenSaysSoInsteadOfDialling()
{
    ConnectFixture f;
    f.profiles.removeProfile(f.profileId);
    f.profiles.setActiveId(QString());
    QSignalSpy errorSpy(&f.controller, &AppController::error);

    f.controller.upgradeRemoteService(QString());

    QCOMPARE(f.boot.connectCalls, 0);
    QCOMPARE(errorSpy.count(), 1);
    const QString message = errorSpy.at(0).at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("Choose a server")),
             qPrintable(message));
}

// The failure mode this whole signal exists for. An upgrade that could not run
// is reported on an attempt that then CONNECTS, and AppController holds — then
// discards — the bootstrap's ordinary error() for exactly that case. Routed
// through error() instead, the user would be told nothing and would believe
// their server had been updated.
void TstAppController::anUpgradeThatDidNotHappenIsReportedEvenWhenTheConnectSucceeds()
{
    ConnectFixture f;
    f.boot.connectOk = true;
    QSignalSpy errorSpy(&f.controller, &AppController::error);
    // Emitted from inside the connect, which is where ensureRemoteService()
    // raises it: m_connecting is set, which is the state that swallows error().
    f.boot.duringConnect = [&f] {
        emit f.boot.upgradeFailed(QStringLiteral("nothing was changed here"));
    };

    f.controller.upgradeRemoteService(f.profileId);

    // Reported despite arriving while m_connecting is set, which is precisely
    // when the bootstrap's ordinary error() is held back. Anything the rest of
    // the attempt reports afterwards is a separate, later toast; this one is
    // FIRST and it is not the connection's.
    QVERIFY(!errorSpy.isEmpty());
    QCOMPARE(errorSpy.at(0).at(0).toString(),
             QStringLiteral("nothing was changed here"));
}

// Every way a connect chain can be abandoned has to spend the upgrade request
// with it. The request is a one-shot "replace the service on that server", it
// survives a PARKED prompt on purpose (the retry is the same user action), and
// so an abandonment that forgot to clear it would leave the next ordinary
// Connect — possibly to a different profile — reinstalling unasked.
void TstAppController::anAbandonedUpgradeDoesNotAmbushTheNextOrdinaryConnect()
{
    // (a) The user rejects the host key.
    {
        ConnectFixture f;
        f.boot.duringConnect = [&f] { offerUnknownHostKey(f.pool); };
        f.controller.upgradeRemoteService(f.profileId);
        QCOMPARE(f.controller.connectionState(), QStringLiteral("hostkey"));
        // Still armed while the answer is outstanding: the accept path retries
        // the same attempt, and that retry is still the upgrade.
        QVERIFY(f.boot.remoteUpgradeRequested());

        f.controller.resolveHostKey(false);
        QVERIFY2(!f.boot.remoteUpgradeRequested(),
                 "a rejected host key left the upgrade armed for the next connect");
    }

    // (b) The user cancels the credential prompt.
    {
        ConnectFixture f;
        f.boot.duringConnect = [&f] {
            f.pool.credentialCallback()(
                QStringLiteral("yichen"),
                SshConnectionPool::CredentialKind::Password);
        };
        f.controller.upgradeRemoteService(f.profileId);
        QCOMPARE(f.controller.connectionState(), QStringLiteral("credential"));
        QVERIFY(f.boot.remoteUpgradeRequested());

        f.controller.submitCredential(QString(), QStringLiteral("password"));
        QVERIFY2(!f.boot.remoteUpgradeRequested(),
                 "a cancelled credential prompt left the upgrade armed");
    }

    // (c) The chain simply fails, with no prompt to park on.
    {
        ConnectFixture f;
        f.boot.connectOk = false;
        f.controller.upgradeRemoteService(f.profileId);
        QCOMPARE(f.controller.connectionState(), QStringLiteral("failed"));
        QVERIFY2(!f.boot.remoteUpgradeRequested(),
                 "a failed upgrade chain left the request armed");
    }

    // (d) The user disconnects instead.
    {
        ConnectFixture f;
        f.boot.duringConnect = [&f] { offerUnknownHostKey(f.pool); };
        f.controller.upgradeRemoteService(f.profileId);
        QVERIFY(f.boot.remoteUpgradeRequested());
        f.controller.disconnectServer();
        QVERIFY2(!f.boot.remoteUpgradeRequested(),
                 "Disconnect left the upgrade armed");
    }

    // (e) The profile the chain was dialling is deleted from the connect sheet
    // while its host-key prompt is parked, and the user then accepts the key.
    // startConnect() finds nothing to dial and the chain ends right there -
    // which is still an abandonment, and used to be the one that kept the
    // upgrade armed, so the user's next ordinary Connect reinstalled the remote
    // service on a server they never asked to update.
    {
        ConnectFixture f;
        f.boot.duringConnect = [&f] { offerUnknownHostKey(f.pool); };
        f.controller.upgradeRemoteService(f.profileId);
        QCOMPARE(f.controller.connectionState(), QStringLiteral("hostkey"));
        QVERIFY(f.boot.remoteUpgradeRequested());

        f.profiles.removeProfile(f.profileId);
        f.controller.resolveHostKey(true);

        QCOMPARE(f.controller.connectionState(), QStringLiteral("failed"));
        QVERIFY2(!f.boot.remoteUpgradeRequested(),
                 "a chain whose profile vanished left the upgrade armed");

        // ...and the proof of what that costs: an ordinary connect to a
        // DIFFERENT server must reach the bootstrap with nothing armed.
        const QString other = f.profiles.addProfile(
            {{QStringLiteral("name"), QStringLiteral("other box")},
             {QStringLiteral("host"), QStringLiteral("10.0.0.4")},
             {QStringLiteral("port"), 22},
             {QStringLiteral("user"), QStringLiteral("yichen")},
             {QStringLiteral("nodePath"), QStringLiteral("/usr/bin/node")},
             {QStringLiteral("repoRoot"), QStringLiteral("/srv/codeharbor")}});
        QVERIFY(!other.isEmpty());
        bool armedAtDialTime = true;
        f.boot.connectOk = true;
        f.boot.duringConnect = [&f, &armedAtDialTime] {
            armedAtDialTime = f.boot.remoteUpgradeRequested();
        };
        f.controller.connectToProfile(other);
        QCOMPARE(f.boot.connectCalls, 2);
        QVERIFY2(!armedAtDialTime,
                 "the next ordinary connect dialled with an upgrade armed");
    }
}

// Both reorder mutations carry the user's drag result as a whole list. Nothing
// else in the payload says what moved, so an order the client rewrites - or a
// list it sends against the wrong key - silently rearranges the sidebar into
// something the user did not ask for the moment the authoritative tree comes
// back.
void TstAppController::reorderingSendsTheOrderedIdsInOrderAndRefreshes()
{
    FakeTransport transport;
    CodeharbordClient client;
    AppController controller(&client);
    client.setTransport(&transport);
    controller.setServerId(QStringLiteral("srv-1"));
    // setServerId() drives its own workspace.list; drain it so the assertions
    // below read the reorder request and nothing else.
    QCOMPARE(takeRequestIds(transport).size(), 1);

    controller.reorderGroups(
        QStringList{QStringLiteral("g2"), QStringLiteral("g1")});
    const QJsonObject groupReq = takeRequest(transport);
    QCOMPARE(groupReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.reorderGroups"));
    const QJsonObject groupParams =
        groupReq.value(QStringLiteral("params")).toObject();
    // Keyed by the CURRENT server: a reorder that went out under a stale id
    // would be applied to another server's groups, or to none at all.
    QCOMPARE(groupParams.value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-1"));
    const QJsonArray groupIds =
        groupParams.value(QStringLiteral("orderedIds")).toArray();
    QCOMPARE(groupIds.size(), 2);
    QCOMPARE(groupIds.at(0).toString(), QStringLiteral("g2"));
    QCOMPARE(groupIds.at(1).toString(), QStringLiteral("g1"));

    // Acknowledged, and the sidebar is re-read from authoritative state rather
    // than reordered locally.
    const QJsonObject groupAck{
        {"jsonrpc", "2.0"},
        {"id", groupReq.value(QStringLiteral("id")).toInt()},
        {"result", true}};
    transport.deliver(QJsonDocument(groupAck).toJson(QJsonDocument::Compact) + '\n');
    QCOMPARE(takeRequest(transport).value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.list"));

    controller.reorderSessions(
        QStringLiteral("g1"),
        QStringList{QStringLiteral("s3"), QStringLiteral("s1"),
                    QStringLiteral("s2")});
    const QJsonObject sessionReq = takeRequest(transport);
    QCOMPARE(sessionReq.value(QStringLiteral("method")).toString(),
             QStringLiteral("workspace.reorderSessions"));
    const QJsonObject sessionParams =
        sessionReq.value(QStringLiteral("params")).toObject();
    // Sessions are ordered within their GROUP, so this one carries a groupId
    // and no serverId.
    QCOMPARE(sessionParams.value(QStringLiteral("groupId")).toString(),
             QStringLiteral("g1"));
    const QJsonArray sessionIds =
        sessionParams.value(QStringLiteral("orderedIds")).toArray();
    QCOMPARE(sessionIds.size(), 3);
    QCOMPARE(sessionIds.at(0).toString(), QStringLiteral("s3"));
    QCOMPARE(sessionIds.at(1).toString(), QStringLiteral("s1"));
    QCOMPARE(sessionIds.at(2).toString(), QStringLiteral("s2"));

    // An empty list is still a well-formed request: it is what a group whose
    // last session was dragged out looks like, and it must not be turned into a
    // missing field the server would reject.
    controller.reorderSessions(QStringLiteral("g1"), QStringList{});
    const QJsonObject emptyParams = takeRequest(transport)
                                        .value(QStringLiteral("params"))
                                        .toObject();
    QVERIFY(emptyParams.contains(QStringLiteral("orderedIds")));
    QVERIFY(emptyParams.value(QStringLiteral("orderedIds")).toArray().isEmpty());
}

QTEST_GUILESS_MAIN(TstAppController)
#include "tst_appcontroller.moc"