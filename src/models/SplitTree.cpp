#include "SplitTree.h"

#include <QJsonArray>
#include <QJsonValue>

#include <cmath>
#include <utility>

namespace ch {

QJsonObject SplitNode::toJson() const
{
    QJsonObject obj;
    if (isLeaf()) {
        obj[QStringLiteral("type")] = QStringLiteral("leaf");
        obj[QStringLiteral("paneId")] = paneId;
        // Omitted when empty: an "open nothing" leaf must serialize exactly as
        // it did before this field existed, so upgrading the app does not
        // rewrite every stored layout and a downgrade still reads them.
        if (!url.isEmpty())
            obj[QStringLiteral("url")] = url;
        // Same rule, same reason: a viewer leaf and every terminal leaf written
        // before this field existed carry none, and must keep serializing
        // exactly as they did.
        if (!terminalPaneId.isEmpty())
            obj[QStringLiteral("terminalPaneId")] = terminalPaneId;
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

// Hard cap on split-tree nesting depth. Split trees are persisted and may be
// sourced from the remote server (SPEC 2.1 remote-first), so fromJson parses
// data the client did not produce. Without a bound, adversarial or corrupt
// deeply-nested JSON would recurse until the stack overflows and crashes the
// process. Real layouts nest only a handful of levels; 256 is far beyond any
// genuine use while still safely below the stack limit.
constexpr int kMaxDepth = 256;

// Parses obj into out, returning false on ANY structural violation, including a
// malformed nested subtree or nesting deeper than kMaxDepth. This lets fromJson
// reject an otherwise well-formed split that contains an invalid child, rather
// than silently substituting an empty leaf for the bad subtree. A leaf always
// parses successfully (an empty paneId is a legitimate value), so failure is
// unambiguous and only ever originates from a split.
bool parseNode(const QJsonObject &obj, SplitNode &out, int depth)
{
    if (depth > kMaxDepth)
        return false;

    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("leaf")) {
        out = SplitNode{};
        out.paneId = obj.value(QStringLiteral("paneId")).toString();
        // Absent (an older tree) or non-string (corrupt) both yield an empty
        // url, the same tolerance paneId gets: a leaf's content is not worth
        // rejecting a whole layout over.
        out.url = obj.value(QStringLiteral("url")).toString();
        // Absent means "this leaf has no server row bound to it" - either a
        // viewer leaf, or a terminal leaf from a layout stored before the field
        // existed. ch::SessionLayouts is what tells those two apart and decides
        // what to do; the parser only reports what is there.
        out.terminalPaneId = obj.value(QStringLiteral("terminalPaneId")).toString();
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
        if (!parseNode(value.toObject(), child, depth + 1))
            return false;
        // Move, never copy: a SplitNode holds its children by value, so copying
        // one duplicates its whole subtree. Copying at every level would make
        // parsing a d-level tree re-copy the same nodes d times over, on data
        // that arrives from the network.
        node.children.append(std::move(child));
    }

    node.ratios.reserve(ratioArray.size());
    for (const QJsonValue &value : ratioArray) {
        if (!value.isDouble())
            return false;
        const double ratio = value.toDouble();
        // Ratios drive pane geometry (a child's extent is ratio / sum of the
        // parent's ratios). A non-finite (NaN/Inf) or non-positive ratio would
        // yield NaN/negative pane sizes, and an all-zero array a divide-by-zero.
        // Reject such malformed input outright, consistent with the count and
        // structural rejections above, rather than silently persisting a tree
        // that produces broken geometry downstream. Requiring every ratio to be
        // finite and > 0 also guarantees their sum is > 0.
        if (!std::isfinite(ratio) || ratio <= 0.0)
            return false;
        node.ratios.append(ratio);
    }

    out = std::move(node);
    return true;
}

} // namespace

std::optional<SplitNode> SplitNode::tryFromJson(const QJsonObject &obj)
{
    SplitNode node;
    if (!parseNode(obj, node, 1))
        return std::nullopt;
    return node;
}

SplitNode SplitNode::fromJson(const QJsonObject &obj)
{
    return tryFromJson(obj).value_or(SplitNode{});
}

bool SplitNode::operator==(const SplitNode &other) const
{
    // Compare only the fields toJson() persists for each node kind, so equality
    // agrees with the JSON round-trip (see the header). A leaf and a split are
    // never equal; a leaf's identity is its paneId, url and terminalPaneId; a
    // split's is its orientation, ratios, and children (recursively).
    if (isLeaf() != other.isLeaf())
        return false;
    if (isLeaf())
        return paneId == other.paneId && url == other.url
                && terminalPaneId == other.terminalPaneId;
    return orientation == other.orientation && ratios == other.ratios
            && children == other.children;
}

} // namespace ch
