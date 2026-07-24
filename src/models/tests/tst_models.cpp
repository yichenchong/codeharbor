#include <QtTest/QtTest>

#include <QJsonArray>
#include <QJsonObject>

#include "SessionState.h"
#include "SessionsModel.h"
#include "SplitTree.h"
#include "WorkspaceTypes.h"

using namespace ch;

namespace {

TerminalStatus terminal(TerminalState connection, AgentState agent)
{
    return TerminalStatus{TerminalId{QStringLiteral("t")}, connection, agent};
}

} // namespace

class TstModels : public QObject {
    Q_OBJECT

private slots:
    void buildsWorkspaceTree();
    void modelExposesGroupsAndSessions();
    void emptyGroupHasNoSessions();
    void sessionWithoutTerminalsIsDisconnected();
    void splitTreeRoundTripsNestedLayout();
    void splitTreeRejectsRatioMismatch();
    void splitTreeRejectsChildlessSplit();
    void aggregateRowStatePrecedence();
};

// Group -> DevSession -> viewer + terminal panes tree.
void TstModels::buildsWorkspaceTree()
{
    Group group;
    group.id = GroupId{QStringLiteral("g1")};
    group.name = QStringLiteral("Work");
    group.position = 0;
    group.collapsed = false;

    DevSession session;
    session.id = DevSessionId{QStringLiteral("s1")};
    session.groupId = group.id;
    session.name = QStringLiteral("codeharbor");
    session.repositoryRoot = QStringLiteral("/home/dev/codeharbor");
    session.defaultWorkingDirectory = QStringLiteral("/home/dev/codeharbor/src");
    session.taskDescription = QStringLiteral("wave 1 models");
    session.position = 0;
    session.archived = false;

    ViewerPane viewer;
    viewer.id = ViewerPaneId{QStringLiteral("v1")};
    viewer.devSessionId = session.id;
    viewer.url = QStringLiteral("file:///home/dev/codeharbor/README.md");
    viewer.handler = QStringLiteral("markdown");
    viewer.title = QStringLiteral("README");
    viewer.position = 0;

    TerminalPane term;
    term.id = TerminalId{QStringLiteral("tp1")};
    term.devSessionId = session.id;
    term.name = QStringLiteral("shell");
    term.workingDirectory = QStringLiteral("/home/dev/codeharbor");
    term.tmuxTarget = QStringLiteral("codeharbor:0.0");
    term.startupCommand = QStringLiteral("git status");
    term.harness = QStringLiteral("oh-my-pi");
    term.position = 0;

    QCOMPARE(session.groupId, group.id);
    QCOMPARE(viewer.devSessionId, session.id);
    QCOMPARE(term.devSessionId, session.id);
    QCOMPARE(term.tmuxTarget, QStringLiteral("codeharbor:0.0"));

    // Defaulted equality on the value types.
    DevSession copy = session;
    QVERIFY(copy == session);
    copy.archived = true;
    QVERIFY(!(copy == session));
}

void TstModels::modelExposesGroupsAndSessions()
{
    SessionsModel model;

    SessionRow s1;
    s1.session.id = DevSessionId{QStringLiteral("s1")};
    s1.session.name = QStringLiteral("codeharbor");
    s1.subtitle = QStringLiteral("main");
    s1.terminals = {terminal(TerminalState::Ready, AgentState::WaitingInput)};

    GroupRow g1;
    g1.group.id = GroupId{QStringLiteral("g1")};
    g1.group.name = QStringLiteral("Work");
    g1.group.collapsed = true;
    g1.sessions = {s1};

    model.setGroups({g1});

    QCOMPARE(model.rowCount(), 1);
    const QModelIndex groupIndex = model.index(0, 0);
    QVERIFY(groupIndex.isValid());
    QVERIFY(!model.parent(groupIndex).isValid());
    QCOMPARE(model.data(groupIndex, SessionsModel::NameRole).toString(), QStringLiteral("Work"));
    QCOMPARE(model.data(groupIndex, SessionsModel::IsGroupRole).toBool(), true);
    QCOMPARE(model.data(groupIndex, SessionsModel::CollapsedRole).toBool(), true);

    QCOMPARE(model.rowCount(groupIndex), 1);
    const QModelIndex sessionIndex = model.index(0, 0, groupIndex);
    QVERIFY(sessionIndex.isValid());
    QCOMPARE(model.parent(sessionIndex), groupIndex);
    QCOMPARE(model.rowCount(sessionIndex), 0);
    QCOMPARE(model.data(sessionIndex, SessionsModel::NameRole).toString(),
             QStringLiteral("codeharbor"));
    QCOMPARE(model.data(sessionIndex, SessionsModel::SubtitleRole).toString(),
             QStringLiteral("main"));
    QCOMPARE(model.data(sessionIndex, SessionsModel::IsGroupRole).toBool(), false);
    QCOMPARE(model.data(sessionIndex, SessionsModel::RowStateRole).toInt(),
             static_cast<int>(SessionRowState::WaitingForInput));
}

void TstModels::emptyGroupHasNoSessions()
{
    SessionsModel model;
    GroupRow empty;
    empty.group.id = GroupId{QStringLiteral("g0")};
    empty.group.name = QStringLiteral("Empty");
    model.setGroups({empty});

    QCOMPARE(model.rowCount(), 1);
    const QModelIndex groupIndex = model.index(0, 0);
    QCOMPARE(model.rowCount(groupIndex), 0);
    QVERIFY(!model.index(0, 0, groupIndex).isValid());
}

void TstModels::sessionWithoutTerminalsIsDisconnected()
{
    QCOMPARE(static_cast<int>(SessionsModel::aggregateSessionState({})),
             static_cast<int>(SessionRowState::Disconnected));

    SessionsModel model;
    SessionRow s;
    s.session.name = QStringLiteral("idle");
    // no terminals
    GroupRow g;
    g.group.name = QStringLiteral("G");
    g.sessions = {s};
    model.setGroups({g});

    const QModelIndex sessionIndex = model.index(0, 0, model.index(0, 0));
    QCOMPARE(model.data(sessionIndex, SessionsModel::RowStateRole).toInt(),
             static_cast<int>(SessionRowState::Disconnected));
}

// VerticalSplit[ leaf A, HorizontalSplit[ leaf B, leaf C ] ] round-trips exactly.
void TstModels::splitTreeRoundTripsNestedLayout()
{
    SplitNode leafA;
    leafA.paneId = QStringLiteral("A");

    SplitNode leafB;
    leafB.paneId = QStringLiteral("B");
    SplitNode leafC;
    leafC.paneId = QStringLiteral("C");

    SplitNode inner;
    inner.orientation = SplitOrientation::Horizontal;
    inner.children = {leafB, leafC};
    inner.ratios = {0.3, 0.7};

    SplitNode root;
    root.orientation = SplitOrientation::Vertical;
    root.children = {leafA, inner};
    root.ratios = {0.5, 0.5};

    const QJsonObject json = root.toJson();
    const SplitNode restored = SplitNode::fromJson(json);

    QVERIFY(restored == root);
    QVERIFY(restored.children.at(0).isLeaf());
    QVERIFY(!restored.children.at(1).isLeaf());
    QCOMPARE(restored.orientation, SplitOrientation::Vertical);
    QCOMPARE(restored.children.at(1).orientation, SplitOrientation::Horizontal);
    QCOMPARE(restored.children.at(1).children.at(1).paneId, QStringLiteral("C"));
}

void TstModels::splitTreeRejectsRatioMismatch()
{
    SplitNode leafB;
    leafB.paneId = QStringLiteral("B");
    SplitNode leafC;
    leafC.paneId = QStringLiteral("C");

    SplitNode split;
    split.orientation = SplitOrientation::Horizontal;
    split.children = {leafB, leafC};
    split.ratios = {0.5}; // one ratio for two children

    const SplitNode restored = SplitNode::fromJson(split.toJson());
    QVERIFY(restored == SplitNode{});
    QVERIFY(restored.isLeaf());
    QVERIFY(restored.paneId.isEmpty());
}

void TstModels::splitTreeRejectsChildlessSplit()
{
    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("split");
    obj[QStringLiteral("orientation")] = QStringLiteral("vertical");
    obj[QStringLiteral("children")] = QJsonArray{};
    obj[QStringLiteral("ratios")] = QJsonArray{};

    QVERIFY(SplitNode::fromJson(obj) == SplitNode{});
}

// SPEC 4.2 precedence: Error > WaitingForInput > Running > FinishedUnseen > Idle
// > Disconnected. Higher-priority conditions win regardless of others present.
void TstModels::aggregateRowStatePrecedence()
{
    // QCOMPARE on the enum values directly collides with ch::toString (returns
    // QString); compare the ordinals, which also documents the precedence order.
    const auto stateOf = [](const QVector<TerminalStatus> &terminals) {
        return static_cast<int>(SessionsModel::aggregateSessionState(terminals));
    };

    QCOMPARE(stateOf({terminal(TerminalState::Ready, AgentState::Error),
                      terminal(TerminalState::Ready, AgentState::WaitingInput),
                      terminal(TerminalState::Ready, AgentState::Running)}),
             static_cast<int>(SessionRowState::Error));

    QCOMPARE(stateOf({terminal(TerminalState::Ready, AgentState::WaitingInput),
                      terminal(TerminalState::Ready, AgentState::Running),
                      terminal(TerminalState::Ready, AgentState::IdleUnseen)}),
             static_cast<int>(SessionRowState::WaitingForInput));

    QCOMPARE(stateOf({terminal(TerminalState::Ready, AgentState::Running),
                      terminal(TerminalState::Ready, AgentState::IdleUnseen)}),
             static_cast<int>(SessionRowState::Running));

    QCOMPARE(stateOf({terminal(TerminalState::Ready, AgentState::IdleUnseen),
                      terminal(TerminalState::Ready, AgentState::Idle)}),
             static_cast<int>(SessionRowState::FinishedUnseen));

    QCOMPARE(stateOf({terminal(TerminalState::Ready, AgentState::Idle)}),
             static_cast<int>(SessionRowState::Idle));

    QCOMPARE(stateOf({terminal(TerminalState::Disconnected, AgentState::Idle)}),
             static_cast<int>(SessionRowState::Disconnected));

    // A connection error is an error even when the agent is unremarkable.
    QCOMPARE(stateOf({terminal(TerminalState::Error, AgentState::Idle)}),
             static_cast<int>(SessionRowState::Error));
}

QTEST_GUILESS_MAIN(TstModels)
#include "tst_models.moc"
