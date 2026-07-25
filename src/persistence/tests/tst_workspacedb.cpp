#include "CodeharbordClient.h"
#include "WorkspaceDb.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QVector>
#include <QtTest/QtTest>

#include <optional>

using ch::CodeharbordClient;
using ch::CreateGroupParams;
using ch::Group;
using ch::GroupNode;
using ch::RpcError;
using ch::ServerId;
using ch::SplitNode;
using ch::WorkspaceDb;

namespace {

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
    const QByteArray line = newline >= 0 ? buffer.left(newline) : buffer;
    return QJsonDocument::fromJson(line).object();
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
             QStringLiteral("workspace.list"));
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
             QStringLiteral("workspace.createGroup"));
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
             QStringLiteral("workspace.getLayout"));
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
             QStringLiteral("workspace.updateSession"));
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
             QStringLiteral("workspace.setLayout"));
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("serverId")).toString(),
             QStringLiteral("srv-1"));
    QCOMPARE(params.value(QStringLiteral("devSessionId")).toString(),
             QStringLiteral("s1"));
    QCOMPARE(params.value(QStringLiteral("region")).toString(),
             QStringLiteral("terminal"));
    // The tree is an inline split-tree object mirroring SplitNode::toJson.
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
             QStringLiteral("workspace.duplicateSession"));
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
             QStringLiteral("workspace.reorderGroups"));
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

    m_client->setTransport(nullptr);
    proc.closeWriteChannel();
    proc.terminate();
    if (!proc.waitForFinished(2000))
        proc.kill();
}

QTEST_GUILESS_MAIN(TstWorkspaceDb)
#include "tst_workspacedb.moc"
