#include <QtTest/QtTest>

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <limits>

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
        terminal(TerminalState::Connecting, AgentState::Stopped),
    };
    QCOMPARE(static_cast<int>(SessionsModel::aggregateSessionState(terminals)),
             static_cast<int>(SessionRowState::Disconnected));

    // anyWaitingInput without anyError still yields WaitingForInput.
    QCOMPARE(static_cast<int>(SessionsModel::aggregateSessionState(
                 {terminal(TerminalState::Disconnected, AgentState::WaitingInput)})),
             static_cast<int>(SessionRowState::WaitingForInput));
}

QTEST_GUILESS_MAIN(TstModels)
#include "tst_models.moc"
