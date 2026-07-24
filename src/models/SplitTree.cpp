#include "SplitTree.h"

#include <QJsonArray>
#include <QJsonValue>

namespace ch {

QJsonObject SplitNode::toJson() const
{
    QJsonObject obj;
    if (isLeaf()) {
        obj[QStringLiteral("type")] = QStringLiteral("leaf");
        obj[QStringLiteral("paneId")] = paneId;
        return obj;
    }

    obj[QStringLiteral("type")] = QStringLiteral("split");
    obj[QStringLiteral("orientation")] = orientation == SplitOrientation::Vertical
            ? QStringLiteral("vertical")
            : QStringLiteral("horizontal");

    QJsonArray childArray;
    for (const SplitNode &child : children)
        childArray.append(child.toJson());
    obj[QStringLiteral("children")] = childArray;

    QJsonArray ratioArray;
    for (double ratio : ratios)
        ratioArray.append(ratio);
    obj[QStringLiteral("ratios")] = ratioArray;

    return obj;
}

namespace {

// Parses obj into out, returning false on ANY structural violation, including a
// malformed nested subtree. This lets fromJson reject an otherwise well-formed
// split that contains an invalid child, rather than silently substituting an
// empty leaf for the bad subtree. A leaf always parses successfully (an empty
// paneId is a legitimate value), so failure is unambiguous and only ever
// originates from a split.
bool parseNode(const QJsonObject &obj, SplitNode &out)
{
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("leaf")) {
        out = SplitNode{};
        out.paneId = obj.value(QStringLiteral("paneId")).toString();
        return true;
    }

    if (type != QStringLiteral("split"))
        return false;

    const QJsonArray childArray = obj.value(QStringLiteral("children")).toArray();
    const QJsonArray ratioArray = obj.value(QStringLiteral("ratios")).toArray();

    // An internal node must have children, and exactly one ratio per child.
    if (childArray.isEmpty() || ratioArray.size() != childArray.size())
        return false;

    SplitNode node;
    node.orientation = obj.value(QStringLiteral("orientation")).toString()
                == QStringLiteral("vertical")
            ? SplitOrientation::Vertical
            : SplitOrientation::Horizontal;

    node.children.reserve(childArray.size());
    for (const QJsonValue &value : childArray) {
        if (!value.isObject())
            return false;
        SplitNode child;
        if (!parseNode(value.toObject(), child))
            return false;
        node.children.append(child);
    }

    node.ratios.reserve(ratioArray.size());
    for (const QJsonValue &value : ratioArray) {
        if (!value.isDouble())
            return false;
        node.ratios.append(value.toDouble());
    }

    out = node;
    return true;
}

} // namespace

SplitNode SplitNode::fromJson(const QJsonObject &obj)
{
    SplitNode node;
    if (!parseNode(obj, node))
        return {};
    return node;
}

} // namespace ch
