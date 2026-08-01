#include <QtTest/QtTest>

#include <QAbstractItemModelTester>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <cmath>
#include <functional>
#include <limits>
#include <utility>

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
    void splitTreeRejectsInvalidRatioValues();
    void modelSatisfiesItemModelInvariants();
    void splitTreeRejectsUnknownAndMissingType();
    void splitTreeRejectsInvalidNestedChild();
    void splitTreeRoundTripsSingleChildAndDeepNesting();
    void aggregateRowStatePrecedence();
    void splitTreeLeafOrientationRoundTrips();
    void splitTreeRoundTripsLeafUrl();
    void splitTreeWithoutUrlIsUnchangedByTheField();
    void splitTreeSplitPaneIdIgnoredOnRoundTrip();
    void splitTreeUnicodePaneIdRoundTrips();
    void splitTreeRejectsPathologicalDepth();
    void splitTreeAcceptsModerateDepth();
    void sessionsModelRejectsNonZeroColumn();
    void sessionsModelDataOnInvalidIndex();
    void sessionsModelSetGroupsTwiceResets();
    void aggregateRowStateAllFalseIsDisconnected();
    void aggregateRowStateCoversEveryInputCombination();
    void stateStringsArePinnedWireValues();
    void sessionsModelRoleNamesCoverEveryServedRole();
    void splitTreeRejectsNonObjectChildAndNonNumericRatio();
    void startingAgentCountsAsRunning();
    void agentStateWireWordsMatchRemoteEventsTs();
    void updateTerminalStatesEmitsTargetedDataChangedNotReset();
    void updateTerminalStatesFallsBackForEveryStructuralDrift();
    void updateTerminalStatesStaysSilentWhenTheAggregateDoesNotMove();
    void sessionsModelSessionIndicesAreScopedToTheirGroup();
    void sessionsModelIsSingleColumn();
    void splitTreeSplitUrlIgnoredOnRoundTrip();
    void splitTreeRoundTripsLeafTerminalPaneId();
    void splitTreeWithoutTerminalPaneIdIsUnchangedByTheField();
    void splitTreeSplitTerminalPaneIdIgnoredOnRoundTrip();
    void workspaceValueTypesCompareEveryField();
    void updateTerminalStatesFinishesEveryRowBeforeSignalling();
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

// Ratios drive pane geometry, so fromJson must reject non-finite (NaN/Inf) or
// non-positive ratio values (and all-zero arrays, whose sum is not > 0) from
// untrusted remote/persisted input, rather than accepting them and producing
// NaN/negative sizes or a divide-by-zero downstream. A valid ratio set must
// still round-trip unharmed.
void TstModels::splitTreeRejectsInvalidRatioValues()
{
    // Build a two-child split whose ratios array is supplied by the caller.
    const auto splitWithRatios = [](const QJsonArray &ratios) {
        QJsonObject leafA;
        leafA[QStringLiteral("type")] = QStringLiteral("leaf");
        leafA[QStringLiteral("paneId")] = QStringLiteral("A");
        QJsonObject leafB;
        leafB[QStringLiteral("type")] = QStringLiteral("leaf");
        leafB[QStringLiteral("paneId")] = QStringLiteral("B");

        QJsonObject obj;
        obj[QStringLiteral("type")] = QStringLiteral("split");
        obj[QStringLiteral("orientation")] = QStringLiteral("horizontal");
        obj[QStringLiteral("children")] = QJsonArray{leafA, leafB};
        obj[QStringLiteral("ratios")] = ratios;
        return obj;
    };

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // Each of these is malformed and must collapse to the empty-leaf sentinel.
    QVERIFY(SplitNode::fromJson(splitWithRatios(QJsonArray{-0.5, 1.5})) == SplitNode{}); // negative
    QVERIFY(SplitNode::fromJson(splitWithRatios(QJsonArray{0.0, 1.0})) == SplitNode{});  // zero
    QVERIFY(SplitNode::fromJson(splitWithRatios(QJsonArray{0.0, 0.0})) == SplitNode{});  // all-zero
    QVERIFY(SplitNode::fromJson(splitWithRatios(QJsonArray{nan, 0.5})) == SplitNode{});  // NaN
    QVERIFY(SplitNode::fromJson(splitWithRatios(QJsonArray{inf, 0.5})) == SplitNode{});  // +Inf
    QVERIFY(SplitNode::fromJson(splitWithRatios(QJsonArray{-inf, 0.5})) == SplitNode{}); // -Inf

    // A valid, strictly-positive finite ratio set is preserved exactly.
    const SplitNode restored = SplitNode::fromJson(splitWithRatios(QJsonArray{0.3, 0.7}));
    QVERIFY(!restored.isLeaf());
    QCOMPARE(restored.ratios.size(), 2);
    QCOMPARE(restored.ratios.at(0), 0.3);
    QCOMPARE(restored.ratios.at(1), 0.7);
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

// QAbstractItemModelTester drives the model through Qt's own invariant checks
// (index()/parent() inverse, rowCount/columnCount bounds, hasChildren coherence,
// data() on valid+invalid indices). It runs a full recursive pass over the
// populated tree on construction, catching model-protocol violations the
// hand-written assertions above do not exercise.
void TstModels::modelSatisfiesItemModelInvariants()
{
    SessionsModel model;

    const auto makeSession = [](const QString &name, TerminalState conn, AgentState agent) {
        SessionRow row;
        row.session.id = DevSessionId{name};
        row.session.name = name;
        row.subtitle = name + QStringLiteral(" subtitle");
        row.terminals = {terminal(conn, agent)};
        return row;
    };

    GroupRow work;
    work.group.id = GroupId{QStringLiteral("g-work")};
    work.group.name = QStringLiteral("Work");
    work.group.collapsed = false;
    work.sessions = {makeSession(QStringLiteral("alpha"), TerminalState::Ready, AgentState::Running),
                     makeSession(QStringLiteral("beta"), TerminalState::Error, AgentState::Idle)};

    GroupRow empty;
    empty.group.id = GroupId{QStringLiteral("g-empty")};
    empty.group.name = QStringLiteral("Empty");
    empty.group.collapsed = true;

    GroupRow personal;
    personal.group.id = GroupId{QStringLiteral("g-personal")};
    personal.group.name = QStringLiteral("Personal");
    personal.sessions = {makeSession(QStringLiteral("gamma"), TerminalState::Disconnected,
                                     AgentState::Unknown)};

    model.setGroups({work, empty, personal});

    // Constructed after population so the tester validates the full tree. In
    // QtTest reporting mode any invariant breach is raised as a test failure.
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    Q_UNUSED(tester);

    // Sanity anchors independent of the tester's internal checks.
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.rowCount(model.index(0, 0)), 2);
    QCOMPARE(model.rowCount(model.index(1, 0)), 0);
}

void TstModels::splitTreeRejectsUnknownAndMissingType()
{
    QJsonObject unknown;
    unknown[QStringLiteral("type")] = QStringLiteral("gadget");
    QVERIFY(SplitNode::fromJson(unknown) == SplitNode{});

    QJsonObject missing;
    missing[QStringLiteral("paneId")] = QStringLiteral("A");
    QVERIFY(SplitNode::fromJson(missing) == SplitNode{});
}

// A well-formed split whose child is itself an invalid split must be rejected
// wholesale, not silently repaired into a split with an empty-leaf child.
void TstModels::splitTreeRejectsInvalidNestedChild()
{
    QJsonObject badChild;
    badChild[QStringLiteral("type")] = QStringLiteral("split");
    badChild[QStringLiteral("orientation")] = QStringLiteral("vertical");
    badChild[QStringLiteral("children")] = QJsonArray{};
    badChild[QStringLiteral("ratios")] = QJsonArray{};

    QJsonObject leaf;
    leaf[QStringLiteral("type")] = QStringLiteral("leaf");
    leaf[QStringLiteral("paneId")] = QStringLiteral("A");

    QJsonObject root;
    root[QStringLiteral("type")] = QStringLiteral("split");
    root[QStringLiteral("orientation")] = QStringLiteral("horizontal");
    root[QStringLiteral("children")] = QJsonArray{leaf, badChild};
    root[QStringLiteral("ratios")] = QJsonArray{0.5, 0.5};

    QVERIFY(SplitNode::fromJson(root) == SplitNode{});
}

void TstModels::splitTreeRoundTripsSingleChildAndDeepNesting()
{
    // A split with a single child is structurally valid and must round-trip.
    SplitNode onlyChild;
    onlyChild.paneId = QStringLiteral("solo");
    SplitNode single;
    single.orientation = SplitOrientation::Vertical;
    single.children = {onlyChild};
    single.ratios = {1.0};
    QVERIFY(SplitNode::fromJson(single.toJson()) == single);

    // Deep nesting with a distinct orientation at each level must be preserved.
    SplitNode d;
    d.paneId = QStringLiteral("D");
    SplitNode c;
    c.paneId = QStringLiteral("C");
    SplitNode level2;
    level2.orientation = SplitOrientation::Horizontal;
    level2.children = {c, d};
    level2.ratios = {0.25, 0.75};

    SplitNode b;
    b.paneId = QStringLiteral("B");
    SplitNode level1;
    level1.orientation = SplitOrientation::Vertical;
    level1.children = {b, level2};
    level1.ratios = {0.4, 0.6};

    SplitNode a;
    a.paneId = QStringLiteral("A");
    SplitNode root;
    root.orientation = SplitOrientation::Horizontal;
    root.children = {a, level1};
    root.ratios = {0.5, 0.5};

    const SplitNode restored = SplitNode::fromJson(root.toJson());
    QVERIFY(restored == root);
    QCOMPARE(restored.orientation, SplitOrientation::Horizontal);
    QCOMPARE(restored.children.at(1).orientation, SplitOrientation::Vertical);
    QCOMPARE(restored.children.at(1).children.at(1).orientation, SplitOrientation::Horizontal);
    QCOMPARE(restored.children.at(1).children.at(1).children.at(1).paneId, QStringLiteral("D"));
}

// A leaf's orientation/ratios are meaningless and dropped by toJson(); equality
// must therefore ignore them so fromJson(toJson(leaf)) == leaf holds even when a
// leaf carries a non-default orientation. A defaulted operator== would fail this.
void TstModels::splitTreeLeafOrientationRoundTrips()
{
    SplitNode leaf;
    leaf.paneId = QStringLiteral("solo");
    leaf.orientation = SplitOrientation::Vertical; // dropped by toJson for leaves
    leaf.ratios = {0.25};                          // dropped by toJson for leaves

    const SplitNode restored = SplitNode::fromJson(leaf.toJson());
    QVERIFY(restored.isLeaf());
    QCOMPARE(restored.paneId, QStringLiteral("solo"));
    QVERIFY(restored == leaf);

    // Two leaves differing only in the dropped fields are equal.
    SplitNode other = leaf;
    other.orientation = SplitOrientation::Horizontal;
    other.ratios.clear();
    QVERIFY(other == leaf);

    // A leaf and a split are never equal even with matching paneId.
    SplitNode split;
    split.paneId = QStringLiteral("solo");
    split.children = {SplitNode{}};
    split.ratios = {1.0};
    QVERIFY(!(split == leaf));
}

// A leaf remembers WHAT IT SHOWS, not just that it exists. Without this the
// layout reopens with the right panes and every one of them blank, so the user
// re-finds their files on every launch.
void TstModels::splitTreeRoundTripsLeafUrl()
{
    SplitNode leaf;
    leaf.paneId = QStringLiteral("viewer-1");
    leaf.url = QStringLiteral("codeharbor-internal://file/a b/c%20d.txt#frag?q=1");

    const SplitNode restored = SplitNode::fromJson(leaf.toJson());
    QVERIFY(restored.isLeaf());
    QCOMPARE(restored.url, leaf.url);
    QVERIFY(restored == leaf);

    // The url is part of a leaf's identity: two panes showing different files
    // are not the same pane, or a reload could silently adopt the wrong one.
    SplitNode other = leaf;
    other.url = QStringLiteral("codeharbor-internal://file/other.txt");
    QVERIFY(!(other == leaf));

    // It survives nesting, which is where a split puts it.
    SplitNode split;
    split.children = {leaf, SplitNode{}};
    split.ratios = {0.5, 0.5};
    const SplitNode deep = SplitNode::fromJson(split.toJson());
    QCOMPARE(deep.children.at(0).url, leaf.url);
}

// Backwards compatibility: every layout already stored on a server predates the
// url field. Reading one must not invent a url, and re-writing it must not
// change the bytes - otherwise the first launch after an upgrade rewrites every
// saved layout.
void TstModels::splitTreeWithoutUrlIsUnchangedByTheField()
{
    const QJsonObject legacy = QJsonDocument::fromJson(R"({
        "type": "split", "orientation": "horizontal", "ratios": [0.5, 0.5],
        "children": [
            {"type": "leaf", "paneId": "viewer-1"},
            {"type": "leaf", "paneId": "viewer-2"}
        ]
    })").object();

    const SplitNode parsed = SplitNode::fromJson(legacy);
    QVERIFY(parsed.children.at(0).url.isEmpty());
    QCOMPARE(QJsonDocument(parsed.toJson()).toJson(QJsonDocument::Compact),
             QJsonDocument(legacy).toJson(QJsonDocument::Compact));
}

// A split's paneId is meaningless and dropped by toJson(); equality must ignore
// it so the round-trip is exact for a split that happens to carry a stray paneId.
void TstModels::splitTreeSplitPaneIdIgnoredOnRoundTrip()
{
    SplitNode child;
    child.paneId = QStringLiteral("A");

    SplitNode split;
    split.paneId = QStringLiteral("ignored-on-splits");
    split.orientation = SplitOrientation::Vertical;
    split.children = {child};
    split.ratios = {1.0};

    const SplitNode restored = SplitNode::fromJson(split.toJson());
    QVERIFY(!restored.isLeaf());
    QVERIFY(restored.paneId.isEmpty()); // toJson never persists a split's paneId
    QVERIFY(restored == split);
}

void TstModels::splitTreeUnicodePaneIdRoundTrips()
{
    SplitNode leaf;
    leaf.paneId = QStringLiteral("面板-\u00e9\u2603\U0001F680");

    SplitNode split;
    split.orientation = SplitOrientation::Horizontal;
    split.children = {leaf, SplitNode{}};
    split.ratios = {0.5, 0.5};

    const SplitNode restored = SplitNode::fromJson(split.toJson());
    QVERIFY(restored == split);
    QCOMPARE(restored.children.at(0).paneId, leaf.paneId);
}

// Deeply-nested JSON (possibly from a remote/corrupt source) must be rejected
// rather than recursing until the stack overflows.
void TstModels::splitTreeRejectsPathologicalDepth()
{
    QJsonObject node;
    node[QStringLiteral("type")] = QStringLiteral("leaf");
    node[QStringLiteral("paneId")] = QStringLiteral("deep");
    for (int i = 0; i < 400; ++i) {
        QJsonObject split;
        split[QStringLiteral("type")] = QStringLiteral("split");
        split[QStringLiteral("orientation")] = QStringLiteral("horizontal");
        split[QStringLiteral("children")] = QJsonArray{node};
        split[QStringLiteral("ratios")] = QJsonArray{1.0};
        node = split;
    }
    QVERIFY(SplitNode::fromJson(node) == SplitNode{});
}

// The depth guard must not reject a legitimately (if deeply) nested tree.
void TstModels::splitTreeAcceptsModerateDepth()
{
    SplitNode node;
    node.paneId = QStringLiteral("leaf");
    for (int i = 0; i < 100; ++i) {
        SplitNode split;
        split.orientation = SplitOrientation::Vertical;
        split.children = {node};
        split.ratios = {1.0};
        node = split;
    }
    QVERIFY(SplitNode::fromJson(node.toJson()) == node);
}

// index() must reject any column other than 0 (the model is single-column).
void TstModels::sessionsModelRejectsNonZeroColumn()
{
    SessionsModel model;
    SessionRow s;
    s.session.name = QStringLiteral("s");
    GroupRow g;
    g.group.name = QStringLiteral("G");
    g.sessions = {s};
    model.setGroups({g});

    QVERIFY(!model.index(0, 1).isValid());
    QVERIFY(!model.index(0, -1).isValid());
    const QModelIndex group = model.index(0, 0);
    QVERIFY(group.isValid());
    QVERIFY(!model.index(0, 1, group).isValid());
}

// data() on an invalid index yields an invalid QVariant for every role.
void TstModels::sessionsModelDataOnInvalidIndex()
{
    SessionsModel model;
    QVERIFY(!model.data(QModelIndex(), SessionsModel::NameRole).isValid());
    QVERIFY(!model.data(QModelIndex(), Qt::DisplayRole).isValid());
    QVERIFY(!model.data(QModelIndex(), SessionsModel::RowStateRole).isValid());
}

// setGroups() replaces the whole model: it must emit a model reset each call and
// reflect the new contents, discarding the previous ones.
void TstModels::sessionsModelSetGroupsTwiceResets()
{
    SessionsModel model;

    GroupRow first;
    first.group.name = QStringLiteral("First");
    first.sessions = {SessionRow{}, SessionRow{}};
    model.setGroups({first});
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.rowCount(model.index(0, 0)), 2);

    QSignalSpy aboutTo(&model, &QAbstractItemModel::modelAboutToBeReset);
    QSignalSpy done(&model, &QAbstractItemModel::modelReset);

    GroupRow a;
    a.group.name = QStringLiteral("A");
    GroupRow b;
    b.group.name = QStringLiteral("B");
    b.sessions = {SessionRow{}};
    model.setGroups({a, b});

    QCOMPARE(aboutTo.count(), 1);
    QCOMPARE(done.count(), 1);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.data(model.index(0, 0), SessionsModel::NameRole).toString(),
             QStringLiteral("A"));
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
    QCOMPARE(model.rowCount(model.index(1, 0)), 1);
}

// Terminals present but none in any notable state (e.g. unloaded/unknown) reduce
// to Disconnected, matching the empty-set case (SPEC 4.2 lowest priority).
void TstModels::aggregateRowStateAllFalseIsDisconnected()
{
    const QVector<TerminalStatus> terminals = {
        terminal(TerminalState::Unloaded, AgentState::Unknown),
        terminal(TerminalState::Disconnected, AgentState::Stopped),
    };
    QCOMPARE(static_cast<int>(SessionsModel::aggregateSessionState(terminals)),
             static_cast<int>(SessionRowState::Disconnected));

    // anyWaitingInput without anyError still yields WaitingForInput.
    QCOMPARE(static_cast<int>(SessionsModel::aggregateSessionState(
                 {terminal(TerminalState::Disconnected, AgentState::WaitingInput)})),
             static_cast<int>(SessionRowState::WaitingForInput));
}

// The precedence reducer has five boolean inputs, so its behaviour is fully
// described by 32 cases; the scenario tests above only sample a handful of them
// through SessionsModel::aggregateSessionState. Enumerate all 32 against an
// independent restatement of the SPEC 4.2 order, written lowest-priority-first
// so it shares no structure with the implementation's highest-priority-first
// early returns. Any swapped pair of levels fails here.
void TstModels::aggregateRowStateCoversEveryInputCombination()
{
    for (int mask = 0; mask < 32; ++mask) {
        const bool anyError = (mask & 1) != 0;
        const bool anyWaitingInput = (mask & 2) != 0;
        const bool anyRunning = (mask & 4) != 0;
        const bool anyFinishedUnseen = (mask & 8) != 0;
        const bool anyConnected = (mask & 16) != 0;

        SessionRowState expected = SessionRowState::Disconnected;
        if (anyConnected) expected = SessionRowState::Idle;
        if (anyFinishedUnseen) expected = SessionRowState::FinishedUnseen;
        if (anyRunning) expected = SessionRowState::Running;
        if (anyWaitingInput) expected = SessionRowState::WaitingForInput;
        if (anyError) expected = SessionRowState::Error;

        const SessionRowState got = aggregateRowState(
                anyError, anyWaitingInput, anyRunning, anyFinishedUnseen, anyConnected);
        QVERIFY2(got == expected,
                 qPrintable(QStringLiteral("mask %1: got %2, expected %3")
                                    .arg(mask)
                                    .arg(toString(got), toString(expected))));
    }

    // The enum's declaration order IS the priority order (lower ordinal = higher
    // priority), which the sidebar's colour/glyph table in SessionRow.qml relies
    // on. Reordering the enumerators would silently repaint every row.
    QVERIFY(static_cast<int>(SessionRowState::Error)
            < static_cast<int>(SessionRowState::WaitingForInput));
    QVERIFY(static_cast<int>(SessionRowState::WaitingForInput)
            < static_cast<int>(SessionRowState::Running));
    QVERIFY(static_cast<int>(SessionRowState::Running)
            < static_cast<int>(SessionRowState::FinishedUnseen));
    QVERIFY(static_cast<int>(SessionRowState::FinishedUnseen)
            < static_cast<int>(SessionRowState::Idle));
    QVERIFY(static_cast<int>(SessionRowState::Idle)
            < static_cast<int>(SessionRowState::Disconnected));
}

// These strings cross process and language boundaries, so they are contracts and
// not display text: toString(AgentState) must match the AGENT_STATES union the
// remote agent-status bridge emits (remote/src/events.ts), toString(TerminalState)
// is what TerminalBridge::connectionState hands the xterm.js page and
// TerminalPaneView.qml compares against, and toString(FileState) is what
// EditorController's fileState property publishes to the Monaco page. Renaming
// any of them breaks a consumer that this test suite cannot see, so the tables
// are pinned here.
void TstModels::stateStringsArePinnedWireValues()
{
    QCOMPARE(toString(TerminalState::Unloaded), QStringLiteral("unloaded"));
    QCOMPARE(toString(TerminalState::OpeningChannel), QStringLiteral("opening_channel"));
    QCOMPARE(toString(TerminalState::AttachingTmux), QStringLiteral("attaching_tmux"));
    QCOMPARE(toString(TerminalState::Ready), QStringLiteral("ready"));
    QCOMPARE(toString(TerminalState::Disconnected), QStringLiteral("disconnected"));
    QCOMPARE(toString(TerminalState::Error), QStringLiteral("error"));

    QCOMPARE(toString(AgentState::Starting), QStringLiteral("starting"));
    QCOMPARE(toString(AgentState::Running), QStringLiteral("running"));
    QCOMPARE(toString(AgentState::WaitingInput), QStringLiteral("waiting_input"));
    QCOMPARE(toString(AgentState::IdleUnseen), QStringLiteral("idle_unseen"));
    QCOMPARE(toString(AgentState::Idle), QStringLiteral("idle"));
    QCOMPARE(toString(AgentState::Error), QStringLiteral("error"));
    QCOMPARE(toString(AgentState::Stopped), QStringLiteral("stopped"));
    QCOMPARE(toString(AgentState::Unknown), QStringLiteral("unknown"));

    QCOMPARE(toString(SessionRowState::Error), QStringLiteral("error"));
    QCOMPARE(toString(SessionRowState::WaitingForInput), QStringLiteral("waiting_for_input"));
    QCOMPARE(toString(SessionRowState::Running), QStringLiteral("running"));
    QCOMPARE(toString(SessionRowState::FinishedUnseen), QStringLiteral("finished_unseen"));
    QCOMPARE(toString(SessionRowState::Idle), QStringLiteral("idle"));
    QCOMPARE(toString(SessionRowState::Disconnected), QStringLiteral("disconnected"));

    QCOMPARE(toString(FileState::Loading), QStringLiteral("loading"));
    QCOMPARE(toString(FileState::Clean), QStringLiteral("clean"));
    QCOMPARE(toString(FileState::Modified), QStringLiteral("modified"));
    QCOMPARE(toString(FileState::Saving), QStringLiteral("saving"));
    QCOMPARE(toString(FileState::Saved), QStringLiteral("saved"));
    QCOMPARE(toString(FileState::ExternallyModified), QStringLiteral("externally_modified"));
    QCOMPARE(toString(FileState::Conflict), QStringLiteral("conflict"));
    QCOMPARE(toString(FileState::ReadOnly), QStringLiteral("read_only"));
    QCOMPARE(toString(FileState::Error), QStringLiteral("error"));
    QCOMPARE(toString(FileState::Disconnected), QStringLiteral("disconnected"));

    // Within one enum every name must be distinct: two enumerators sharing a
    // string make the states indistinguishable to the page reading them, which a
    // copy-paste in the switch would otherwise produce silently.
    const auto allDistinct = [](const QStringList &names) {
        return QSet<QString>(names.cbegin(), names.cend()).size() == names.size();
    };
    QVERIFY(allDistinct({toString(TerminalState::Unloaded),
                         toString(TerminalState::OpeningChannel),
                         toString(TerminalState::AttachingTmux), toString(TerminalState::Ready),
                         toString(TerminalState::Disconnected), toString(TerminalState::Error)}));
    QVERIFY(allDistinct({toString(AgentState::Starting), toString(AgentState::Running),
                         toString(AgentState::WaitingInput), toString(AgentState::IdleUnseen),
                         toString(AgentState::Idle), toString(AgentState::Error),
                         toString(AgentState::Stopped), toString(AgentState::Unknown)}));
    QVERIFY(allDistinct({toString(SessionRowState::Error),
                         toString(SessionRowState::WaitingForInput),
                         toString(SessionRowState::Running),
                         toString(SessionRowState::FinishedUnseen),
                         toString(SessionRowState::Idle),
                         toString(SessionRowState::Disconnected)}));
    QVERIFY(allDistinct({toString(FileState::Loading), toString(FileState::Clean),
                         toString(FileState::Modified), toString(FileState::Saving),
                         toString(FileState::Saved), toString(FileState::ExternallyModified),
                         toString(FileState::Conflict), toString(FileState::ReadOnly),
                         toString(FileState::Error), toString(FileState::Disconnected)}));
}

// roleNames() is the ONLY way QML can reach a role: a role that data() answers
// but roleNames() does not name is unreachable from the sidebar delegates, and a
// name change breaks the delegates' bindings. Pin both directions, plus the fact
// that a role a given row kind does not carry yields an INVALID QVariant rather
// than a default-constructed one a view would render as real data.
void TstModels::sessionsModelRoleNamesCoverEveryServedRole()
{
    SessionsModel model;
    SessionRow s;
    s.session.id = DevSessionId{QStringLiteral("s1")};
    s.session.name = QStringLiteral("S");
    s.subtitle = QStringLiteral("sub");
    GroupRow g;
    g.group.id = GroupId{QStringLiteral("g1")};
    g.group.name = QStringLiteral("G");
    g.sessions = {s};
    model.setGroups({g});

    const QModelIndex group = model.index(0, 0);
    const QModelIndex session = model.index(0, 0, group);
    const QHash<int, QByteArray> names = model.roleNames();

    const QVector<int> served = {Qt::DisplayRole,
                                 SessionsModel::NameRole,
                                 SessionsModel::SubtitleRole,
                                 SessionsModel::RowStateRole,
                                 SessionsModel::IsGroupRole,
                                 SessionsModel::CollapsedRole,
                                 SessionsModel::IdRole,
                                 SessionsModel::GroupIdRole};
    QCOMPARE(names.size(), served.size());
    for (int role : served) {
        QVERIFY2(names.contains(role), "roleNames() omits a role that data() serves");
        QVERIFY2(model.data(group, role).isValid() || model.data(session, role).isValid(),
                 "roleNames() names a role no row kind answers");
    }

    // The exact names SessionsSidebar.qml, GroupHeader.qml and SessionRow.qml bind to.
    QCOMPARE(names.value(SessionsModel::NameRole), QByteArrayLiteral("name"));
    QCOMPARE(names.value(SessionsModel::SubtitleRole), QByteArrayLiteral("subtitle"));
    QCOMPARE(names.value(SessionsModel::RowStateRole), QByteArrayLiteral("rowState"));
    QCOMPARE(names.value(SessionsModel::IsGroupRole), QByteArrayLiteral("isGroup"));
    QCOMPARE(names.value(SessionsModel::CollapsedRole), QByteArrayLiteral("collapsed"));
    QCOMPARE(names.value(SessionsModel::IdRole), QByteArrayLiteral("itemId"));
    QCOMPARE(names.value(SessionsModel::GroupIdRole), QByteArrayLiteral("groupId"));

    // Group rows carry no subtitle or aggregate state; session rows carry no
    // collapsed flag; nobody serves a role the model never declared.
    QVERIFY(!model.data(group, SessionsModel::SubtitleRole).isValid());
    QVERIFY(!model.data(group, SessionsModel::RowStateRole).isValid());
    QVERIFY(!model.data(session, SessionsModel::CollapsedRole).isValid());
    QVERIFY(!model.data(group, Qt::DecorationRole).isValid());
    QVERIFY(!model.data(session, Qt::DecorationRole).isValid());

    // Qt::DisplayRole is an alias for the row's name on both row kinds.
    QCOMPARE(model.data(group, Qt::DisplayRole).toString(), QStringLiteral("G"));
    QCOMPARE(model.data(session, Qt::DisplayRole).toString(), QStringLiteral("S"));
}

// Two rejection branches in SplitTree.cpp's parser that no other case reaches: a
// children[] entry that is not a JSON object, and a ratios[] entry that is not a
// number. Both can only come from corrupt or hostile stored/remote data, and both
// must sink the whole tree rather than being coerced into something plausible.
void TstModels::splitTreeRejectsNonObjectChildAndNonNumericRatio()
{
    QJsonObject leaf;
    leaf[QStringLiteral("type")] = QStringLiteral("leaf");
    leaf[QStringLiteral("paneId")] = QStringLiteral("A");

    // children: [leaf, 42] - a number where a subtree belongs.
    QJsonObject numericChild;
    numericChild[QStringLiteral("type")] = QStringLiteral("split");
    numericChild[QStringLiteral("orientation")] = QStringLiteral("horizontal");
    numericChild[QStringLiteral("children")] = QJsonArray{leaf, 42};
    numericChild[QStringLiteral("ratios")] = QJsonArray{0.5, 0.5};
    QVERIFY(SplitNode::fromJson(numericChild) == SplitNode{});

    // ratios: [0.5, "0.5"] - a string where a number belongs. A silently
    // coerced 0 here would divide the pane geometry by zero.
    QJsonObject stringRatio;
    stringRatio[QStringLiteral("type")] = QStringLiteral("split");
    stringRatio[QStringLiteral("orientation")] = QStringLiteral("horizontal");
    stringRatio[QStringLiteral("children")] = QJsonArray{leaf, leaf};
    stringRatio[QStringLiteral("ratios")] =
            QJsonArray{0.5, QStringLiteral("0.5")};
    QVERIFY(SplitNode::fromJson(stringRatio) == SplitNode{});

    // ratios: [0.5, true] - a boolean is not a number either.
    QJsonObject boolRatio = stringRatio;
    boolRatio[QStringLiteral("ratios")] = QJsonArray{0.5, true};
    QVERIFY(SplitNode::fromJson(boolRatio) == SplitNode{});
}

// An agent that has been asked to start but has not reported Running yet is
// still work in flight, so the sidebar shows Running rather than dropping to
// Idle for the gap. Nothing else covers the Starting -> Running mapping in
// SessionsModel::aggregateSessionState.
void TstModels::startingAgentCountsAsRunning()
{
    QCOMPARE(static_cast<int>(SessionsModel::aggregateSessionState(
                 {terminal(TerminalState::Ready, AgentState::Starting)})),
             static_cast<int>(SessionRowState::Running));

    // It outranks a sibling terminal that has finished with unseen output...
    QCOMPARE(static_cast<int>(SessionsModel::aggregateSessionState(
                 {terminal(TerminalState::Ready, AgentState::Starting),
                  terminal(TerminalState::Ready, AgentState::IdleUnseen)})),
             static_cast<int>(SessionRowState::Running));

    // ...and is outranked by one waiting for the user.
    QCOMPARE(static_cast<int>(SessionsModel::aggregateSessionState(
                 {terminal(TerminalState::Ready, AgentState::Starting),
                  terminal(TerminalState::Ready, AgentState::WaitingInput)})),
             static_cast<int>(SessionRowState::WaitingForInput));
}

// The AgentState ordinals are what actually cross into the user interface —
// ch::AgentStatusMonitor::stateFor() returns an int and its agentStateChanged
// signal carries an int — and the wire words are what the remote agent-status
// bridge sends. Both sides therefore have to agree on the same words in the same
// order, and the TypeScript half of that agreement lives in a file no C++
// compiler ever looks at (remote/src/events.ts). So read it here and compare.
//
// This fails if either side is reordered, if a state is added to only one side,
// or if a word is spelled differently. Note it deliberately checks the file's
// literal AGENT_STATES array rather than a copy pinned in this test: a copy
// would only re-state what stateStringsArePinnedWireValues() already checks and
// could not notice an edit made on the TypeScript side.
void TstModels::agentStateWireWordsMatchRemoteEventsTs()
{
    // CH_REPO_ROOT is the configure-time source directory (see this directory's
    // CMakeLists.txt), the same mechanism tst_workspacedb uses to reach
    // repository files. A missing file is a hard failure, never a silent skip:
    // the whole point of this test is that it cannot be quietly disabled.
    QFile events(QStringLiteral(CH_REPO_ROOT "/remote/src/events.ts"));
    QVERIFY2(events.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(QStringLiteral("cannot open %1: %2")
                                .arg(events.fileName(), events.errorString())));
    const QString source = QString::fromUtf8(events.readAll());

    // Cut out exactly the AGENT_STATES array literal, then take the quoted words
    // inside it in order. Anchoring on both the opening and the closing bracket
    // keeps unrelated string literals elsewhere in the file out of the list.
    // Escaped rather than raw string literals on purpose: moc's own preprocessor
    // does not understand a raw string used as a macro argument and fails with
    // "missing ')' in macro usage" on QStringLiteral(R"(...)").
    static const QRegularExpression arrayRe(
            QStringLiteral("export const AGENT_STATES\\s*=\\s*\\[([^\\]]*)\\]"));
    const QRegularExpressionMatch arrayMatch = arrayRe.match(source);
    QVERIFY2(arrayMatch.hasMatch(),
             qPrintable(QStringLiteral("no AGENT_STATES array literal in %1")
                                .arg(events.fileName())));

    static const QRegularExpression wordRe(QStringLiteral("\"([^\"]*)\""));
    QStringList remoteWords;
    QRegularExpressionMatchIterator it = wordRe.globalMatch(arrayMatch.captured(1));
    while (it.hasNext())
        remoteWords << it.next().captured(1);
    QVERIFY2(!remoteWords.isEmpty(),
             qPrintable(QStringLiteral("AGENT_STATES in %1 parsed as empty")
                                .arg(events.fileName())));

    // Every C++ enumerator, walked by ordinal so the comparison covers order and
    // count as well as spelling.
    QStringList cppWords;
    cppWords.reserve(kAgentStateCount);
    for (int i = 0; i < kAgentStateCount; ++i)
        cppWords << toString(static_cast<AgentState>(i));

    QCOMPARE(cppWords, remoteWords);
}

// The incremental update path must NOT reset the model. It updates per-terminal
// state in place and emits a targeted dataChanged() (RowStateRole) for only the
// session rows whose aggregate state actually moved - so delegates and the
// id-tracked selection survive. A drifted structure falls back to a full reset.
void TstModels::updateTerminalStatesEmitsTargetedDataChangedNotReset()
{
    SessionsModel model;

    const auto session = [](const QString &id, TerminalState conn, AgentState agent) {
        SessionRow row;
        row.session.id = DevSessionId{id};
        row.session.name = id;
        row.terminals = {terminal(conn, agent)};
        return row;
    };

    GroupRow g;
    g.group.id = GroupId{QStringLiteral("g1")};
    g.group.name = QStringLiteral("G");
    g.sessions = {session(QStringLiteral("s1"), TerminalState::Ready, AgentState::Running),
                  session(QStringLiteral("s2"), TerminalState::Ready, AgentState::Idle)};
    model.setGroups({g});

    const QModelIndex groupIndex = model.index(0, 0);
    const QModelIndex s1Index = model.index(0, 0, groupIndex);
    QCOMPARE(model.data(s1Index, SessionsModel::RowStateRole).toInt(),
             static_cast<int>(SessionRowState::Running));

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

    // Same structure/ids; only s1's terminal moves (Running -> WaitingForInput),
    // s2 is untouched.
    GroupRow updated;
    updated.group.id = GroupId{QStringLiteral("g1")};
    updated.group.name = QStringLiteral("G");
    updated.sessions = {session(QStringLiteral("s1"), TerminalState::Ready, AgentState::WaitingInput),
                        session(QStringLiteral("s2"), TerminalState::Ready, AgentState::Idle)};
    model.updateTerminalStates({updated});

    // No reset; exactly one targeted change for s1 carrying RowStateRole.
    QCOMPARE(reset.count(), 0);
    QCOMPARE(changed.count(), 1);
    const QModelIndex topLeft = changed.at(0).at(0).value<QModelIndex>();
    const QModelIndex bottomRight = changed.at(0).at(1).value<QModelIndex>();
    QCOMPARE(topLeft, s1Index);
    QCOMPARE(bottomRight, s1Index);
    QVERIFY(changed.at(0).at(2).value<QList<int>>().contains(SessionsModel::RowStateRole));
    QCOMPARE(model.data(s1Index, SessionsModel::RowStateRole).toInt(),
             static_cast<int>(SessionRowState::WaitingForInput));

    // A structural drift (different session id) cannot be applied in place, so it
    // falls back to a full reset instead of a bogus targeted change.
    QSignalSpy reset2(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changed2(&model, &QAbstractItemModel::dataChanged);
    GroupRow drifted;
    drifted.group.id = GroupId{QStringLiteral("g1")};
    drifted.group.name = QStringLiteral("G");
    drifted.sessions = {session(QStringLiteral("s1"), TerminalState::Ready, AgentState::Running),
                        session(QStringLiteral("sX"), TerminalState::Ready, AgentState::Idle)};
    model.updateTerminalStates({drifted});
    QCOMPARE(reset2.count(), 1);
    QCOMPARE(changed2.count(), 0);
}

// updateTerminalStates may only patch rows in place while the structure it is
// handed still matches the one held. Three different kinds of drift can reach it
// and each must fall back to a full reset; only the session-id case above was
// covered, so a regression in the group-count, group-id or session-count check
// would silently patch terminal state onto the WRONG rows.
void TstModels::updateTerminalStatesFallsBackForEveryStructuralDrift()
{
    const auto session = [](const QString &id, AgentState agent) {
        SessionRow row;
        row.session.id = DevSessionId{id};
        row.session.name = id;
        row.terminals = {terminal(TerminalState::Ready, agent)};
        return row;
    };
    const auto group = [](const QString &id, QVector<SessionRow> sessions) {
        GroupRow row;
        row.group.id = GroupId{id};
        row.group.name = id;
        row.sessions = std::move(sessions);
        return row;
    };

    const GroupRow baseline = group(QStringLiteral("g1"),
                                    {session(QStringLiteral("s1"), AgentState::Running)});

    // Each entry is a drifted replacement that must trigger a reset.
    const QVector<QVector<GroupRow>> drifts = {
        // A second group appeared: group count differs.
        {baseline, group(QStringLiteral("g2"), {})},
        // Same count, different group id (a group was replaced wholesale).
        {group(QStringLiteral("gX"), {session(QStringLiteral("s1"), AgentState::Running)})},
        // Same group, an extra session: session count differs.
        {group(QStringLiteral("g1"),
               {session(QStringLiteral("s1"), AgentState::Running),
                session(QStringLiteral("s2"), AgentState::Idle)})},
    };

    for (const QVector<GroupRow> &drift : drifts) {
        SessionsModel model;
        model.setGroups({baseline});

        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.updateTerminalStates(drift);

        QCOMPARE(reset.count(), 1);
        QCOMPARE(changed.count(), 0);
        // The drifted contents are what the model now holds.
        QCOMPARE(model.rowCount(), static_cast<int>(drift.size()));
        QCOMPARE(model.data(model.index(0, 0), SessionsModel::IdRole).toString(),
                 drift.at(0).group.id.value);
        QCOMPARE(model.rowCount(model.index(0, 0)),
                 static_cast<int>(drift.at(0).sessions.size()));
    }
}

// The other half of the targeted-update contract: a refresh that leaves a row's
// aggregate state where it was must emit NOTHING for that row. Every emitted
// dataChanged re-evaluates the delegate's bindings, and agent status refreshes
// arrive continuously, so signalling unchanged rows would undo the whole point
// of not resetting the model.
void TstModels::updateTerminalStatesStaysSilentWhenTheAggregateDoesNotMove()
{
    SessionsModel model;

    SessionRow s1;
    s1.session.id = DevSessionId{QStringLiteral("s1")};
    s1.session.name = QStringLiteral("s1");
    s1.terminals = {terminal(TerminalState::Ready, AgentState::Running)};
    GroupRow g;
    g.group.id = GroupId{QStringLiteral("g1")};
    g.sessions = {s1};
    model.setGroups({g});

    const QModelIndex sessionIndex = model.index(0, 0, model.index(0, 0));

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

    // A different terminal set that still reduces to Running (Starting counts as
    // Running, and an extra Idle terminal is outranked by it).
    GroupRow refreshed = g;
    refreshed.sessions[0].terminals = {terminal(TerminalState::Ready, AgentState::Starting),
                                       terminal(TerminalState::Ready, AgentState::Idle)};
    model.updateTerminalStates({refreshed});

    QCOMPARE(reset.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(model.data(sessionIndex, SessionsModel::RowStateRole).toInt(),
             static_cast<int>(SessionRowState::Running));

    // A refresh that DOES move it still signals, so silence above is selective
    // rather than the update path being broken outright.
    refreshed.sessions[0].terminals = {terminal(TerminalState::Ready, AgentState::Idle)};
    model.updateTerminalStates({refreshed});
    QCOMPARE(changed.count(), 1);
    QCOMPARE(model.data(sessionIndex, SessionsModel::RowStateRole).toInt(),
             static_cast<int>(SessionRowState::Idle));
}

// Session indices carry their parent group's ROW in the QModelIndex internal id.
// That encoding is the model's only link from a child back to its group, so row 0
// of group 0 and row 0 of group 1 must be different indices that resolve to
// different parents and different data. Getting this wrong shows one group's
// sessions under another.
void TstModels::sessionsModelSessionIndicesAreScopedToTheirGroup()
{
    SessionsModel model;

    const auto session = [](const QString &id) {
        SessionRow row;
        row.session.id = DevSessionId{id};
        row.session.name = id;
        return row;
    };

    GroupRow first;
    first.group.id = GroupId{QStringLiteral("g1")};
    first.sessions = {session(QStringLiteral("a")), session(QStringLiteral("b"))};
    GroupRow second;
    second.group.id = GroupId{QStringLiteral("g2")};
    second.sessions = {session(QStringLiteral("c"))};
    model.setGroups({first, second});

    const QModelIndex g1 = model.index(0, 0);
    const QModelIndex g2 = model.index(1, 0);
    const QModelIndex a = model.index(0, 0, g1);
    const QModelIndex c = model.index(0, 0, g2);

    QVERIFY(a.isValid());
    QVERIFY(c.isValid());
    QVERIFY(a != c); // same row, different parent
    QCOMPARE(model.parent(a), g1);
    QCOMPARE(model.parent(c), g2);
    QCOMPARE(model.data(a, SessionsModel::IdRole).toString(), QStringLiteral("a"));
    QCOMPARE(model.data(c, SessionsModel::IdRole).toString(), QStringLiteral("c"));
    // GroupIdRole on a session row reports the CONTAINING group, not its own id.
    QCOMPARE(model.data(a, SessionsModel::GroupIdRole).toString(), QStringLiteral("g1"));
    QCOMPARE(model.data(c, SessionsModel::GroupIdRole).toString(), QStringLiteral("g2"));
    // Group 2 has one session, so its row 1 does not exist even though group 1's does.
    QVERIFY(model.index(1, 0, g1).isValid());
    QVERIFY(!model.index(1, 0, g2).isValid());
    // Sessions are leaves.
    QVERIFY(!model.hasChildren(a));
    QVERIFY(model.hasChildren(g1));
}

// The sidebar is a single-column tree. columnCount() must say so for the root,
// for a group and for a session, and parent() of an invalid index must be
// invalid rather than a fabricated row -1 index.
void TstModels::sessionsModelIsSingleColumn()
{
    SessionsModel model;
    QCOMPARE(model.columnCount(), 1);
    QVERIFY(!model.parent(QModelIndex()).isValid());

    SessionRow s;
    s.session.name = QStringLiteral("s");
    GroupRow g;
    g.group.name = QStringLiteral("G");
    g.sessions = {s};
    model.setGroups({g});

    const QModelIndex group = model.index(0, 0);
    const QModelIndex session = model.index(0, 0, group);
    QCOMPARE(model.columnCount(group), 1);
    QCOMPARE(model.columnCount(session), 1);
}

// url, like paneId, is persisted for LEAVES only: a split has no content of its
// own. toJson() must drop a split's stray url and equality must ignore it, or a
// tree stops comparing equal to its own round-trip and SessionLayouts rewrites
// the layout on every load.
void TstModels::splitTreeSplitUrlIgnoredOnRoundTrip()
{
    SplitNode child;
    child.paneId = QStringLiteral("A");
    child.url = QStringLiteral("codeharbor-internal://file/a.txt");

    SplitNode split;
    split.url = QStringLiteral("codeharbor-internal://file/ignored-on-splits.txt");
    split.orientation = SplitOrientation::Horizontal;
    split.children = {child};
    split.ratios = {1.0};

    const QJsonObject json = split.toJson();
    QVERIFY(!json.contains(QStringLiteral("url")));

    const SplitNode restored = SplitNode::fromJson(json);
    QVERIFY(!restored.isLeaf());
    QVERIFY(restored.url.isEmpty());
    QVERIFY(restored == split);
    // The child's url, by contrast, is preserved.
    QCOMPARE(restored.children.at(0).url, child.url);
}

// A terminal leaf's identity is the server's `terminal_panes` row id, and it
// rides in the leaf so every client sharing the layout agrees which remote
// shell that leaf owns. It has to survive the round trip exactly, and it has to
// count towards equality: two leaves naming different rows are different panes,
// and treating them as equal would let a reload silently adopt the wrong shell.
void TstModels::splitTreeRoundTripsLeafTerminalPaneId()
{
    SplitNode leaf;
    leaf.paneId = QStringLiteral("terminal-2");
    leaf.terminalPaneId = QStringLiteral("2f1c9a54-0b3e-4a77-9d21-6c8f0e5b1a44");

    const QJsonObject json = leaf.toJson();
    QCOMPARE(json.value(QStringLiteral("terminalPaneId")).toString(),
             leaf.terminalPaneId);

    const SplitNode restored = SplitNode::fromJson(json);
    QVERIFY(restored.isLeaf());
    QCOMPARE(restored.terminalPaneId, leaf.terminalPaneId);
    QVERIFY(restored == leaf);

    // Two leaves differing ONLY in the row id are not the same pane.
    SplitNode other = leaf;
    other.terminalPaneId = QStringLiteral("8ad0b1c2-1111-4222-8333-944455556666");
    QVERIFY(!(other == leaf));

    // A leaf that never had one compares equal to itself and to its own round
    // trip, so an existing layout is not seen as changed by the new field.
    SplitNode bare;
    bare.paneId = QStringLiteral("terminal-1");
    QVERIFY(bare.terminalPaneId.isEmpty());
    QVERIFY(!bare.toJson().contains(QStringLiteral("terminalPaneId")));
    QVERIFY(SplitNode::fromJson(bare.toJson()) == bare);
    QVERIFY(bare == bare);
    QVERIFY(!(bare == leaf));

    // It survives nesting, which is where a split puts it.
    SplitNode split;
    split.children = {leaf, bare};
    split.ratios = {1.0, 1.0};
    const SplitNode deep = SplitNode::fromJson(split.toJson());
    QCOMPARE(deep.children.at(0).terminalPaneId, leaf.terminalPaneId);
    QVERIFY(deep.children.at(1).terminalPaneId.isEmpty());
    QVERIFY(deep == split);
}

// Backwards compatibility: every terminal layout already stored on a server
// predates the row id. Reading one must not invent an id, and re-writing it must
// not change a byte - otherwise the first launch after an upgrade rewrites every
// saved layout, and (worse) a leaf could no longer be recognised as the
// pre-migration leaf that it is.
void TstModels::splitTreeWithoutTerminalPaneIdIsUnchangedByTheField()
{
    const QJsonObject legacy = QJsonDocument::fromJson(R"({
        "type": "split",
        "orientation": "vertical",
        "ratios": [1, 1],
        "children": [
            {"type": "leaf", "paneId": "terminal-1"},
            {"type": "leaf", "paneId": "terminal-2"}
        ]
    })").object();

    const SplitNode parsed = SplitNode::fromJson(legacy);
    QVERIFY(parsed.children.at(0).terminalPaneId.isEmpty());
    QVERIFY(parsed.children.at(1).terminalPaneId.isEmpty());
    QCOMPARE(QJsonDocument(parsed.toJson()).toJson(QJsonDocument::Compact),
             QJsonDocument(legacy).toJson(QJsonDocument::Compact));
}

// terminalPaneId, like paneId and url, is persisted for LEAVES only: a split is
// structure and owns no terminal. toJson() must drop a split's stray one and
// equality must ignore it, or a tree stops comparing equal to its own round trip
// and SessionLayouts rewrites the layout on every load.
void TstModels::splitTreeSplitTerminalPaneIdIgnoredOnRoundTrip()
{
    SplitNode child;
    child.paneId = QStringLiteral("terminal-1");
    child.terminalPaneId = QStringLiteral("row-1");

    SplitNode split;
    split.terminalPaneId = QStringLiteral("ignored-on-splits");
    split.orientation = SplitOrientation::Horizontal;
    split.children = {child};
    split.ratios = {1.0};

    const QJsonObject json = split.toJson();
    QVERIFY(!json.contains(QStringLiteral("terminalPaneId")));

    const SplitNode restored = SplitNode::fromJson(json);
    QVERIFY(!restored.isLeaf());
    QVERIFY(restored.terminalPaneId.isEmpty());
    QVERIFY(restored == split);
    // The child's row id, by contrast, is preserved.
    QCOMPARE(restored.children.at(0).terminalPaneId, child.terminalPaneId);
}

// Defaulted equality on the persisted value types has to compare EVERY member:
// these structs are diffed to decide whether a change needs writing back, so a
// field left out of the comparison is a field whose edits are silently dropped.
// Only DevSession was covered; flip each member of the other three in turn.
void TstModels::workspaceValueTypesCompareEveryField()
{
    Group group;
    group.id = GroupId{QStringLiteral("g1")};
    group.serverId = ServerId{QStringLiteral("srv")};
    group.name = QStringLiteral("Work");
    group.position = 2;
    group.collapsed = false;
    QVERIFY(group == Group(group));
    for (const auto &mutate : QVector<std::function<void(Group &)>>{
                 [](Group &g) { g.id = GroupId{QStringLiteral("other")}; },
                 [](Group &g) { g.serverId = ServerId{QStringLiteral("other")}; },
                 [](Group &g) { g.name = QStringLiteral("other"); },
                 [](Group &g) { g.position = 3; },
                 [](Group &g) { g.collapsed = true; }}) {
        Group changed = group;
        mutate(changed);
        QVERIFY(!(changed == group));
    }

    ViewerPane viewer;
    viewer.id = ViewerPaneId{QStringLiteral("v1")};
    viewer.serverId = ServerId{QStringLiteral("srv")};
    viewer.devSessionId = DevSessionId{QStringLiteral("s1")};
    viewer.url = QStringLiteral("file:///a");
    viewer.handler = QStringLiteral("markdown");
    viewer.title = QStringLiteral("A");
    viewer.position = 1;
    QVERIFY(viewer == ViewerPane(viewer));
    for (const auto &mutate : QVector<std::function<void(ViewerPane &)>>{
                 [](ViewerPane &p) { p.id = ViewerPaneId{QStringLiteral("other")}; },
                 [](ViewerPane &p) { p.serverId = ServerId{QStringLiteral("other")}; },
                 [](ViewerPane &p) { p.devSessionId = DevSessionId{QStringLiteral("other")}; },
                 [](ViewerPane &p) { p.url = QStringLiteral("file:///b"); },
                 [](ViewerPane &p) { p.handler = QStringLiteral("web"); },
                 [](ViewerPane &p) { p.title = QStringLiteral("B"); },
                 [](ViewerPane &p) { p.position = 2; }}) {
        ViewerPane changed = viewer;
        mutate(changed);
        QVERIFY(!(changed == viewer));
    }

    TerminalPane term;
    term.id = TerminalId{QStringLiteral("t1")};
    term.serverId = ServerId{QStringLiteral("srv")};
    term.devSessionId = DevSessionId{QStringLiteral("s1")};
    term.name = QStringLiteral("shell");
    term.workingDirectory = QStringLiteral("/wd");
    term.tmuxTarget = QStringLiteral("ch:0.0");
    term.startupCommand = QStringLiteral("git status");
    term.harness = QStringLiteral("oh-my-pi");
    term.position = 1;
    QVERIFY(term == TerminalPane(term));
    for (const auto &mutate : QVector<std::function<void(TerminalPane &)>>{
                 [](TerminalPane &p) { p.id = TerminalId{QStringLiteral("other")}; },
                 [](TerminalPane &p) { p.serverId = ServerId{QStringLiteral("other")}; },
                 [](TerminalPane &p) { p.devSessionId = DevSessionId{QStringLiteral("other")}; },
                 [](TerminalPane &p) { p.name = QStringLiteral("other"); },
                 [](TerminalPane &p) { p.workingDirectory = QStringLiteral("/other"); },
                 [](TerminalPane &p) { p.tmuxTarget = QStringLiteral("ch:1.0"); },
                 [](TerminalPane &p) { p.startupCommand = QStringLiteral("ls"); },
                 [](TerminalPane &p) { p.harness = QStringLiteral("generic"); },
                 [](TerminalPane &p) { p.position = 2; }}) {
        TerminalPane changed = term;
        mutate(changed);
        QVERIFY(!(changed == term));
    }
}

// updateTerminalStates has to put the WHOLE model into its new state before the
// first dataChanged goes out. A receiver that reads another row from inside its
// slot - a proxy model recomputing a filter, a delegate re-evaluating a binding -
// would otherwise be shown rows that are still carrying their previous terminal
// state, and a receiver that reacted by calling setGroups() would free the
// vectors the update loop is still walking.
void TstModels::updateTerminalStatesFinishesEveryRowBeforeSignalling()
{
    SessionsModel model;

    const auto session = [](const QString &id, AgentState agent) {
        SessionRow row;
        row.session.id = DevSessionId{id};
        row.session.name = id;
        row.terminals = {terminal(TerminalState::Ready, agent)};
        return row;
    };

    GroupRow g;
    g.group.id = GroupId{QStringLiteral("g1")};
    g.sessions = {session(QStringLiteral("s1"), AgentState::Idle),
                  session(QStringLiteral("s2"), AgentState::Idle)};
    model.setGroups({g});

    const QModelIndex groupIndex = model.index(0, 0);
    const QModelIndex s1 = model.index(0, 0, groupIndex);
    const QModelIndex s2 = model.index(1, 0, groupIndex);

    // Record what the model reports for BOTH rows each time a change is
    // announced, including the very first announcement.
    QVector<int> s1AtSignal;
    QVector<int> s2AtSignal;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&](const QModelIndex &, const QModelIndex &, const QList<int> &) {
                         s1AtSignal.append(model.data(s1, SessionsModel::RowStateRole).toInt());
                         s2AtSignal.append(model.data(s2, SessionsModel::RowStateRole).toInt());
                     });

    // Both rows move Idle -> Error, so two signals go out.
    GroupRow refreshed = g;
    refreshed.sessions = {session(QStringLiteral("s1"), AgentState::Error),
                          session(QStringLiteral("s2"), AgentState::Error)};
    model.updateTerminalStates({refreshed});

    const int error = static_cast<int>(SessionRowState::Error);
    QCOMPARE(s1AtSignal, QVector<int>({error, error}));
    QCOMPARE(s2AtSignal, QVector<int>({error, error}));
}

QTEST_GUILESS_MAIN(TstModels)
#include "tst_models.moc"
