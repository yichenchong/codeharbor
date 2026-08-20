#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVector>
#include <QByteArray>
#include <QQmlEngine>

namespace ch {

// The mobile pane picker's list model: BOTH server-authoritative region trees,
// flattened into one flat, ordered list of selectable panes (SPEC 4.5).
//
// This is the mobile client's answer to a structural fact about the desktop
// shell: a Dev Session's layout is a recursive split tree per region, and the
// desktop draws it as nested, simultaneously visible panes. A phone shows
// exactly ONE pane at a time, so the tree is not a layout here — it is a MENU.
// Flattening it is therefore the whole of the mobile "layout engine": there is
// no split, no ratio, no region resize, and the tree's geometry (orientation,
// ratios) is deliberately discarded rather than approximated.
//
// ORDER is depth-first, viewer region first, preserving in-tree child order.
// That is load bearing, not incidental: SessionLayouts republishes both trees on
// every load and on every structural edit, and a picker whose rows reshuffled
// because the model sorted by title or by id would move the row under the user's
// thumb. Depth-first over the persisted child order is the one ordering that is
// stable across republishes for as long as the tree itself is unchanged.
//
// paneKey is "<region>:<paneId>", the only identifier that is unique across both
// regions: a slot label is unique only WITHIN its region ("viewer-1" and
// "terminal-1" coexist by design, see UiStateStore::nextPaneSuffix), so the
// region has to be part of the key. It is what MobileAppController persists as
// the per-Dev-Session selected pane and what QML round-trips through
// selectPane().
//
// A pane's IDENTITY is not the key. For a terminal leaf that is the
// server-minted `terminalPaneId` (SPEC 5.2), carried through as its own role,
// because the slot label is recyclable and the row id is not.
class PaneListModel : public QAbstractListModel {
    Q_OBJECT
    // Reached as mobile.panes and used as a ListView model.
    QML_ELEMENT
    QML_UNCREATABLE("PaneListModel is owned by MobileAppController.")

public:
    enum Roles {
        PaneKeyRole = Qt::UserRole + 1,
        RegionRole,
        PaneIdRole,
        UrlRole,
        TerminalPaneIdRole,
        TitleRole,
        KindRole,
    };
    Q_ENUM(Roles)

    explicit PaneListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replace the whole list from the two published trees. Either may be a null
    // QVariant, which is how SessionLayouts spells "not loaded yet" (and how it
    // spells a region whose getLayout failed): that region simply contributes no
    // rows, so a half-loaded session shows the panes it does know about rather
    // than nothing at all.
    //
    // A MALFORMED tree - a node that is neither leaf nor split, a split with no
    // children, or nesting past SplitNode::kMaxDepth - contributes no rows
    // either, and does so without recursing into it. The published trees have
    // already been through SplitNode::tryFromJson, so this is defence against a
    // caller that is not SessionLayouts (a test, a future host) rather than an
    // expected state; the point is that it can never be a crash.
    //
    // A leaf whose paneId REPEATS one already flattened from the same region is
    // skipped, because its paneKey would be a duplicate and could only ever
    // select the first of the two. That cannot happen to a tree this client
    // wrote (slot labels come from a per-region counter), but nothing in
    // SplitNode enforces it, so a corrupt tree must not produce two rows that
    // open the same pane.
    void setTrees(const QVariant& viewerTree, const QVariant& terminalTree);

    // The row with this paneKey as a {paneKey, region, paneId, url,
    // terminalPaneId, title, kind} map, or an EMPTY map for a key this model
    // does not hold. An unknown key is a question, not a programming error: QML
    // asks with a key restored from UiStateStore, and the pane it names may have
    // been closed by another client since.
    Q_INVOKABLE QVariantMap paneByKey(const QString& paneKey) const;

private:
    struct Pane {
        QString paneKey;
        QString region;
        QString paneId;
        QString url;
        QString terminalPaneId;
        QString title;
        QString kind;
    };

    // Depth-first append of one region's leaves. Returns false the moment the
    // subtree is not one SplitNode::tryFromJson would accept, so the caller can
    // drop the region whole instead of publishing a partial walk of a tree it
    // could not trust. `depth` counts the ROOT as 1, exactly as SplitTree.cpp's
    // parser does, so both refuse the same trees.
    static bool flattenRegion(const QVariant& tree, const QString& region,
                              int depth, QVector<Pane>& out);

    QVector<Pane> m_panes;
};

} // namespace ch
