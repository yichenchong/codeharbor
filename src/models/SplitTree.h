#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

// Recursive split-tree model for the viewer and terminal regions (SPEC 4.5).
// Each region persists an independent tree; ratios are stored per Dev Session.
namespace ch {

enum class SplitOrientation { Horizontal, Vertical };

// A node in a region's split tree. A node is EITHER:
//   * a leaf, holding a single paneId (children empty); or
//   * an internal split, holding an orientation, child subtrees, and one ratio
//     per child (ratios.size() == children.size()).
// A default-constructed node is an empty leaf and is used as the "invalid"
// sentinel returned by fromJson() when the input fails validation.
struct SplitNode {
    QString paneId;
    SplitOrientation orientation = SplitOrientation::Horizontal;
    QVector<SplitNode> children;
    QVector<double> ratios;

    bool isLeaf() const { return children.isEmpty(); }

    // Exact JSON round-trip. toJson()/fromJson() are inverses for any valid tree.
    QJsonObject toJson() const;
    // Returns a default (empty-leaf) node when obj is not a valid split tree,
    // including the case where ratios.size() != children.size().
    static SplitNode fromJson(const QJsonObject &obj);

    bool operator==(const SplitNode &) const = default;
};

} // namespace ch
