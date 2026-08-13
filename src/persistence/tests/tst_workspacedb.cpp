#include "CodeharbordClient.h"
#include "RpcTypes.h"
#include "WorkspaceDb.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QVector>
#include <QtTest/QtTest>

#include <optional>
#include <type_traits>

using ch::CodeharbordClient;
using ch::CreateGroupParams;
using ch::Group;
using ch::GroupNode;
using ch::RpcError;
using ch::ServerId;
using ch::SplitNode;
using ch::WorkspaceDb;

namespace {

// Wire method names come from the shared contract (src/remote/RpcTypes.h,
// mirrored in remote/src/rpc-types.ts and gated by remote/test/rpc-mirror.test.ts)
// rather than being retyped here, so this suite cannot pass against a name the
// server no longer serves.
const auto kList = QString::fromLatin1(ch::rpc::kMethodWorkspaceList);
const auto kCreateGroup =
    QString::fromLatin1(ch::rpc::kMethodWorkspaceCreateGroup);
const auto kReorderGroups =
    QString::fromLatin1(ch::rpc::kMethodWorkspaceReorderGroups);
const auto kUpdateSession =
    QString::fromLatin1(ch::rpc::kMethodWorkspaceUpdateSession);
const auto kDuplicateSession =
    QString::fromLatin1(ch::rpc::kMethodWorkspaceDuplicateSession);
const auto kGetLayout = QString::fromLatin1(ch::rpc::kMethodWorkspaceGetLayout);
const auto kSetLayout = QString::fromLatin1(ch::rpc::kMethodWorkspaceSetLayout);

// Serialize a JSON-RPC object as one framed (newline-terminated) wire line.
QByteArray jsonLine(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace

// Exercises the client-side WorkspaceDb repository against a QLocalSocket pair
// (no live server): the CodeharbordClient transport is one socket, the test
// reads the request the repository emits and writes canned JSON-RPC responses on
// the other. One best-effort test drives the real Node codeharbord service.
class TstWorkspaceDb : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void listParsesNestedTree();
    void createGroupSerializesParams();
    void errorResponseDeliversRpcError();
    void getLayoutNullDeliversNullopt();
    void createGroupOmitsUnsetOptionals();
    void updateSessionSerializesPartialWithFalse();
    void setLayoutSerializesInlineTree();
    void duplicateSessionParsesNestedNode();
    void reorderGroupsSerializesIdArray();
    void getLayoutMalformedTreeDeliversNullopt();
    void listSkipsNonObjectEntries();
    void schemaVersionMatchesTheRemoteSchema();
    void malformedResultsDeliverRpcErrorInsteadOfBlankRecords();
    void deleteSessionSerializesIdAndForwardsOutcome();
    void reorderSessionsSerializesGroupIdAndIdArray();
    void moveSessionToGroupSerializesParams();
    void createTerminalPaneSerializesOptionalsAndParsesRecord();
    void resolveTerminalPaneSendsTheSlotAddressAndParsesTheRow();
    void resolveTerminalPaneAddressesARowByIdWithoutItsLabel();
    void setLayoutMalformedTreeDeliversRpcError();
    void resolveTerminalPaneWithNoAddressFailsWithoutAskingTheServer();
    void setLayoutRefusesATreeItCouldNotReadBack();
    void workspaceDbCannotBeBuiltWithoutAClient();
    void updateOptionalsOmitUnsetAndSendEmptyStringsVerbatim();
    void pinnedStateCrossesTheWireInBothDirections();
    void updateGroupSerializesPartialWithFalse();
    void viewerPaneMethodsSerializeParamsAndForwardOutcomes();
    void liveCreateAndListOverProcess();

private:
    void makePair();
    QJsonObject readRequest();

    QLocalServer* m_server = nullptr;
    QLocalSocket* m_clientSide = nullptr; // transport bound to the client
    QLocalSocket* m_serverSide = nullptr; // test reads/writes canned frames here
    CodeharbordClient* m_client = nullptr;
    WorkspaceDb* m_db = nullptr;
    static int s_seq;
};

int TstWorkspaceDb::s_seq = 0;

void TstWorkspaceDb::init()
{
    m_client = new CodeharbordClient;
    m_db = new WorkspaceDb(m_client);
}

void TstWorkspaceDb::cleanup()
{
    delete m_db;
    m_db = nullptr;
    // Delete the client next so it disconnects from the transport cleanly.
    delete m_client;
    m_client = nullptr;
    delete m_serverSide;
    m_serverSide = nullptr;
    delete m_clientSide;
    m_clientSide = nullptr;
    delete m_server;
    m_server = nullptr;
}

void TstWorkspaceDb::makePair()
{
    const QString name = QStringLiteral("ch_ws_test_%1_%2")
                             .arg(QCoreApplication::applicationPid())
                             .arg(++s_seq);
    QLocalServer::removeServer(name);

    m_server = new QLocalServer;
    QVERIFY(m_server->listen(name));

    m_clientSide = new QLocalSocket;
    m_clientSide->connectToServer(name);
    QVERIFY(m_clientSide->waitForConnected(2000));
    QVERIFY(m_server->waitForNewConnection(2000));
    m_serverSide = m_server->nextPendingConnection();
    QVERIFY(m_serverSide != nullptr);

    m_client->setTransport(m_clientSide);
}

// Read exactly one JSONL request frame that the repository just wrote through
// the client transport. Flushes the client side first because CodeharbordClient
// buffers its write and this test drives no event loop between call and read.
//
// Cannot use QVERIFY: that macro returns from its enclosing function, which
// here would mean handing the caller an empty object anyway. So a timeout is
// reported as a warning instead of vanishing - without it, a repository that
// never sends its request fails later and elsewhere, as a callback that simply
// never fires.
QJsonObject TstWorkspaceDb::readRequest()
{
    m_clientSide->flush();
    QByteArray buffer;
    QDeadlineTimer deadline(2000);
    while (!buffer.contains('\n') && !deadline.hasExpired()) {
        if (m_serverSide->bytesAvailable() > 0 || m_serverSide->waitForReadyRead(100))
            buffer += m_serverSide->readAll();
    }
    const qsizetype newline = buffer.indexOf('\n');
    if (newline < 0) {
        qWarning("readRequest: no complete request frame arrived within 2s (%lld bytes read)",
                 static_cast<long long>(buffer.size()));
        return {};
    }
    return QJsonDocument::fromJson(buffer.left(newline)).object();
}

void TstWorkspaceDb::listParsesNestedTree()
{
    makePair();

    QVector<GroupNode> got;
    std::optional<RpcError> err;
    bool fired = false;
    m_db->list(ServerId{QStringLiteral("srv-1")},
               [&](QVector<GroupNode> groups, std::optional<RpcError> e) {
                   got = groups;
                   err = e;
                   fired = true;
               });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             kList);
    QCOMPARE(req.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-1"));
    const int id = req.value(QStringLiteral("id")).toInt();

    // A canned workspace.list result mirroring the real server (camelCase, with
    // an inline viewer split-tree object and a null terminal layout).
    const char* kListJson = R"([
      {
        "id": "g1", "serverId": "srv-1", "name": "Alpha",
        "position": 0, "collapsed": false,
        "sessions": [
          {
            "id": "s1", "serverId": "srv-1", "groupId": "g1", "name": "S1",
            "repositoryRoot": "/home/me/repo",
            "defaultWorkingDirectory": "/home/me/repo/sub",
            "taskDescription": "do things", "position": 0, "archived": false,
            "viewerPanes": [
              { "id": "v1", "serverId": "srv-1", "devSessionId": "s1",
                "url": "http://x", "handler": "web", "title": "T", "position": 0 }
            ],
            "terminalPanes": [
              { "id": "t1", "serverId": "srv-1", "devSessionId": "s1",
                "name": "term", "workingDirectory": "/wd", "tmuxTarget": "ch_x",
                "startupCommand": "vi", "harness": "bash", "position": 0 }
            ],
            "layouts": {
              "viewer": { "type": "leaf", "paneId": "v1" },
              "terminal": null
            }
          }
        ]
      }
    ])";
    const QJsonArray result = QJsonDocument::fromJson(kListJson).array();
    QCOMPARE(result.size(), 1); // guard against a malformed canned fixture

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());

    // Group: name + serverId mapped from camelCase into the typed wrapper.
    QCOMPARE(got.size(), 1);
    QCOMPARE(got[0].group.name, QStringLiteral("Alpha"));
    QCOMPARE(got[0].group.serverId.value, QStringLiteral("srv-1"));

    // Nested session with its repository root.
    QCOMPARE(got[0].sessions.size(), 1);
    const ch::SessionNode& node = got[0].sessions[0];
    QCOMPARE(node.session.repositoryRoot, QStringLiteral("/home/me/repo"));
    QCOMPARE(node.session.groupId.value, QStringLiteral("g1"));

    // A viewer pane url.
    QCOMPARE(node.viewerPanes.size(), 1);
    QCOMPARE(node.viewerPanes[0].url, QStringLiteral("http://x"));
    QCOMPARE(node.terminalPanes.size(), 1);
    QCOMPARE(node.terminalPanes[0].tmuxTarget, QStringLiteral("ch_x"));

    // Layout SplitNode decoded via SplitNode::fromJson; terminal layout absent.
    QVERIFY(node.viewerLayout.has_value());
    QVERIFY(node.viewerLayout->isLeaf());
    QCOMPARE(node.viewerLayout->paneId, QStringLiteral("v1"));
    const SplitNode expectedLeaf = SplitNode::fromJson(
        QJsonObject{{"type", "leaf"}, {"paneId", "v1"}});
    QCOMPARE(*node.viewerLayout, expectedLeaf);
    QVERIFY(!node.terminalLayout.has_value());
}

void TstWorkspaceDb::createGroupSerializesParams()
{
    makePair();

    std::optional<Group> group;
    bool fired = false;
    m_db->createGroup(
        CreateGroupParams{.serverId = ServerId{QStringLiteral("srv-1")},
                          .name = QStringLiteral("Beta"),
                          .position = 3,
                          .collapsed = true},
        [&](std::optional<Group> g, std::optional<RpcError>) {
            group = g;
            fired = true;
        });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("jsonrpc")).toString(),
             QStringLiteral("2.0"));
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             kCreateGroup);
    QVERIFY(req.contains(QStringLiteral("id")));

    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-1"));
    QCOMPARE(params.value(QStringLiteral("name")).toString(),
             QStringLiteral("Beta"));
    QCOMPARE(params.value(QStringLiteral("position")).toInt(), 3);
    QCOMPARE(params.value(QStringLiteral("collapsed")).toBool(), true);
    QVERIFY(!fired); // no response written yet

    // Deliver the created group and confirm it maps back into a typed Group.
    const int id = req.value(QStringLiteral("id")).toInt();
    const QJsonObject created{{"id", "gNew"}, {"serverId", "srv-1"},
                              {"name", "Beta"}, {"position", 3},
                              {"collapsed", true}};
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", created}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QVERIFY(group.has_value());
    QCOMPARE(group->id.value, QStringLiteral("gNew"));
    QCOMPARE(group->collapsed, true);
}

void TstWorkspaceDb::errorResponseDeliversRpcError()
{
    makePair();

    std::optional<Group> group;
    std::optional<RpcError> got;
    bool fired = false;
    m_db->createGroup(
        CreateGroupParams{.serverId = ServerId{QStringLiteral("srv-1")},
                          .name = QStringLiteral("X")},
        [&](std::optional<Group> g, std::optional<RpcError> e) {
            group = g;
            got = e;
            fired = true;
        });

    const QJsonObject req = readRequest();
    const int id = req.value(QStringLiteral("id")).toInt();

    const QJsonObject error{{"code", -32602}, {"message", "bad params"}};
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"error", error}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value());
    QCOMPARE(got->code, -32602);
    QCOMPARE(got->message, QStringLiteral("bad params"));
    QVERIFY(!group.has_value()); // no partial result alongside an error
}

void TstWorkspaceDb::getLayoutNullDeliversNullopt()
{
    makePair();

    bool hasLayout = true;
    std::optional<RpcError> err;
    bool fired = false;
    m_db->getLayout(
        ch::DevSessionId{QStringLiteral("s1")}, ch::Region::Viewer,
        [&](std::optional<SplitNode> tree, std::optional<RpcError> e) {
            hasLayout = tree.has_value();
            err = e;
            fired = true;
        });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             kGetLayout);
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("devSessionId")).toString(),
             QStringLiteral("s1"));
    QCOMPARE(params.value(QStringLiteral("region")).toString(),
             QStringLiteral("viewer"));
    const int id = req.value(QStringLiteral("id")).toInt();

    // The server returns JSON null when the region has no persisted layout;
    // the client must decode that to std::nullopt, not a default-constructed
    // (empty-leaf) SplitNode.
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonValue(QJsonValue::Null)}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QVERIFY(!hasLayout);
}

void TstWorkspaceDb::createGroupOmitsUnsetOptionals()
{
    makePair();

    bool fired = false;
    m_db->createGroup(
        CreateGroupParams{.serverId = ServerId{QStringLiteral("srv-1")},
                          .name = QStringLiteral("Bare")},
        [&](std::optional<Group>, std::optional<RpcError>) { fired = true; });

    const QJsonObject req = readRequest();
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("name")).toString(),
             QStringLiteral("Bare"));
    // Unset optionals are ABSENT on the wire (not null/default) so the server
    // applies its own defaults (SPEC 11.2).
    QVERIFY(!params.contains(QStringLiteral("position")));
    QVERIFY(!params.contains(QStringLiteral("collapsed")));

    const int id = req.value(QStringLiteral("id")).toInt();
    const QJsonObject created{{"id", "g"}, {"serverId", "srv-1"},
                              {"name", "Bare"}, {"position", 0},
                              {"collapsed", false}};
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", created}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
}

void TstWorkspaceDb::updateSessionSerializesPartialWithFalse()
{
    makePair();

    bool fired = false;
    m_db->updateSession(
        ch::UpdateSessionParams{.id = ch::DevSessionId{QStringLiteral("s1")},
                                .name = QStringLiteral("renamed"),
                                .archived = false},
        [&](std::optional<ch::DevSession>, std::optional<RpcError>) {
            fired = true;
        });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             kUpdateSession);
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("id")).toString(),
             QStringLiteral("s1"));
    QCOMPARE(params.value(QStringLiteral("name")).toString(),
             QStringLiteral("renamed"));
    // A set optional<bool> of false is transmitted (present + false), never
    // dropped: omission keys on has_value, not on the contained value.
    QVERIFY(params.contains(QStringLiteral("archived")));
    QCOMPARE(params.value(QStringLiteral("archived")).toBool(), false);
    // Untouched optionals stay absent.
    QVERIFY(!params.contains(QStringLiteral("repositoryRoot")));
    QVERIFY(!params.contains(QStringLiteral("defaultWorkingDirectory")));
    QVERIFY(!params.contains(QStringLiteral("taskDescription")));
    QVERIFY(!params.contains(QStringLiteral("position")));

    const int id = req.value(QStringLiteral("id")).toInt();
    const QJsonObject updated{{"id", "s1"}, {"serverId", "srv-1"},
                              {"groupId", "g1"}, {"name", "renamed"},
                              {"repositoryRoot", "/r"},
                              {"defaultWorkingDirectory",
                               QJsonValue(QJsonValue::Null)},
                              {"taskDescription", QJsonValue(QJsonValue::Null)},
                              {"position", 0}, {"archived", false}};
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", updated}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
}

void TstWorkspaceDb::setLayoutSerializesInlineTree()
{
    makePair();

    SplitNode leafA;
    leafA.paneId = QStringLiteral("pA");
    SplitNode leafB;
    leafB.paneId = QStringLiteral("pB");
    SplitNode split;
    split.orientation = ch::SplitOrientation::Vertical;
    split.children = {leafA, leafB};
    split.ratios = {0.5, 0.5};

    bool fired = false;
    std::optional<SplitNode> got;
    m_db->setLayout(
        ServerId{QStringLiteral("srv-1")},
        ch::DevSessionId{QStringLiteral("s1")}, ch::Region::Terminal, split,
        [&](std::optional<SplitNode> tree, std::optional<RpcError>) {
            got = tree;
            fired = true;
        });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             kSetLayout);
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-1"));
    QCOMPARE(params.value(QStringLiteral("devSessionId")).toString(),
             QStringLiteral("s1"));
    QCOMPARE(params.value(QStringLiteral("region")).toString(),
             QStringLiteral("terminal"));
    // The tree is an inline split-tree object mirroring SplitNode::tryToJson.
    const QJsonObject treeObj = params.value(QStringLiteral("tree")).toObject();
    QCOMPARE(treeObj.value(QStringLiteral("type")).toString(),
             QStringLiteral("split"));
    QCOMPARE(treeObj.value(QStringLiteral("orientation")).toString(),
             QStringLiteral("vertical"));
    QCOMPARE(treeObj.value(QStringLiteral("children")).toArray().size(), 2);

    // The server echoes a full SessionLayout row; the client extracts .tree.
    const int id = req.value(QStringLiteral("id")).toInt();
    const QJsonObject row{{"id", "L1"}, {"serverId", "srv-1"},
                          {"devSessionId", "s1"}, {"region", "terminal"},
                          {"tree", treeObj}, {"createdAt", 1},
                          {"updatedAt", 1}};
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", row}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(got.has_value());
    QVERIFY(!got->isLeaf());
    QCOMPARE(got->children.size(), 2);
    QCOMPARE(*got, split);
}

void TstWorkspaceDb::duplicateSessionParsesNestedNode()
{
    makePair();

    bool fired = false;
    std::optional<ch::SessionNode> node;
    std::optional<RpcError> err;
    m_db->duplicateSession(
        ch::DevSessionId{QStringLiteral("s1")},
        [&](std::optional<ch::SessionNode> n, std::optional<RpcError> e) {
            node = n;
            err = e;
            fired = true;
        });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             kDuplicateSession);
    QCOMPARE(req.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("id")).toString(),
             QStringLiteral("s1"));
    const int id = req.value(QStringLiteral("id")).toInt();

    // A canned duplicateSession result: a fresh session with a copied viewer
    // pane, a terminal pane bearing a fresh tmux target, and a remapped layout
    // per region (camelCase, inline layout trees).
    const char* kNodeJson = R"({
      "id": "s2", "serverId": "srv-1", "groupId": "g1", "name": "copy",
      "repositoryRoot": "/r", "defaultWorkingDirectory": null,
      "taskDescription": null, "position": 1, "archived": false,
      "viewerPanes": [
        { "id": "v2", "serverId": "srv-1", "devSessionId": "s2",
          "url": "http://a", "handler": null, "title": null, "position": 0 }
      ],
      "terminalPanes": [
        { "id": "t2", "serverId": "srv-1", "devSessionId": "s2", "name": "sh",
          "workingDirectory": null, "tmuxTarget": "ch_s2_t2",
          "startupCommand": null, "harness": null, "position": 0 }
      ],
      "layouts": {
        "viewer": { "type": "leaf", "paneId": "v2" },
        "terminal": { "type": "leaf", "paneId": "t2" }
      }
    })";
    const QJsonObject node0 = QJsonDocument::fromJson(kNodeJson).object();
    QVERIFY(!node0.isEmpty()); // guard against a malformed canned fixture
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", node0}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QVERIFY(node.has_value());
    QCOMPARE(node->session.id.value, QStringLiteral("s2"));
    QCOMPARE(node->session.name, QStringLiteral("copy"));
    QCOMPARE(node->viewerPanes.size(), 1);
    QCOMPARE(node->terminalPanes.size(), 1);
    // A fresh tmux target rides through the nested parse (terminal region).
    QCOMPARE(node->terminalPanes[0].tmuxTarget, QStringLiteral("ch_s2_t2"));
    QVERIFY(node->viewerLayout.has_value());
    QCOMPARE(node->viewerLayout->paneId, QStringLiteral("v2"));
    QVERIFY(node->terminalLayout.has_value());
    QCOMPARE(node->terminalLayout->paneId, QStringLiteral("t2"));

    // Documented intentional narrowing (WorkspaceDb.cpp): the fixture's null
    // nullable-text columns decode to an empty QString, since the ch:: model
    // types them as plain QString, not std::optional. Null and "" are
    // indistinguishable on the client by design.
    QVERIFY(node->session.defaultWorkingDirectory.isEmpty());
    QVERIFY(node->session.taskDescription.isEmpty());
    QVERIFY(node->viewerPanes[0].handler.isEmpty());
    QVERIFY(node->viewerPanes[0].title.isEmpty());
    QVERIFY(node->terminalPanes[0].workingDirectory.isEmpty());
    QVERIFY(node->terminalPanes[0].startupCommand.isEmpty());
    QVERIFY(node->terminalPanes[0].harness.isEmpty());
}

void TstWorkspaceDb::reorderGroupsSerializesIdArray()
{
    makePair();

    bool fired = false;
    m_db->reorderGroups(
        ServerId{QStringLiteral("srv-1")},
        {ch::GroupId{QStringLiteral("g3")}, ch::GroupId{QStringLiteral("g1")},
         ch::GroupId{QStringLiteral("g2")}},
        [&](std::optional<RpcError>) { fired = true; });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             kReorderGroups);
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-1"));
    const QJsonArray ids =
        params.value(QStringLiteral("orderedIds")).toArray();
    QCOMPARE(ids.size(), 3);
    QCOMPARE(ids.at(0).toString(), QStringLiteral("g3"));
    QCOMPARE(ids.at(1).toString(), QStringLiteral("g1"));
    QCOMPARE(ids.at(2).toString(), QStringLiteral("g2"));

    const int rid = req.value(QStringLiteral("id")).toInt();
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", rid},
         {"result", QJsonObject{{"ok", true}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
}

void TstWorkspaceDb::liveCreateAndListOverProcess()
{
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty())
        QSKIP("node not on PATH");

    QTemporaryDir dbDir;
    QVERIFY(dbDir.isValid());

    QProcess proc;
    QProcessEnvironment envp = QProcessEnvironment::systemEnvironment();
    envp.insert(QStringLiteral("CODEHARBOR_DB"),
                dbDir.filePath(QStringLiteral("ws.db")));
    proc.setProcessEnvironment(envp);
    proc.setProgram(node);
    proc.setArguments({QStringLiteral(CH_REPO_ROOT "/remote/src/codeharbord.ts"),
                       QStringLiteral("rpc"), QStringLiteral("--stdio")});
    proc.start();
    if (!proc.waitForStarted(3000))
        QSKIP("failed to start node codeharbord");

    // Unbind and reap on EVERY exit path, not just the happy one. A failing
    // QVERIFY below returns from this function immediately: `proc` would then
    // be destroyed while the client still held it as its transport, and
    // cleanup() would delete the client onto that dangling device. Unbinding
    // also fails every still-pending request synchronously, so the callbacks
    // below - which capture this frame's locals by reference - all run while
    // that frame is still alive.
    const auto teardown = qScopeGuard([&] {
        m_client->setTransport(nullptr);
        proc.closeWriteChannel();
        proc.terminate();
        if (!proc.waitForFinished(2000))
            proc.kill();
    });

    m_client->setTransport(&proc);

    // Create a group against a fresh temp database...
    std::optional<Group> created;
    std::optional<RpcError> createErr;
    bool createFired = false;
    m_db->createGroup(
        CreateGroupParams{.serverId = ServerId{QStringLiteral("srv-live")},
                          .name = QStringLiteral("Live")},
        [&](std::optional<Group> g, std::optional<RpcError> e) {
            created = g;
            createErr = e;
            createFired = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(createFired, 8000);
    QVERIFY(!createErr.has_value());
    QVERIFY(created.has_value());
    QCOMPARE(created->name, QStringLiteral("Live"));
    QCOMPARE(created->serverId.value, QStringLiteral("srv-live"));

    // ...then list it back through the same repository.
    QVector<GroupNode> listed;
    std::optional<RpcError> listErr;
    bool listFired = false;
    m_db->list(ServerId{QStringLiteral("srv-live")},
               [&](QVector<GroupNode> groups, std::optional<RpcError> e) {
                   listed = groups;
                   listErr = e;
                   listFired = true;
               });
    QTRY_VERIFY_WITH_TIMEOUT(listFired, 8000);
    QVERIFY(!listErr.has_value());
    QCOMPARE(listed.size(), 1);
    QCOMPARE(listed[0].group.name, QStringLiteral("Live"));
}

// A layout row whose tree fails SplitNode validation must arrive as "no layout",
// not as a valid-looking single blank pane: SessionLayouts leaves a region null
// when its layout cannot be loaded, precisely so the user is never invited to
// edit a fabricated layout over the real one the server still holds. A tree that
// genuinely IS an empty leaf (which is what closing a region's last pane
// persists) must still arrive as a present layout.
void TstWorkspaceDb::getLayoutMalformedTreeDeliversNullopt()
{
    makePair();

    bool fired = false;
    bool hasLayout = false;
    std::optional<RpcError> err;

    // Ask for the viewer layout, then answer with a SessionLayout row carrying
    // `tree`. Leaves the three locals above holding the callback's verdict.
    const auto requestLayout = [&](const QJsonObject& tree) {
        fired = false;
        hasLayout = false;
        err.reset();
        m_db->getLayout(ch::DevSessionId{QStringLiteral("s1")}, ch::Region::Viewer,
                        [&](std::optional<SplitNode> got, std::optional<RpcError> e) {
                            hasLayout = got.has_value();
                            err = e;
                            fired = true;
                        });
        const QJsonObject req = readRequest();
        const QJsonObject row{
            {"id", "L1"},
            {"tree", tree},
        };
        m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                      {"id", req.value(QStringLiteral("id")).toInt()},
                                      {"result", row}}));
        m_serverSide->flush();
    };

    // Two children but only one ratio: rejected by SplitNode::fromJson.
    const QJsonObject leafA{{"type", "leaf"}, {"paneId", "pA"}};
    const QJsonObject leafB{{"type", "leaf"}, {"paneId", "pB"}};
    requestLayout(QJsonObject{{"type", "split"},
                              {"orientation", "vertical"},
                              {"children", QJsonArray{leafA, leafB}},
                              {"ratios", QJsonArray{1.0}}});
    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QVERIFY(!hasLayout);

    // An unknown node type is likewise rejected.
    requestLayout(QJsonObject{{"type", "gadget"}});
    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QVERIFY(!hasLayout);

    // A stored empty leaf is a legitimate layout and must survive.
    requestLayout(QJsonObject{{"type", "leaf"}, {"paneId", ""}});
    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QVERIFY(hasLayout);
}

// A non-object element anywhere in the nested workspace.list result is dropped
// rather than decoded into a fully-default record. Appending one would put an
// entry with an empty id into the sidebar and into every id-keyed map built from
// it, where it looks like a real group/session/pane.
void TstWorkspaceDb::listSkipsNonObjectEntries()
{
    makePair();

    QVector<GroupNode> got;
    bool fired = false;
    std::optional<RpcError> err;
    m_db->list(ServerId{QStringLiteral("srv-1")},
               [&](QVector<GroupNode> groups, std::optional<RpcError> e) {
                   got = groups;
                   err = e;
                   fired = true;
               });

    const QJsonObject req = readRequest();
    const int id = req.value(QStringLiteral("id")).toInt();

    const char* kJson = R"([
      7,
      {},
      {
        "id": "g1", "serverId": "srv-1", "name": "Alpha",
        "position": 0, "collapsed": false,
        "sessions": [
          null,
          {},
          {
            "id": "s-bad", "viewerPanes": {}, "terminalPanes": []
          },
          {
            "id": "s1", "serverId": "srv-1", "groupId": "g1", "name": "S1",
            "repositoryRoot": "/r", "position": 0, "archived": false,
            "viewerPanes": ["nonsense", {},
              { "id": "v1", "serverId": "srv-1", "devSessionId": "s1",
                "url": "http://x", "position": 0 }
            ],
            "terminalPanes": [[], {},
              { "id": "t1", "serverId": "srv-1", "devSessionId": "s1",
                "name": "term", "position": 0 }
            ],
            "layouts": { "viewer": null, "terminal": null }
          }
        ]
      }
    ])";
    const QJsonArray result = QJsonDocument::fromJson(kJson).array();
    QCOMPARE(result.size(), 3); // guard against a malformed canned fixture

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QCOMPARE(got.size(), 1);
    QCOMPARE(got[0].group.id.value, QStringLiteral("g1"));
    QCOMPARE(got[0].sessions.size(), 1);
    QCOMPARE(got[0].sessions[0].session.id.value, QStringLiteral("s1"));
    QCOMPARE(got[0].sessions[0].viewerPanes.size(), 1);
    QCOMPARE(got[0].sessions[0].viewerPanes[0].id.value, QStringLiteral("v1"));
    QCOMPARE(got[0].sessions[0].terminalPanes.size(), 1);
    QCOMPARE(got[0].sessions[0].terminalPanes[0].id.value, QStringLiteral("t1"));
}

// WorkspaceDb::kSchemaVersion documents itself as being kept "in lockstep with
// remote/sql/schema.sql and WORKSPACE_SCHEMA_VERSION". Nothing enforced that:
// remote/test/schema.test.ts pins the two TypeScript/SQL sides to each other but
// cannot see the C++ constant, so a server-side bump could land while the client
// still advertises the old version. Read both sibling sources and compare.
void TstWorkspaceDb::schemaVersionMatchesTheRemoteSchema()
{
    QFile schema(QStringLiteral(CH_REPO_ROOT "/remote/sql/schema.sql"));
    QVERIFY2(schema.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(schema.fileName()));
    const QString sql = QString::fromUtf8(schema.readAll());
    const QRegularExpressionMatch seed =
        QRegularExpression(
            QStringLiteral("INSERT\\s+OR\\s+IGNORE\\s+INTO\\s+schema_version[^;]*?"
                           "VALUES\\s*\\(\\s*1\\s*,\\s*(\\d+)\\s*\\)"),
            QRegularExpression::CaseInsensitiveOption)
            .match(sql);
    QVERIFY2(seed.hasMatch(), "schema.sql no longer seeds schema_version as expected");
    QCOMPARE(seed.captured(1).toInt(), WorkspaceDb::kSchemaVersion);

    QFile module(QStringLiteral(CH_REPO_ROOT "/remote/src/workspace.ts"));
    QVERIFY2(module.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(module.fileName()));
    const QString ts = QString::fromUtf8(module.readAll());
    const QRegularExpressionMatch declared =
        QRegularExpression(
            QStringLiteral("WORKSPACE_SCHEMA_VERSION\\s*=\\s*(\\d+)"))
            .match(ts);
    QVERIFY2(declared.hasMatch(),
             "workspace.ts no longer declares WORKSPACE_SCHEMA_VERSION as expected");
    QCOMPARE(declared.captured(1).toInt(), WorkspaceDb::kSchemaVersion);
}

// A response that carries no `error` still says nothing about the SHAPE of its
// result. QJsonValue::toObject()/toArray() turn anything of the wrong kind into
// an EMPTY object/array, and the parse helpers map an empty object to a
// fully-DEFAULT record whose id is "". Handing that to the caller as a SUCCESS
// is what puts a nameless, empty-id group into the sidebar and into every
// id-keyed map built from it. Every workspace method must instead report the
// wrong-kind result as a failure. getLayout is the single exception: JSON null
// is its documented "no layout here", so null stays a success with no layout,
// while a non-null non-object is still a failure.
void TstWorkspaceDb::malformedResultsDeliverRpcErrorInsteadOfBlankRecords()
{
    makePair();

    // Answer the next request with `result`, then report what the callback saw.
    const auto respond = [&](const QJsonValue& result) {
        const QJsonObject req = readRequest();
        m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                      {"id", req.value(QStringLiteral("id")).toInt()},
                                      {"result", result}}));
        m_serverSide->flush();
    };

    // A record method (createGroup) answered with an array instead of an object.
    std::optional<Group> group;
    std::optional<RpcError> groupErr;
    bool groupFired = false;
    m_db->createGroup(
        CreateGroupParams{.serverId = ServerId{QStringLiteral("srv-1")},
                          .name = QStringLiteral("Beta")},
        [&](std::optional<Group> g, std::optional<RpcError> e) {
            group = g;
            groupErr = e;
            groupFired = true;
        });
    respond(QJsonArray{});
    QTRY_VERIFY(groupFired);
    QVERIFY(!group.has_value()); // never a blank Group with an empty id
    QVERIFY(groupErr.has_value());
    QCOMPARE(groupErr->code, -32603); // JSON-RPC reserved "internal error"
    QVERIFY(groupErr->message.contains(QStringLiteral("workspace.createGroup")));

    // The same for a record method answered with JSON null.
    std::optional<ch::TerminalPane> pane;
    std::optional<RpcError> paneErr;
    bool paneFired = false;
    m_db->createTerminalPane(
        ch::CreateTerminalPaneParams{.serverId = ServerId{QStringLiteral("srv-1")},
                                     .devSessionId = ch::DevSessionId{QStringLiteral("s1")},
                                     .name = QStringLiteral("sh")},
        [&](std::optional<ch::TerminalPane> p, std::optional<RpcError> e) {
            pane = p;
            paneErr = e;
            paneFired = true;
        });
    respond(QJsonValue(QJsonValue::Null));
    QTRY_VERIFY(paneFired);
    QVERIFY(!pane.has_value());
    QVERIFY(paneErr.has_value());

    // An object with no identity is also malformed; accepting it would create
    // a blank TerminalPane despite passing the JSON-kind check.
    pane.reset();
    paneErr.reset();
    paneFired = false;
    m_db->createTerminalPane(
        ch::CreateTerminalPaneParams{.serverId = ServerId{QStringLiteral("srv-1")},
                                     .devSessionId = ch::DevSessionId{QStringLiteral("s1")},
                                     .name = QStringLiteral("sh")},
        [&](std::optional<ch::TerminalPane> p, std::optional<RpcError> e) {
            pane = p;
            paneErr = e;
            paneFired = true;
        });
    respond(QJsonObject{});
    QTRY_VERIFY(paneFired);
    QVERIFY(!pane.has_value());
    QVERIFY(paneErr.has_value());
    QCOMPARE(paneErr->code, -32603);

    // list() answered with an object instead of an array: an empty sidebar
    // reported as a successful load is indistinguishable from a server that
    // really has no groups, so it must fail instead.
    QVector<GroupNode> listed;
    std::optional<RpcError> listErr;
    bool listFired = false;
    m_db->list(ServerId{QStringLiteral("srv-1")},
               [&](QVector<GroupNode> groups, std::optional<RpcError> e) {
                   listed = groups;
                   listErr = e;
                   listFired = true;
               });
    respond(QJsonObject{{"nonsense", true}});
    QTRY_VERIFY(listFired);
    QVERIFY(listed.isEmpty());
    QVERIFY(listErr.has_value());
    QVERIFY(listErr->message.contains(QStringLiteral("workspace.list")));

    // getLayout answered with a number: not the documented null, so a failure.
    bool layoutFired = false;
    bool hasLayout = true;
    std::optional<RpcError> layoutErr;
    m_db->getLayout(ch::DevSessionId{QStringLiteral("s1")}, ch::Region::Viewer,
                    [&](std::optional<SplitNode> tree, std::optional<RpcError> e) {
                        hasLayout = tree.has_value();
                        layoutErr = e;
                        layoutFired = true;
                    });
    respond(QJsonValue(7));
    QTRY_VERIFY(layoutFired);
    QVERIFY(!hasLayout);
    QVERIFY(layoutErr.has_value());

    // ...whereas null remains a plain "this region has no layout" success. (The
    // same case as getLayoutNullDeliversNullopt, repeated here so the boundary
    // between the two verdicts is asserted in one place.)
    layoutFired = false;
    hasLayout = true;
    layoutErr.reset();
    m_db->getLayout(ch::DevSessionId{QStringLiteral("s1")}, ch::Region::Viewer,
                    [&](std::optional<SplitNode> tree, std::optional<RpcError> e) {
                        hasLayout = tree.has_value();
                        layoutErr = e;
                        layoutFired = true;
                    });
    respond(QJsonValue(QJsonValue::Null));
    QTRY_VERIFY(layoutFired);
    QVERIFY(!hasLayout);
    QVERIFY(!layoutErr.has_value());
}

// The delete methods that can destroy terminal panes report WHICH remote tmux
// sessions went with them (remote/src/rpc-types.ts DeleteWithTmuxTargetsResult),
// because after the delete nothing else can name them. Nothing covered one, so
// neither the parameter shape, nor that the targets reach the caller, nor that
// a server error does, was pinned for this whole family.
void TstWorkspaceDb::deleteSessionSerializesIdAndForwardsOutcome()
{
    makePair();

    QStringList targets;
    std::optional<RpcError> err;
    bool fired = false;
    m_db->deleteSession(ch::DevSessionId{QStringLiteral("s1")},
                        [&](QStringList t, std::optional<RpcError> e) {
                            targets = t;
                            err = e;
                            fired = true;
                        });

    QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceDeleteSession));
    QCOMPARE(req.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("id")).toString(),
             QStringLiteral("s1"));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"ok", true},
                                {"tmuxTargets",
                                 QJsonArray{QStringLiteral("ch_s1_a"),
                                            QStringLiteral("ch_s1_b")}}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QCOMPARE(targets, QStringList({QStringLiteral("ch_s1_a"),
                                   QStringLiteral("ch_s1_b")}));

    // A `codeharbord` too old to report the field answers with `ok` alone. That
    // decodes to "nothing to kill" rather than to a guess: the client has no
    // way to name a tmux session the server did not hand it (SPEC 5.2).
    targets = QStringList{QStringLiteral("stale")};
    fired = false;
    m_db->deleteSession(ch::DevSessionId{QStringLiteral("s2")},
                        [&](QStringList t, std::optional<RpcError> e) {
                            targets = t;
                            err = e;
                            fired = true;
                        });
    req = readRequest();
    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", req.value(QStringLiteral("id")).toInt()},
                                  {"result", QJsonObject{{"ok", true}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QVERIFY(targets.isEmpty());

    // A server-side failure is forwarded verbatim rather than swallowed.
    err.reset();
    targets = QStringList{QStringLiteral("stale")};
    fired = false;
    m_db->deleteSession(ch::DevSessionId{QStringLiteral("gone")},
                        [&](QStringList t, std::optional<RpcError> e) {
                            targets = t;
                            err = e;
                            fired = true;
                        });
    req = readRequest();
    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"error", QJsonObject{{"code", -32602}, {"message", "no such session"}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QVERIFY(err.has_value());
    QCOMPARE(err->code, -32602);
    QCOMPARE(err->message, QStringLiteral("no such session"));
    // A failed delete destroyed nothing, so it reports nothing to kill.
    QVERIFY(targets.isEmpty());

    // Shapes the client does NOT understand are reported, not read as "success,
    // nothing to kill". That reading is the dangerous one: it looks exactly like
    // a clean delete of a session with no terminals, so the sidebar refreshes as
    // though the rows are gone while any tmux session behind that response keeps
    // running with nobody told. `{ok: true}` alone stays legal (an older server,
    // covered above); everything here does not.
    const QVector<QJsonValue> rejected{
        QJsonValue(true),                                     // a bare boolean
        QJsonValue(QJsonArray{}),                             // not an object
        QJsonValue(QJsonObject{}),                            // no `ok` at all
        QJsonValue(QJsonObject{{"ok", false}}),               // `ok` says no
        QJsonValue(QJsonObject{{"ok", true}, {"tmuxTargets", 7}}),
        QJsonValue(QJsonObject{{"ok", true},
                               {"tmuxTargets", QJsonArray{7}}}),
    };
    for (const QJsonValue& result : rejected) {
        err.reset();
        targets = QStringList{QStringLiteral("stale")};
        fired = false;
        m_db->deleteSession(ch::DevSessionId{QStringLiteral("s3")},
                            [&](QStringList t, std::optional<RpcError> e) {
                                targets = t;
                                err = e;
                                fired = true;
                            });
        req = readRequest();
        m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                      {"id", req.value(QStringLiteral("id")).toInt()},
                                      {"result", result}}));
        m_serverSide->flush();
        QTRY_VERIFY(fired);
        QVERIFY2(err.has_value(),
                 qPrintable(QStringLiteral("accepted a malformed delete result: %1")
                                .arg(QString::fromUtf8(
                                    QJsonDocument(QJsonArray{result}).toJson(
                                        QJsonDocument::Compact)))));
        QVERIFY(err->message.contains(QStringLiteral("malformed")));
        QVERIFY(targets.isEmpty());
    }
}

// reorderSessions is keyed by groupId, not serverId (unlike reorderGroups), and
// the order of the id array IS the new sidebar order. Sending the wrong key or
// losing the order silently reshuffles the user's sessions.
void TstWorkspaceDb::reorderSessionsSerializesGroupIdAndIdArray()
{
    makePair();

    bool fired = false;
    m_db->reorderSessions(ch::GroupId{QStringLiteral("g1")},
                          {ch::DevSessionId{QStringLiteral("s2")},
                           ch::DevSessionId{QStringLiteral("s3")},
                           ch::DevSessionId{QStringLiteral("s1")}},
                          [&](std::optional<RpcError>) { fired = true; });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceReorderSessions));
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("groupId")).toString(), QStringLiteral("g1"));
    QVERIFY(!params.contains(QStringLiteral("serverId")));
    const QJsonArray ids = params.value(QStringLiteral("orderedIds")).toArray();
    QCOMPARE(ids.size(), 3);
    QCOMPARE(ids.at(0).toString(), QStringLiteral("s2"));
    QCOMPARE(ids.at(1).toString(), QStringLiteral("s3"));
    QCOMPARE(ids.at(2).toString(), QStringLiteral("s1"));

    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", req.value(QStringLiteral("id")).toInt()},
                                  {"result", QJsonObject{{"ok", true}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
}

// moveSessionToGroup carries the DESTINATION group id as a required field and an
// optional insertion position, and answers with the moved session. Untested.
void TstWorkspaceDb::moveSessionToGroupSerializesParams()
{
    makePair();

    std::optional<ch::DevSession> moved;
    bool fired = false;
    m_db->moveSessionToGroup(
        ch::MoveSessionParams{.id = ch::DevSessionId{QStringLiteral("s1")},
                              .groupId = ch::GroupId{QStringLiteral("g2")},
                              .position = 0},
        [&](std::optional<ch::DevSession> s, std::optional<RpcError>) {
            moved = s;
            fired = true;
        });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceMoveSessionToGroup));
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("id")).toString(), QStringLiteral("s1"));
    QCOMPARE(params.value(QStringLiteral("groupId")).toString(), QStringLiteral("g2"));
    // position 0 is a SET optional and must survive: omission keys on has_value,
    // not on the contained value, and 0 means "first" rather than "unspecified".
    QVERIFY(params.contains(QStringLiteral("position")));
    QCOMPARE(params.value(QStringLiteral("position")).toInt(), 0);

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "s1"}, {"serverId", "srv-1"},
                                {"groupId", "g2"}, {"name", "S1"},
                                {"repositoryRoot", "/r"}, {"position", 0},
                                {"archived", false}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QVERIFY(moved.has_value());
    QCOMPARE(moved->groupId.value, QStringLiteral("g2"));
    QCOMPARE(moved->position, 0);
}

// The terminal-pane family has the widest optional set (working directory, tmux
// target, startup command, harness) and none of it was covered: every unset
// optional must be ABSENT on the wire so the server keeps its own default, and
// the full record must decode back, including the tmux target that makes a
// terminal reattach to the right remote session after a disconnect.
void TstWorkspaceDb::createTerminalPaneSerializesOptionalsAndParsesRecord()
{
    makePair();

    std::optional<ch::TerminalPane> created;
    bool fired = false;
    m_db->createTerminalPane(
        ch::CreateTerminalPaneParams{
            .serverId = ServerId{QStringLiteral("srv-1")},
            .devSessionId = ch::DevSessionId{QStringLiteral("s1")},
            .name = QStringLiteral("shell"),
            .workingDirectory = QStringLiteral("/home/me/repo"),
            .harness = QStringLiteral("oh-my-pi")},
        [&](std::optional<ch::TerminalPane> p, std::optional<RpcError>) {
            created = p;
            fired = true;
        });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceCreateTerminalPane));
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("serverId")).toString(), QStringLiteral("srv-1"));
    QCOMPARE(params.value(QStringLiteral("devSessionId")).toString(), QStringLiteral("s1"));
    QCOMPARE(params.value(QStringLiteral("name")).toString(), QStringLiteral("shell"));
    QCOMPARE(params.value(QStringLiteral("workingDirectory")).toString(),
             QStringLiteral("/home/me/repo"));
    QCOMPARE(params.value(QStringLiteral("harness")).toString(),
             QStringLiteral("oh-my-pi"));
    QVERIFY(!params.contains(QStringLiteral("tmuxTarget")));
    QVERIFY(!params.contains(QStringLiteral("startupCommand")));
    QVERIFY(!params.contains(QStringLiteral("position")));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "t9"}, {"serverId", "srv-1"},
                                {"devSessionId", "s1"}, {"name", "shell"},
                                {"workingDirectory", "/home/me/repo"},
                                {"tmuxTarget", "ch_s1_t9"},
                                {"startupCommand", QJsonValue(QJsonValue::Null)},
                                {"harness", "oh-my-pi"}, {"position", 2}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QVERIFY(created.has_value());
    QCOMPARE(created->id.value, QStringLiteral("t9"));
    QCOMPARE(created->devSessionId.value, QStringLiteral("s1"));
    QCOMPARE(created->tmuxTarget, QStringLiteral("ch_s1_t9"));
    QCOMPARE(created->harness, QStringLiteral("oh-my-pi"));
    QCOMPARE(created->position, 2);
    // Documented narrowing: a null nullable-text column decodes to "".
    QVERIFY(created->startupCommand.isEmpty());
}

// The LEGACY half of the call every terminal pane makes before it can attach.
// It must go out as the RESOLVE method — the atomic server-side
// lookup-or-create — and never as list + create, which two clients can run into
// two rows for one layout slot. `name` is the layout slot label, which is the
// lookup key only for a leaf stored before layouts carried a row id; nothing
// else may be sent, because this call must not be able to overwrite a pane it
// merely wanted to find.
void TstWorkspaceDb::resolveTerminalPaneSendsTheSlotAddressAndParsesTheRow()
{
    makePair();

    bool fired = false;
    std::optional<ch::TerminalPane> resolved;
    m_db->resolveTerminalPane(
        ch::ResolveTerminalPaneParams{.serverId = ch::ServerId{QStringLiteral("srv-1")},
                                      .devSessionId = ch::DevSessionId{QStringLiteral("s1")},
                                      .name = QStringLiteral("terminal-2"),
                                      .workingDirectory = QStringLiteral("/home/me/repo")},
        [&](std::optional<ch::TerminalPane> pane, std::optional<RpcError>) {
            resolved = pane;
            fired = true;
        });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceResolveTerminalPane));
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("serverId")).toString(), QStringLiteral("srv-1"));
    QCOMPARE(params.value(QStringLiteral("devSessionId")).toString(), QStringLiteral("s1"));
    QCOMPARE(params.value(QStringLiteral("name")).toString(), QStringLiteral("terminal-2"));
    QCOMPARE(params.value(QStringLiteral("workingDirectory")).toString(),
             QStringLiteral("/home/me/repo"));
    // The client cannot propose an identity: minting is the server's, once.
    QVERIFY(!params.contains(QStringLiteral("tmuxTarget")));
    QVERIFY(!params.contains(QStringLiteral("position")));
    QVERIFY(!params.contains(QStringLiteral("harness")));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "row-uuid"}, {"serverId", "srv-1"},
                                {"devSessionId", "s1"}, {"name", "terminal-2"},
                                {"workingDirectory", "/home/me/repo"},
                                {"tmuxTarget", "ch_s1_row-uuid"},
                                {"startupCommand", QJsonValue(QJsonValue::Null)},
                                {"harness", QJsonValue(QJsonValue::Null)},
                                {"position", 1}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QVERIFY(resolved.has_value());
    // The target the pane will attach: the server's, read back verbatim.
    QCOMPARE(resolved->tmuxTarget, QStringLiteral("ch_s1_row-uuid"));
    QCOMPARE(resolved->name, QStringLiteral("terminal-2"));

    // The working directory is omitted when the caller has none, so the server
    // applies its own default instead of being told to store an explicit null.
    //
    // This request is ANSWERED rather than left hanging: a pending callback is
    // dispatched with a synthetic error when the client is destroyed in
    // cleanup(), which is after this function has returned, so a callback
    // capturing a local by reference would write into a dead stack frame.
    bool secondFired = false;
    m_db->resolveTerminalPane(
        ch::ResolveTerminalPaneParams{.serverId = ch::ServerId{QStringLiteral("srv-1")},
                                      .devSessionId = ch::DevSessionId{QStringLiteral("s1")},
                                      .name = QStringLiteral("terminal-3")},
        [&](std::optional<ch::TerminalPane>, std::optional<RpcError>) { secondFired = true; });
    const QJsonObject secondReq = readRequest();
    const QJsonObject bare = secondReq.value(QStringLiteral("params")).toObject();
    QVERIFY(!bare.contains(QStringLiteral("workingDirectory")));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", secondReq.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "row-3"}, {"serverId", "srv-1"},
                                {"devSessionId", "s1"}, {"name", "terminal-3"},
                                {"tmuxTarget", "ch_s1_row-3"}, {"position", 2}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(secondFired);
    QCOMPARE(m_client->pendingCount(), 0);
}

// The NORMAL half: a leaf that carries its `terminal_panes` row id is addressed
// by that id, and the recyclable slot label does not go out at all. Sending
// both would leave the server choosing, and sending the label alone is the
// defect this replaces — a label freed by a closed pane is re-minted by another
// client, and lookup-or-create hands the new pane the closed pane's shell.
void TstWorkspaceDb::resolveTerminalPaneAddressesARowByIdWithoutItsLabel()
{
    makePair();

    m_db->resolveTerminalPane(
        ch::ResolveTerminalPaneParams{.serverId = ch::ServerId{QStringLiteral("srv-1")},
                                      .devSessionId = ch::DevSessionId{QStringLiteral("s1")},
                                      .id = ch::TerminalId{QStringLiteral("row-uuid")},
                                      .name = QStringLiteral("terminal-2")},
        [](std::optional<ch::TerminalPane>, std::optional<RpcError>) {});

    const QJsonObject params =
        readRequest().value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("id")).toString(), QStringLiteral("row-uuid"));
    QVERIFY(!params.contains(QStringLiteral("name")));
}

// Neither address is not a question: there is no row id and no slot label, so
// there is nothing to look up. It used to go out as `name: ""`, asking the
// server to find - or CREATE - a pane called "". The refusal happens here, with
// nothing on the wire, and it arrives the way every other answer does: from the
// event loop, after this method has returned, never re-entering the caller from
// inside its own call.
void TstWorkspaceDb::resolveTerminalPaneWithNoAddressFailsWithoutAskingTheServer()
{
    makePair();

    bool fired = false;
    std::optional<RpcError> err;
    std::optional<ch::TerminalPane> pane;
    m_db->resolveTerminalPane(
        ch::ResolveTerminalPaneParams{
            .serverId = ch::ServerId{QStringLiteral("srv-1")},
            .devSessionId = ch::DevSessionId{QStringLiteral("s1")}},
        [&](std::optional<ch::TerminalPane> p, std::optional<RpcError> e) {
            pane = p;
            err = e;
            fired = true;
        });

    QVERIFY(!fired); // async like every other reply; no caller is re-entered
    QTRY_VERIFY(fired);
    QVERIFY(!pane.has_value());
    QVERIFY(err.has_value());
    QCOMPARE(err->code, -32602);
    QVERIFY2(err->message.contains(
                 QString::fromLatin1(ch::rpc::kMethodWorkspaceResolveTerminalPane)),
             qPrintable(err->message));

    // Nothing was asked of the server: no request frame, and no id burned.
    m_clientSide->flush();
    QVERIFY(!m_serverSide->waitForReadyRead(100));
    QCOMPARE(m_serverSide->bytesAvailable(), 0);
    QCOMPARE(m_client->pendingCount(), 0);
}

// A tree the client cannot read back must not be written. Storing one loses the
// user's layout silently: setLayout reports success, and the loss only shows up
// on the next launch, when getLayout maps the unreadable tree to "no layout".
// The refusal is local, so the bad bytes never reach the server at all.
void TstWorkspaceDb::setLayoutRefusesATreeItCouldNotReadBack()
{
    makePair();

    SplitNode a;
    a.paneId = QStringLiteral("pA");
    SplitNode b;
    b.paneId = QStringLiteral("pB");
    SplitNode bad;
    bad.children = {a, b};
    bad.ratios = {1.0}; // one ratio for two children: fromJson refuses this

    bool fired = false;
    std::optional<RpcError> err;
    std::optional<SplitNode> stored;
    m_db->setLayout(ch::ServerId{QStringLiteral("srv-1")},
                    ch::DevSessionId{QStringLiteral("s1")}, ch::Region::Viewer,
                    bad,
                    [&](std::optional<SplitNode> tree, std::optional<RpcError> e) {
                        stored = tree;
                        err = e;
                        fired = true;
                    });

    QVERIFY(!fired); // async like every other reply
    QTRY_VERIFY(fired);
    QVERIFY(!stored.has_value());
    QVERIFY(err.has_value());
    QCOMPARE(err->code, -32602);
    QVERIFY2(err->message.contains(kSetLayout), qPrintable(err->message));

    m_clientSide->flush();
    QVERIFY(!m_serverSide->waitForReadyRead(100));
    QCOMPARE(m_serverSide->bytesAvailable(), 0);
    QCOMPARE(m_client->pendingCount(), 0);

    // The same call with a tree that DOES round-trip still goes out, so the
    // check is selective rather than a blanket refusal.
    bad.ratios = {1.0, 1.0};
    m_db->setLayout(ch::ServerId{QStringLiteral("srv-1")},
                    ch::DevSessionId{QStringLiteral("s1")}, ch::Region::Viewer,
                    bad,
                    [](std::optional<SplitNode>, std::optional<RpcError>) {});
    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(), kSetLayout);
    QVERIFY(SplitNode::fromJson(req.value(QStringLiteral("params"))
                                    .toObject()
                                    .value(QStringLiteral("tree"))
                                    .toObject())
            == bad);
}

// The repository dereferences its client in every method, so there is no such
// thing as a WorkspaceDb without one. A literal nullptr is refused by the
// compiler; this pins that the deleted overload exists, because the mistake it
// prevents is a crash inside whichever async method happened to run first,
// arbitrarily far from the construction site that caused it.
void TstWorkspaceDb::workspaceDbCannotBeBuiltWithoutAClient()
{
    QVERIFY(!(std::is_constructible_v<WorkspaceDb, std::nullptr_t>));
    QVERIFY((std::is_constructible_v<WorkspaceDb, CodeharbordClient*>));
}

// setLayout has NO legitimate "there is no layout here" answer: it echoes the
// SessionLayout row it has just stored. A row whose `tree` is missing or fails
// split-tree validation is therefore a malformed reply, and handing the caller
// (no tree, no error) would report the write as landed while giving it nothing
// to adopt - the same blank-record trap every other method reports as -32603.
// getLayout's opposite verdict (an unreadable tree means "no layout") is
// deliberate and covered by getLayoutMalformedTreeDeliversNullopt; this pins the
// boundary between the two.
void TstWorkspaceDb::setLayoutMalformedTreeDeliversRpcError()
{
    makePair();

    SplitNode leaf;
    leaf.paneId = QStringLiteral("pA");

    bool fired = false;
    bool hasTree = true;
    std::optional<RpcError> err;

    // Write the current tree, then answer with `row`. Leaves the three locals
    // above holding the callback's verdict.
    const auto writeLayout = [&](const QJsonObject& row) {
        fired = false;
        hasTree = true;
        err.reset();
        m_db->setLayout(ServerId{QStringLiteral("srv-1")},
                        ch::DevSessionId{QStringLiteral("s1")}, ch::Region::Viewer,
                        leaf,
                        [&](std::optional<SplitNode> tree, std::optional<RpcError> e) {
                            hasTree = tree.has_value();
                            err = e;
                            fired = true;
                        });
        const QJsonObject req = readRequest();
        m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                      {"id", req.value(QStringLiteral("id")).toInt()},
                                      {"result", row}}));
        m_serverSide->flush();
    };

    // A SessionLayout row carrying no `tree` key at all.
    writeLayout(QJsonObject{{"id", "L1"}, {"devSessionId", "s1"}});
    QTRY_VERIFY(fired);
    QVERIFY(!hasTree);
    QVERIFY(err.has_value());
    QCOMPARE(err->code, -32603);
    QVERIFY(err->message.contains(QStringLiteral("workspace.setLayout")));

    // A row whose tree is structurally invalid: two children, one ratio.
    const QJsonObject leafA{{"type", "leaf"}, {"paneId", "pA"}};
    const QJsonObject leafB{{"type", "leaf"}, {"paneId", "pB"}};
    writeLayout(QJsonObject{
        {"id", "L1"},
        {"tree", QJsonObject{{"type", "split"},
                             {"orientation", "vertical"},
                             {"children", QJsonArray{leafA, leafB}},
                             {"ratios", QJsonArray{1.0}}}}});
    QTRY_VERIFY(fired);
    QVERIFY(!hasTree);
    QVERIFY(err.has_value());
    QCOMPARE(err->code, -32603);

    // The rejection is selective: a row echoing a valid tree still succeeds, and
    // an empty leaf (what closing a region's last pane stores) is a valid tree.
    writeLayout(QJsonObject{{"id", "L1"},
                            {"tree", QJsonObject{{"type", "leaf"}, {"paneId", ""}}}});
    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QVERIFY(hasTree);
}

// The server reads the nullable-text update fields as three-valued: absent means
// "keep the stored value", JSON null means "clear the column", and a string means
// "store that string" (remote/src/workspace.ts updateTerminalPane). A
// std::optional<QString> can only express two of those, and WHICH two is a wire
// contract the server's logic depends on: a disengaged optional MUST leave the
// key out (or the update would overwrite a column the caller never mentioned),
// and an engaged one MUST send a JSON string even when it is empty (turning ""
// into an omission would silently drop a deliberate write, and turning it into
// null would clear a column the caller only meant to blank). Neither rule is
// visible from the C++ signature, so pin both here.
void TstWorkspaceDb::updateOptionalsOmitUnsetAndSendEmptyStringsVerbatim()
{
    makePair();

    bool fired = false;
    m_db->updateTerminalPane(
        ch::UpdateTerminalPaneParams{.id = ch::TerminalId{QStringLiteral("t1")},
                                     .workingDirectory = QString(),
                                     .tmuxTarget = QString(),
                                     .harness = QString()},
        [&](std::optional<ch::TerminalPane>, std::optional<RpcError>) { fired = true; });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceUpdateTerminalPane));
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("id")).toString(), QStringLiteral("t1"));

    // Engaged-but-empty: present, a JSON string, and empty. Not absent, not null.
    for (const QString& key : {QStringLiteral("workingDirectory"),
                               QStringLiteral("tmuxTarget"),
                               QStringLiteral("harness")}) {
        QVERIFY2(params.contains(key), qPrintable(key));
        QVERIFY2(params.value(key).isString(), qPrintable(key));
        QVERIFY2(!params.value(key).isNull(), qPrintable(key));
        QVERIFY2(params.value(key).toString().isEmpty(), qPrintable(key));
    }
    // An empty tmuxTarget is a WRITE of "", which the server refuses (it demands
    // 1-N characters of [A-Za-z0-9_-]) rather than treating as a clear. The
    // client's job is to send it verbatim and let that -32602 come back, which is
    // exactly what the assertions above pin.

    // Disengaged: absent entirely, so the server keeps the stored value.
    QVERIFY(!params.contains(QStringLiteral("name")));
    QVERIFY(!params.contains(QStringLiteral("startupCommand")));
    QVERIFY(!params.contains(QStringLiteral("position")));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "t1"}, {"serverId", "srv-1"},
                                {"devSessionId", "s1"}, {"name", "sh"},
                                {"workingDirectory", ""}, {"tmuxTarget", ""},
                                {"startupCommand", QJsonValue(QJsonValue::Null)},
                                {"harness", ""}, {"position", 0}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
}

// The Dev Session pin bit is server-owned state (schema version 5) and it drives
// the sidebar's star filter, so it has to survive both directions of the wire.
// Nothing covered it: the pin could have been dropped on decode, or an explicit
// "unpin" could have been swallowed as if it were an unset field, and every
// existing test would still have passed.
void TstWorkspaceDb::pinnedStateCrossesTheWireInBothDirections()
{
    makePair();

    QVector<GroupNode> got;
    std::optional<RpcError> err;
    bool fired = false;
    m_db->list(ServerId{QStringLiteral("srv-1")},
               [&](QVector<GroupNode> groups, std::optional<RpcError> e) {
                   got = groups;
                   err = e;
                   fired = true;
               });

    const QJsonObject listReq = readRequest();
    // The pin FILTER is a client-local presentation choice held by
    // ch::SessionsModel, never a server-side narrowing: asking the server to
    // pre-filter would return a tree missing rows the client still needs the
    // moment the user turns the filter off.
    QVERIFY(!listReq.value(QStringLiteral("params")).toObject()
                 .contains(QStringLiteral("pinnedOnly")));

    const char* kJson = R"([
      {
        "id": "g1", "serverId": "srv-1", "name": "Alpha",
        "position": 0, "collapsed": false,
        "sessions": [
          { "id": "s1", "serverId": "srv-1", "groupId": "g1", "name": "Pinned",
            "repositoryRoot": "/r", "position": 0, "archived": false,
            "pinned": true,
            "viewerPanes": [], "terminalPanes": [],
            "layouts": { "viewer": null, "terminal": null } },
          { "id": "s2", "serverId": "srv-1", "groupId": "g1", "name": "Plain",
            "repositoryRoot": "/r", "position": 1, "archived": true,
            "pinned": false,
            "viewerPanes": [], "terminalPanes": [],
            "layouts": { "viewer": null, "terminal": null } },
          { "id": "s3", "serverId": "srv-1", "groupId": "g1", "name": "Legacy",
            "repositoryRoot": "/r", "position": 2, "archived": false,
            "viewerPanes": [], "terminalPanes": [],
            "layouts": { "viewer": null, "terminal": null } }
        ]
      }
    ])";
    const QJsonArray result = QJsonDocument::fromJson(kJson).array();
    QCOMPARE(result.size(), 1); // guard against a malformed canned fixture

    m_serverSide->write(jsonLine({{"jsonrpc", "2.0"},
                                  {"id", listReq.value(QStringLiteral("id")).toInt()},
                                  {"result", result}}));
    m_serverSide->flush();

    QTRY_VERIFY(fired);
    QVERIFY(!err.has_value());
    QCOMPARE(got.size(), 1);
    QCOMPARE(got[0].sessions.size(), 3);
    QCOMPARE(got[0].sessions[0].session.pinned, true);
    QCOMPARE(got[0].sessions[1].session.pinned, false);
    QCOMPARE(got[0].sessions[1].session.archived, true);
    // A row from a database that predates the column arrives without the key at
    // all and must read as unpinned rather than as anything else.
    QCOMPARE(got[0].sessions[2].session.pinned, false);

    // Unpinning is an explicit `pinned: false` on the wire. Dropping it because
    // the value is false would leave the row pinned on the server while the
    // sidebar showed it unpinned.
    bool updateFired = false;
    m_db->updateSession(
        ch::UpdateSessionParams{.id = ch::DevSessionId{QStringLiteral("s1")},
                                .pinned = false},
        [&](std::optional<ch::DevSession>, std::optional<RpcError>) {
            updateFired = true;
        });
    const QJsonObject updateReq = readRequest();
    const QJsonObject updateParams =
        updateReq.value(QStringLiteral("params")).toObject();
    QVERIFY(updateParams.contains(QStringLiteral("pinned")));
    QCOMPARE(updateParams.value(QStringLiteral("pinned")).toBool(), false);
    QVERIFY(!updateParams.contains(QStringLiteral("archived")));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", updateReq.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "s1"}, {"serverId", "srv-1"},
                                {"groupId", "g1"}, {"name", "Pinned"},
                                {"repositoryRoot", "/r"}, {"position", 0},
                                {"archived", false}, {"pinned", false}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(updateFired);

    // On create, an unset pin is ABSENT so the server applies its own default.
    bool createFired = false;
    m_db->createSession(
        ch::CreateSessionParams{.serverId = ServerId{QStringLiteral("srv-1")},
                                .groupId = ch::GroupId{QStringLiteral("g1")},
                                .name = QStringLiteral("Fresh"),
                                .repositoryRoot = QStringLiteral("/r")},
        [&](std::optional<ch::DevSession>, std::optional<RpcError>) {
            createFired = true;
        });
    const QJsonObject createReq = readRequest();
    const QJsonObject createParams =
        createReq.value(QStringLiteral("params")).toObject();
    QVERIFY(!createParams.contains(QStringLiteral("pinned")));
    QVERIFY(!createParams.contains(QStringLiteral("archived")));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", createReq.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "s4"}, {"serverId", "srv-1"},
                                {"groupId", "g1"}, {"name", "Fresh"},
                                {"repositoryRoot", "/r"}, {"position", 3},
                                {"archived", false}, {"pinned", false}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(createFired);
}

// updateGroup is a partial update whose only two mutable fields are a name and
// a boolean, and nothing covered it. `collapsed: false` is the interesting
// case: expanding a collapsed group is spelled as a SET optional holding
// false, and dropping it because the value is falsy would leave every group
// permanently collapsed on the server while the sidebar showed it open.
void TstWorkspaceDb::updateGroupSerializesPartialWithFalse()
{
    makePair();

    std::optional<Group> updated;
    bool fired = false;
    m_db->updateGroup(
        ch::UpdateGroupParams{.id = ch::GroupId{QStringLiteral("g1")},
                              .collapsed = false},
        [&](std::optional<Group> g, std::optional<RpcError>) {
            updated = g;
            fired = true;
        });

    const QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceUpdateGroup));
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("id")).toString(), QStringLiteral("g1"));
    QVERIFY(params.contains(QStringLiteral("collapsed")));
    QCOMPARE(params.value(QStringLiteral("collapsed")).toBool(), false);
    // Untouched fields stay absent so the server keeps what it stores.
    QVERIFY(!params.contains(QStringLiteral("name")));
    QVERIFY(!params.contains(QStringLiteral("position")));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "g1"}, {"serverId", "srv-1"},
                                {"name", "Alpha"}, {"position", 0},
                                {"collapsed", false}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(fired);
    QVERIFY(updated.has_value());
    QCOMPARE(updated->id.value, QStringLiteral("g1"));
    QCOMPARE(updated->collapsed, false);
}

// The whole viewer-pane family was untested: create (required url plus three
// optionals), update (partial), and delete (result discarded, outcome
// forwarded). A viewer pane's `url` is what the pane actually shows, so losing
// it on the wire reopens the region blank.
void TstWorkspaceDb::viewerPaneMethodsSerializeParamsAndForwardOutcomes()
{
    makePair();

    // create: the required fields go out, the unset optionals do not.
    std::optional<ch::ViewerPane> created;
    bool createFired = false;
    m_db->createViewerPane(
        ch::CreateViewerPaneParams{
            .serverId = ServerId{QStringLiteral("srv-1")},
            .devSessionId = ch::DevSessionId{QStringLiteral("s1")},
            .url = QStringLiteral("codeharbor-internal://file/a%20b.md"),
            .handler = QStringLiteral("markdown")},
        [&](std::optional<ch::ViewerPane> p, std::optional<RpcError>) {
            created = p;
            createFired = true;
        });

    QJsonObject req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceCreateViewerPane));
    QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("serverId")).toString(), QStringLiteral("srv-1"));
    QCOMPARE(params.value(QStringLiteral("devSessionId")).toString(), QStringLiteral("s1"));
    QCOMPARE(params.value(QStringLiteral("url")).toString(),
             QStringLiteral("codeharbor-internal://file/a%20b.md"));
    QCOMPARE(params.value(QStringLiteral("handler")).toString(),
             QStringLiteral("markdown"));
    QVERIFY(!params.contains(QStringLiteral("title")));
    QVERIFY(!params.contains(QStringLiteral("position")));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "v7"}, {"serverId", "srv-1"},
                                {"devSessionId", "s1"},
                                {"url", "codeharbor-internal://file/a%20b.md"},
                                {"handler", "markdown"},
                                {"title", QJsonValue(QJsonValue::Null)},
                                {"position", 4}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(createFired);
    QVERIFY(created.has_value());
    QCOMPARE(created->id.value, QStringLiteral("v7"));
    QCOMPARE(created->url, QStringLiteral("codeharbor-internal://file/a%20b.md"));
    QCOMPARE(created->position, 4);
    QVERIFY(created->title.isEmpty()); // documented null -> "" narrowing

    // update: a partial that moves the pane to position 0 and retitles it.
    // Position 0 is a SET optional and must survive as "first", not vanish.
    std::optional<ch::ViewerPane> moved;
    bool updateFired = false;
    m_db->updateViewerPane(
        ch::UpdateViewerPaneParams{.id = ch::ViewerPaneId{QStringLiteral("v7")},
                                   .title = QStringLiteral("Notes"),
                                   .position = 0},
        [&](std::optional<ch::ViewerPane> p, std::optional<RpcError>) {
            moved = p;
            updateFired = true;
        });
    req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceUpdateViewerPane));
    params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("id")).toString(), QStringLiteral("v7"));
    QCOMPARE(params.value(QStringLiteral("title")).toString(), QStringLiteral("Notes"));
    QVERIFY(params.contains(QStringLiteral("position")));
    QCOMPARE(params.value(QStringLiteral("position")).toInt(), 0);
    QVERIFY(!params.contains(QStringLiteral("url")));
    QVERIFY(!params.contains(QStringLiteral("handler")));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"result", QJsonObject{{"id", "v7"}, {"serverId", "srv-1"},
                                {"devSessionId", "s1"},
                                {"url", "codeharbor-internal://file/a%20b.md"},
                                {"handler", "markdown"}, {"title", "Notes"},
                                {"position", 0}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(updateFired);
    QVERIFY(moved.has_value());
    QCOMPARE(moved->title, QStringLiteral("Notes"));
    QCOMPARE(moved->position, 0);

    // delete: id only, and a server refusal reaches the caller verbatim rather
    // than being reported as a successful removal.
    std::optional<RpcError> deleteErr;
    bool deleteFired = false;
    m_db->deleteViewerPane(ch::ViewerPaneId{QStringLiteral("v7")},
                           [&](std::optional<RpcError> e) {
                               deleteErr = e;
                               deleteFired = true;
                           });
    req = readRequest();
    QCOMPARE(req.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWorkspaceDeleteViewerPane));
    QCOMPARE(req.value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("id")).toString(),
             QStringLiteral("v7"));

    m_serverSide->write(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", req.value(QStringLiteral("id")).toInt()},
         {"error", QJsonObject{{"code", -32602}, {"message", "no such pane"}}}}));
    m_serverSide->flush();
    QTRY_VERIFY(deleteFired);
    QVERIFY(deleteErr.has_value());
    QCOMPARE(deleteErr->code, -32602);
    QCOMPARE(deleteErr->message, QStringLiteral("no such pane"));
}

QTEST_GUILESS_MAIN(TstWorkspaceDb)
#include "tst_workspacedb.moc"
