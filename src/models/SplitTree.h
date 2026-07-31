#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

// Recursive split-tree model for the viewer and terminal regions (SPEC 4.5).
// Each region persists an independent tree; ratios are stored per Dev Session.
namespace ch {

enum class SplitOrientation { Horizontal, Vertical };

// A node in a region's split tree. A node is EITHER:
//   * a leaf, holding a single paneId and the url it currently shows (children
//     empty); or
//   * an internal split, holding an orientation, child subtrees, and one ratio
//     per child (ratios.size() == children.size()).
// A default-constructed node is an empty leaf. It doubles as the "invalid"
// sentinel fromJson() returns when the input fails validation, which is exactly
// why tryFromJson() exists — see below.
//
// SplitNode is a value type today: children are held by value in the
// QVector<SplitNode> member, so copies are deep and there is no shared
// ownership. That describes the current shape only; it is not a standing
// guarantee. This file has no in-place split/remove/move operations, but if any
// are ever added, node lifetime and tree invariants must be re-examined here.
//
// Depth invariant: every in-memory SplitNode tree is kept within the same
// nesting bound (kMaxDepth in SplitTree.cpp) that fromJson() enforces on parse.
// fromJson() rejects deeper input, and layouts constructed in-memory nest only a
// handful of levels, so toJson() and operator==, which recurse WITHOUT their own
// depth guard, are safe: they only ever walk trees that already honor the bound.
struct SplitNode {
    QString paneId;
    // What this leaf has open (SPEC 4.5 pane content). Empty means "nothing
    // open", which is also what an older tree written before this field existed
    // loads as. Persisted ONLY for leaves and ONLY when non-empty, so a tree
    // with no urls serializes byte-identically to one produced before the field
    // existed - a pane's content rides along with the structure it belongs to
    // instead of needing a second, separately reconciled store.
    QString url;
    SplitOrientation orientation = SplitOrientation::Horizontal;
    QVector<SplitNode> children;
    QVector<double> ratios;

    bool isLeaf() const { return children.isEmpty(); }

    // Exact JSON round-trip. toJson()/fromJson() are inverses for any valid tree.
    QJsonObject toJson() const;

    // Decode, or std::nullopt when `obj` is not a valid split tree.
    //
    // PREFER THIS over fromJson(). fromJson() has to report a rejection by
    // returning its default-constructed value, which is an empty leaf - and an
    // empty leaf is also a perfectly legitimate stored tree (closing a region's
    // last pane persists exactly that; see src/app/SessionLayouts.cpp). A caller
    // that needs to tell "rejected" from "a genuinely empty leaf" therefore has
    // to re-inspect the input's "type" tag, a subtle rule that was independently
    // reimplemented in two places before this existed. The parser already knows
    // the answer; this simply does not throw it away.
    static std::optional<SplitNode> tryFromJson(const QJsonObject &obj);

    // Returns a default (empty-leaf) node when obj is not a valid split tree,
    // including the case where ratios.size() != children.size(). Kept for
    // callers to which a rejected tree and an empty leaf are the same thing.
    static SplitNode fromJson(const QJsonObject &obj);

    // Structural equality mirroring toJson(): leaf identity is its paneId and
    // url (orientation/ratios are meaningless and dropped for leaves), while a
    // split is defined by orientation, ratios, and children (paneId and url are
    // dropped). This keeps fromJson(toJson(x)) == x for any valid tree, which a
    // defaulted operator== would break by comparing fields toJson() does not
    // persist.
    bool operator==(const SplitNode &other) const;
};

} // namespace ch
