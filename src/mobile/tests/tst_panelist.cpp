#include <QtTest/QtTest>

#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "PaneListModel.h"
#include "SplitTree.h"

using namespace ch;

namespace {

// The persisted wire shapes, spelled exactly as SplitNode::tryToJson() writes
// them (src/models/SplitTree.cpp): a leaf carries `paneId` plus the optional
// `url`, `terminalPaneId` and `customTitle`, and a split carries `orientation`,
// `children` and `ratios`. Written out here rather than produced through
// SessionLayouts so a case can build the shapes SessionLayouts would refuse.
QVariantMap leaf(const QString& paneId)
{
    return QVariantMap{{QStringLiteral("type"), QStringLiteral("leaf")},
                       {QStringLiteral("paneId"), paneId}};
}

QVariantMap viewerLeaf(const QString& paneId, const QString& url)
{
    QVariantMap node = leaf(paneId);
    if (!url.isEmpty())
        node.insert(QStringLiteral("url"), url);
    return node;
}

QVariantMap terminalLeaf(const QString& paneId, const QString& rowId,
                         const QString& customTitle = QString())
{
    QVariantMap node = leaf(paneId);
    if (!rowId.isEmpty())
        node.insert(QStringLiteral("terminalPaneId"), rowId);
    if (!customTitle.isEmpty())
        node.insert(QStringLiteral("customTitle"), customTitle);
    return node;
}

QVariantMap split(const QString& orientation, const QVariantList& children)
{
    QVariantList ratios;
    for (int i = 0; i < children.size(); ++i)
        ratios.append(1.0);
    return QVariantMap{{QStringLiteral("type"), QStringLiteral("split")},
                       {QStringLiteral("orientation"), orientation},
                       {QStringLiteral("children"), children},
                       {QStringLiteral("ratios"), ratios}};
}

// Every row's value for one role, top to bottom. The assertions below are almost
// all about ORDER, so they compare whole lists rather than probing indexes.
QStringList column(const PaneListModel& model, int role)
{
    QStringList values;
    for (int row = 0; row < model.rowCount(); ++row)
        values << model.data(model.index(row, 0), role).toString();
    return values;
}

} // namespace

// ch::PaneListModel is the mobile client's whole layout engine: the two
// server-authoritative region trees, flattened into the one ordered menu a
// single-pane shell can offer. What is asserted here is therefore the contract
// the pane picker and ch::MobileAppController both stand on - the ORDER, the
// paneKey spelling, the titles and kinds - plus the refusals, because the trees
// arrive from a server and a corrupt one must produce an empty list rather than a
// crash or a half-walked tree.
class TstPaneList : public QObject {
    Q_OBJECT
private slots:
    void flattensViewerRegionFirstThenDepthFirst();
    void paneKeyIsRegionQualifiedBecauseSlotLabelsRepeat();
    void viewerTitlesAreDerivedFromTheStoredUrl();
    void terminalTitlePrefersTheUsersOwnTitle();
    void kindsComeFromTheSharedViewerRegistry();
    void aMalformedRegionContributesNothingAndDoesNotTakeTheOtherWithIt();
    void anOverDeepTreeIsRefusedRatherThanRecursedInto();
    void theEmptyPlaceholderLeafIsNotAPane();
    void aNullRegionIsNotLoadedRatherThanEmpty();
    void republishingAnUnchangedTreeKeepsEveryRowInPlace();
    void paneByKeyAnswersTheRowOrAnEmptyMapForAKeyItDoesNotHold();
    void theDepthBoundIsCountedExactlyAsTheParserCountsIt();
    void aRepeatedPaneIdInOneRegionYieldsOneRow();
    void oddViewerUrlsStillProduceAUsableTitle();
};

// Depth-first, viewer region first, in stored child order. This is the ordering
// every other assertion in this file (and the picker's row stability) depends on.
void TstPaneList::flattensViewerRegionFirstThenDepthFirst()
{
    PaneListModel model;

    // viewer: split( leaf v1, split( leaf v2, leaf v3 ) )
    const QVariantMap viewer = split(
        QStringLiteral("horizontal"),
        {viewerLeaf(QStringLiteral("viewer-1"), QStringLiteral("file:///r/a.txt")),
         split(QStringLiteral("vertical"),
               {viewerLeaf(QStringLiteral("viewer-2"),
                           QStringLiteral("file:///r/b.txt")),
                viewerLeaf(QStringLiteral("viewer-3"),
                           QStringLiteral("file:///r/c.txt"))})});
    // terminal: the region default shape, two stacked leaves.
    const QVariantMap terminal = split(
        QStringLiteral("vertical"),
        {terminalLeaf(QStringLiteral("terminal-1"), QStringLiteral("row-1")),
         terminalLeaf(QStringLiteral("terminal-2"), QStringLiteral("row-2"))});

    model.setTrees(viewer, terminal);

    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(column(model, PaneListModel::PaneIdRole),
             QStringList({QStringLiteral("viewer-1"), QStringLiteral("viewer-2"),
                          QStringLiteral("viewer-3"),
                          QStringLiteral("terminal-1"),
                          QStringLiteral("terminal-2")}));
    QCOMPARE(column(model, PaneListModel::RegionRole),
             QStringList({QStringLiteral("viewer"), QStringLiteral("viewer"),
                          QStringLiteral("viewer"), QStringLiteral("terminal"),
                          QStringLiteral("terminal")}));
    // The terminal's identity is the server-minted row id, not its slot label.
    QCOMPARE(column(model, PaneListModel::TerminalPaneIdRole),
             QStringList({QString(), QString(), QString(),
                          QStringLiteral("row-1"), QStringLiteral("row-2")}));
}

// A slot label is unique only WITHIN its region - "viewer-1" and "terminal-1"
// coexist by design - so the key the shell selects by has to carry the region.
// Without this, selecting "terminal-1" could open the viewer pane.
void TstPaneList::paneKeyIsRegionQualifiedBecauseSlotLabelsRepeat()
{
    PaneListModel model;
    model.setTrees(viewerLeaf(QStringLiteral("viewer-1"), QString()),
                   terminalLeaf(QStringLiteral("viewer-1"),
                                QStringLiteral("row-1")));

    QCOMPARE(column(model, PaneListModel::PaneKeyRole),
             QStringList({QStringLiteral("viewer:viewer-1"),
                          QStringLiteral("terminal:viewer-1")}));
    // Same label, two rows, two distinct keys - and each key finds its own row.
    QCOMPARE(model.paneByKey(QStringLiteral("viewer:viewer-1"))
                 .value(QStringLiteral("region")).toString(),
             QStringLiteral("viewer"));
    QCOMPARE(model.paneByKey(QStringLiteral("terminal:viewer-1"))
                 .value(QStringLiteral("terminalPaneId")).toString(),
             QStringLiteral("row-1"));
}

// A viewer row is named by what it has open. The directory case is the one that
// bites: QUrl::fileName() is EMPTY for a trailing-slash url, and a blank row is
// unusable, so the last path segment stands in. A leaf with no url at all is the
// session root, which is a real destination (the repository's own directory) and
// not an error.
void TstPaneList::viewerTitlesAreDerivedFromTheStoredUrl()
{
    PaneListModel model;
    model.setTrees(
        split(QStringLiteral("vertical"),
              {viewerLeaf(QStringLiteral("viewer-1"),
                          QStringLiteral("file:///srv/repo/README.md")),
               viewerLeaf(QStringLiteral("viewer-2"),
                          QStringLiteral("file:///srv/repo/docs/")),
               viewerLeaf(QStringLiteral("viewer-3"), QString()),
               viewerLeaf(QStringLiteral("viewer-4"),
                          QStringLiteral("https://example.com"))}),
        {});

    QCOMPARE(column(model, PaneListModel::TitleRole),
             QStringList({QStringLiteral("README.md"), QStringLiteral("docs"),
                          QStringLiteral("Session root"),
                          QStringLiteral("example.com")}));
}

// The user's own title wins over the generated slot label, exactly as the desktop
// pane header resolves it - and clearing it must fall back to the label rather
// than to a blank row. The title is also NORMALIZED (trimmed, bounded) by
// SplitNode, so a padded value cannot smuggle whitespace into the row.
void TstPaneList::terminalTitlePrefersTheUsersOwnTitle()
{
    PaneListModel model;
    model.setTrees(
        {},
        split(QStringLiteral("vertical"),
              {terminalLeaf(QStringLiteral("terminal-1"), QStringLiteral("row-1"),
                            QStringLiteral("  build  ")),
               terminalLeaf(QStringLiteral("terminal-2"),
                            QStringLiteral("row-2"))}));

    QCOMPARE(column(model, PaneListModel::TitleRole),
             QStringList({QStringLiteral("build"),
                          QStringLiteral("terminal-2")}));
}

// The kind is what decides which single page opens, and it comes from
// ch::ViewerHandlerRegistry - the SAME classification the desktop uses - so the
// two shells cannot disagree about what a file is. A terminal leaf is always
// "terminal"; a url the registry claims nothing for is "unsupported", never a
// guess at "text".
void TstPaneList::kindsComeFromTheSharedViewerRegistry()
{
    PaneListModel model;
    model.setTrees(
        split(QStringLiteral("vertical"),
              {viewerLeaf(QStringLiteral("viewer-1"),
                          QStringLiteral("file:///r/NOTES.md")),
               viewerLeaf(QStringLiteral("viewer-2"),
                          QStringLiteral("file:///r/main.cpp")),
               viewerLeaf(QStringLiteral("viewer-3"),
                          QStringLiteral("file:///r/logo.png")),
               viewerLeaf(QStringLiteral("viewer-4"),
                          QStringLiteral("file:///r/manual.pdf")),
               viewerLeaf(QStringLiteral("viewer-5"),
                          QStringLiteral("file:///r/src/")),
               viewerLeaf(QStringLiteral("viewer-6"),
                          QStringLiteral("https://example.com/page")),
               viewerLeaf(QStringLiteral("viewer-7"),
                          QStringLiteral("file:///r/a.bin")),
               viewerLeaf(QStringLiteral("viewer-8"),
                          QStringLiteral("mailto:nobody@example.com")),
               viewerLeaf(QStringLiteral("viewer-9"), QString())}),
        terminalLeaf(QStringLiteral("terminal-1"), QStringLiteral("row-1")));

    QCOMPARE(column(model, PaneListModel::KindRole),
             QStringList({QStringLiteral("markdown"), QStringLiteral("text"),
                          QStringLiteral("image"), QStringLiteral("pdf"),
                          QStringLiteral("directory"), QStringLiteral("web"),
                          QStringLiteral("binary"),
                          QStringLiteral("unsupported"),
                          QStringLiteral("directory"),
                          QStringLiteral("terminal")}));
}

// A tree that is not one SplitNode::tryFromJson would accept is dropped WHOLE,
// not walked as far as the bad node: a partial walk would publish a picker that
// silently omits panes the session really has, which is worse than an empty one.
// And the two regions are independent - one bad tree must not cost the user the
// other region's panes.
void TstPaneList::aMalformedRegionContributesNothingAndDoesNotTakeTheOtherWithIt()
{
    PaneListModel model;

    // A split with no children, and a node that is neither leaf nor split.
    const QVariantMap emptySplit =
        QVariantMap{{QStringLiteral("type"), QStringLiteral("split")},
                    {QStringLiteral("orientation"), QStringLiteral("vertical")},
                    {QStringLiteral("children"), QVariantList{}},
                    {QStringLiteral("ratios"), QVariantList{}}};
    model.setTrees(emptySplit,
                   terminalLeaf(QStringLiteral("terminal-1"),
                                QStringLiteral("row-1")));
    QCOMPARE(column(model, PaneListModel::PaneKeyRole),
             QStringList({QStringLiteral("terminal:terminal-1")}));

    const QVariantMap unknownType =
        QVariantMap{{QStringLiteral("type"), QStringLiteral("region")},
                    {QStringLiteral("paneId"), QStringLiteral("viewer-1")}};
    model.setTrees(unknownType,
                   terminalLeaf(QStringLiteral("terminal-1"),
                                QStringLiteral("row-1")));
    QCOMPARE(column(model, PaneListModel::PaneKeyRole),
             QStringList({QStringLiteral("terminal:terminal-1")}));

    // A good leaf BURIED under a bad sibling is lost with the rest of the tree,
    // deliberately: half a layout is not a layout.
    model.setTrees(split(QStringLiteral("vertical"),
                         {viewerLeaf(QStringLiteral("viewer-1"), QString()),
                          emptySplit}),
                   {});
    QCOMPARE(model.rowCount(), 0);

    // A tree that is not even a node - a bare string, a list - is refused the
    // same way rather than reaching toMap() and yielding a typeless node.
    model.setTrees(QStringLiteral("not-a-tree"), QVariantList{});
    QCOMPARE(model.rowCount(), 0);
}

// The one bound that is about safety rather than shape: nesting past
// SplitNode::kMaxDepth is refused at the depth check, so no tree - hostile,
// corrupt, or hand-assembled - can drive the flattening into stack exhaustion.
void TstPaneList::anOverDeepTreeIsRefusedRatherThanRecursedInto()
{
    // One level past the bound the parser, the writer and operator== all
    // enforce. Assembled here because nothing that can be PARSED is ever this
    // deep, which is exactly why the check cannot be left to the parser.
    QVariantMap node = viewerLeaf(QStringLiteral("viewer-deep"), QString());
    for (int depth = 0; depth <= SplitNode::kMaxDepth; ++depth)
        node = split(QStringLiteral("vertical"), {node});

    PaneListModel model;
    model.setTrees(node, terminalLeaf(QStringLiteral("terminal-1"),
                                      QStringLiteral("row-1")));

    // The viewer region is gone; the terminal region, which is fine, is not.
    QCOMPARE(column(model, PaneListModel::PaneKeyRole),
             QStringList({QStringLiteral("terminal:terminal-1")}));

    // And a tree that is deep but legal is still flattened, so the bound is not
    // simply refusing everything nested.
    QVariantMap legal = viewerLeaf(QStringLiteral("viewer-deep"), QString());
    for (int depth = 0; depth < 16; ++depth)
        legal = split(QStringLiteral("vertical"), {legal});
    model.setTrees(legal, {});
    QCOMPARE(column(model, PaneListModel::PaneKeyRole),
             QStringList({QStringLiteral("viewer:viewer-deep")}));
}

// Closing the last pane of a region leaves a single leaf with an EMPTY paneId.
// That placeholder exists so the desktop regions have something to split; it has
// no content, no identity and nothing to show, so the picker must not offer it -
// and its region is not malformed, so the OTHER panes of that region (there are
// none here, by construction) and the other region survive.
void TstPaneList::theEmptyPlaceholderLeafIsNotAPane()
{
    PaneListModel model;
    model.setTrees(leaf(QString()),
                   split(QStringLiteral("vertical"),
                         {terminalLeaf(QStringLiteral("terminal-1"),
                                       QStringLiteral("row-1")),
                          leaf(QString())}));

    QCOMPARE(column(model, PaneListModel::PaneKeyRole),
             QStringList({QStringLiteral("terminal:terminal-1")}));
    QVERIFY(model.paneByKey(QStringLiteral("viewer:")).isEmpty());
    QVERIFY(model.paneByKey(QStringLiteral("terminal:")).isEmpty());
}

// A null QVariant is how SessionLayouts spells "not loaded yet" (and "this
// region's getLayout failed"). It is not a malformed tree: the region simply
// contributes no rows, so a half-loaded session lists the panes it does know
// rather than nothing at all.
void TstPaneList::aNullRegionIsNotLoadedRatherThanEmpty()
{
    PaneListModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    model.setTrees({}, {});
    QCOMPARE(model.rowCount(), 0);

    model.setTrees(viewerLeaf(QStringLiteral("viewer-1"), QString()), {});
    QCOMPARE(column(model, PaneListModel::PaneKeyRole),
             QStringList({QStringLiteral("viewer:viewer-1")}));

    // Every publish is one reset, which is what the picker's delegates rebuild
    // from; two calls, two resets.
    QCOMPARE(resetSpy.count(), 2);
}

// Row STABILITY across a republish. SessionLayouts republishes both trees on
// every load and every structural edit, and the picker is a list under the user's
// thumb: a model that sorted by title or id would reshuffle rows that did not
// change. Depth-first over the stored child order is the ordering that cannot.
void TstPaneList::republishingAnUnchangedTreeKeepsEveryRowInPlace()
{
    const QVariantMap viewer =
        split(QStringLiteral("horizontal"),
              {viewerLeaf(QStringLiteral("viewer-2"),
                          QStringLiteral("file:///r/zebra.txt")),
               viewerLeaf(QStringLiteral("viewer-1"),
                          QStringLiteral("file:///r/apple.txt"))});
    const QVariantMap terminal =
        terminalLeaf(QStringLiteral("terminal-1"), QStringLiteral("row-1"));

    PaneListModel model;
    model.setTrees(viewer, terminal);
    const QStringList first = column(model, PaneListModel::PaneKeyRole);
    // Stored order, NOT sorted: viewer-2 really does come first.
    QCOMPARE(first, QStringList({QStringLiteral("viewer:viewer-2"),
                                 QStringLiteral("viewer:viewer-1"),
                                 QStringLiteral("terminal:terminal-1")}));

    model.setTrees(viewer, terminal);
    QCOMPARE(column(model, PaneListModel::PaneKeyRole), first);

    // A pane ADDED by another client extends the list; it does not reorder the
    // rows that were already there.
    const QVariantMap grown =
        split(QStringLiteral("horizontal"),
              {viewerLeaf(QStringLiteral("viewer-2"),
                          QStringLiteral("file:///r/zebra.txt")),
               viewerLeaf(QStringLiteral("viewer-1"),
                          QStringLiteral("file:///r/apple.txt")),
               viewerLeaf(QStringLiteral("viewer-3"),
                          QStringLiteral("file:///r/new.txt"))});
    model.setTrees(grown, terminal);
    const QStringList second = column(model, PaneListModel::PaneKeyRole);
    QCOMPARE(second.mid(0, 2), first.mid(0, 2));
    QCOMPARE(second.last(), QStringLiteral("terminal:terminal-1"));
    QVERIFY(second.contains(QStringLiteral("viewer:viewer-3")));
}

// The lookup QML restores a selection through. An unknown key is a QUESTION, not
// a fault: it may name a pane another client closed since it was remembered.
void TstPaneList::paneByKeyAnswersTheRowOrAnEmptyMapForAKeyItDoesNotHold()
{
    PaneListModel model;
    model.setTrees(viewerLeaf(QStringLiteral("viewer-1"),
                              QStringLiteral("file:///r/a.md")),
                   terminalLeaf(QStringLiteral("terminal-1"),
                                QStringLiteral("row-1"),
                                QStringLiteral("build")));

    const QVariantMap viewer = model.paneByKey(QStringLiteral("viewer:viewer-1"));
    QCOMPARE(viewer.value(QStringLiteral("paneKey")).toString(),
             QStringLiteral("viewer:viewer-1"));
    QCOMPARE(viewer.value(QStringLiteral("paneId")).toString(),
             QStringLiteral("viewer-1"));
    QCOMPARE(viewer.value(QStringLiteral("url")).toString(),
             QStringLiteral("file:///r/a.md"));
    QCOMPARE(viewer.value(QStringLiteral("title")).toString(),
             QStringLiteral("a.md"));
    QCOMPARE(viewer.value(QStringLiteral("kind")).toString(),
             QStringLiteral("markdown"));
    QCOMPARE(viewer.value(QStringLiteral("terminalPaneId")).toString(), QString());

    const QVariantMap terminal =
        model.paneByKey(QStringLiteral("terminal:terminal-1"));
    QCOMPARE(terminal.value(QStringLiteral("kind")).toString(),
             QStringLiteral("terminal"));
    QCOMPARE(terminal.value(QStringLiteral("title")).toString(),
             QStringLiteral("build"));

    QVERIFY(model.paneByKey(QStringLiteral("viewer:viewer-9")).isEmpty());
    QVERIFY(model.paneByKey(QString()).isEmpty());
    // The region half is not optional, and a bare paneId is not a key.
    QVERIFY(model.paneByKey(QStringLiteral("viewer-1")).isEmpty());
}

// The depth bound is only "the same bound as the parser" if it is COUNTED the
// same way. SplitTree.cpp's parser calls the root level 1 and refuses anything
// past kMaxDepth, so a tree of exactly kMaxDepth levels must flatten and one of
// kMaxDepth + 1 levels - which no parser would ever hand over - must not. A
// flattening that started counting at 0 accepted one level more than any tree
// that can be parsed, which is the sort of drift only a boundary case catches.
void TstPaneList::theDepthBoundIsCountedExactlyAsTheParserCountsIt()
{
    // `levels` counts the leaf too, so a tree of N levels is N-1 splits over it.
    const auto treeOfLevels = [](int levels) {
        QVariantMap node = viewerLeaf(QStringLiteral("viewer-deep"), QString());
        for (int level = 1; level < levels; ++level)
            node = split(QStringLiteral("vertical"), {node});
        return node;
    };

    PaneListModel model;
    model.setTrees(treeOfLevels(SplitNode::kMaxDepth), {});
    QCOMPARE(column(model, PaneListModel::PaneKeyRole),
             QStringList({QStringLiteral("viewer:viewer-deep")}));

    model.setTrees(treeOfLevels(SplitNode::kMaxDepth + 1), {});
    QCOMPARE(model.rowCount(), 0);
}

// Two leaves with the SAME paneId in one region are not two panes: their
// paneKeys are identical, so a second row could only ever open the first one
// while the picker highlighted both as selected. SessionLayouts refuses such a
// tree outright, so this is the model's own guard against a tree that did not
// come from it - and it must cost only the duplicate, not the region.
void TstPaneList::aRepeatedPaneIdInOneRegionYieldsOneRow()
{
    PaneListModel model;
    model.setTrees(
        split(QStringLiteral("horizontal"),
              {viewerLeaf(QStringLiteral("viewer-1"),
                          QStringLiteral("file:///r/first.md")),
               viewerLeaf(QStringLiteral("viewer-1"),
                          QStringLiteral("file:///r/second.md")),
               viewerLeaf(QStringLiteral("viewer-2"),
                          QStringLiteral("file:///r/third.md"))}),
        terminalLeaf(QStringLiteral("terminal-1"), QStringLiteral("row-1")));

    // One row per distinct key, and the FIRST occurrence is the one kept - the
    // one every selection by that key would have reached anyway.
    QCOMPARE(column(model, PaneListModel::PaneKeyRole),
             QStringList({QStringLiteral("viewer:viewer-1"),
                          QStringLiteral("viewer:viewer-2"),
                          QStringLiteral("terminal:terminal-1")}));
    QCOMPARE(model.paneByKey(QStringLiteral("viewer:viewer-1"))
                 .value(QStringLiteral("url")).toString(),
             QStringLiteral("file:///r/first.md"));

    // The same label in the OTHER region is a different pane and is untouched by
    // this rule; that is what the region half of the key is for.
    model.setTrees(viewerLeaf(QStringLiteral("p"), QString()),
                   terminalLeaf(QStringLiteral("p"), QStringLiteral("row-1")));
    QCOMPARE(model.rowCount(), 2);
}

// A row's label is the last thing standing between the user and an unusable
// picker, so the derivation has to survive every url a layout can carry. None of
// these may produce a blank row.
void TstPaneList::oddViewerUrlsStillProduceAUsableTitle()
{
    PaneListModel model;
    model.setTrees(
        split(QStringLiteral("vertical"),
              {// A query string belongs to the url, not to the file name.
               viewerLeaf(QStringLiteral("viewer-1"),
                          QStringLiteral("file:///r/report.md?rev=3")),
               // No extension at all is a perfectly ordinary file.
               viewerLeaf(QStringLiteral("viewer-2"),
                          QStringLiteral("file:///r/Makefile")),
               // Percent-encoded non-ASCII comes back decoded, because the row
               // is for a human to read.
               viewerLeaf(QStringLiteral("viewer-3"),
                          QStringLiteral("file:///r/%C3%A9t%C3%A9/r%C3%A9sum"
                                         "%C3%A9.md")),
               // A trailing slash makes QUrl::fileName() empty; the directory's
               // own name stands in.
               viewerLeaf(QStringLiteral("viewer-4"),
                          QStringLiteral("file:///r/deep/docs/")),
               // Nothing to name it by at all: shown verbatim rather than blank.
               viewerLeaf(QStringLiteral("viewer-5"), QStringLiteral("file:///"))}),
        {});

    QCOMPARE(column(model, PaneListModel::TitleRole),
             QStringList({QStringLiteral("report.md"),
                          QStringLiteral("Makefile"),
                          QString::fromUtf8("résumé.md"),
                          QStringLiteral("docs"),
                          QStringLiteral("file:///")}));
}

// Guiless: the model is pure QtCore state and nothing here needs a window
// system, so the target runs identically on a headless CI machine.
QTEST_GUILESS_MAIN(TstPaneList)
#include "tst_panelist.moc"
