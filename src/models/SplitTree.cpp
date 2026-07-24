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

SplitNode SplitNode::fromJson(const QJsonObject &obj)
{
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("leaf")) {
        SplitNode node;
        node.paneId = obj.value(QStringLiteral("paneId")).toString();
        return node;
    }

    if (type != QStringLiteral("split"))
        return {};

    const QJsonArray childArray = obj.value(QStringLiteral("children")).toArray();
    const QJsonArray ratioArray = obj.value(QStringLiteral("ratios")).toArray();

    // An internal node must have children, and exactly one ratio per child.
    if (childArray.isEmpty() || ratioArray.size() != childArray.size())
        return {};

    SplitNode node;
    node.orientation = obj.value(QStringLiteral("orientation")).toString()
                == QStringLiteral("vertical")
            ? SplitOrientation::Vertical
            : SplitOrientation::Horizontal;

    node.children.reserve(childArray.size());
    for (const QJsonValue &value : childArray)
        node.children.append(fromJson(value.toObject()));

    node.ratios.reserve(ratioArray.size());
    for (const QJsonValue &value : ratioArray)
        node.ratios.append(value.toDouble());

    return node;
}

} // namespace ch
