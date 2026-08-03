#include "SplitTree.h"

#include <QJsonArray>
#include <QJsonValue>

#include <cmath>
#include <utility>

namespace ch {

namespace {

// Writes `node` into `obj`, returning false if `node` is not a tree the parser
// below would accept back: a split whose ratio count does not match its child
// count, a ratio that is not finite and > 0, or nesting deeper than kMaxDepth.
// The checks are deliberately the SAME ones parseNode() applies, and are
// applied per node on the way down, so a single bad node anywhere in the tree
// fails the whole serialization instead of yielding a half-written object.
bool writeNode(const SplitNode &node, QJsonObject &obj, int depth)
{
    if (depth > SplitNode::kMaxDepth)
        return false;

    if (node.isLeaf()) {
        obj[QStringLiteral("type")] = QStringLiteral("leaf");
        obj[QStringLiteral("paneId")] = node.paneId;
        // Omitted when empty: an "open nothing" leaf must serialize exactly as
        // it did before this field existed, so upgrading the app does not
        // rewrite every stored layout and a downgrade still reads them.
        if (!node.url.isEmpty())
            obj[QStringLiteral("url")] = node.url;
        // Same rule, same reason: a viewer leaf and every terminal leaf written
        // before this field existed carry none, and must keep serializing
        // exactly as they did.
        if (!node.terminalPaneId.isEmpty())
            obj[QStringLiteral("terminalPaneId")] = node.terminalPaneId;
        const QString customTitle = SplitNode::normalizeCustomTitle(node.customTitle);
        // Empty means "use the generated paneId label". Omitting it keeps old
        // layouts byte-stable while persisting non-empty custom titles beside
        // the leaf's existing content and identity fields.
        if (!customTitle.isEmpty())
            obj[QStringLiteral("customTitle")] = customTitle;
        return true;
    }

    // One ratio per child, each finite and > 0 - the parser's rule, checked
    // here so the bad shape is never emitted in the first place.
    if (node.ratios.size() != node.children.size())
        return false;

    QJsonArray ratioArray;
    for (double ratio : node.ratios) {
        if (!std::isfinite(ratio) || ratio <= 0.0)
            return false;
        ratioArray.append(ratio);
    }

    QJsonArray childArray;
    for (const SplitNode &child : node.children) {
        QJsonObject childObj;
        if (!writeNode(child, childObj, depth + 1))
            return false;
        childArray.append(childObj);
    }

    obj[QStringLiteral("type")] = QStringLiteral("split");
    obj[QStringLiteral("orientation")] =
            node.orientation == SplitOrientation::Vertical
            ? QStringLiteral("vertical")
            : QStringLiteral("horizontal");
    obj[QStringLiteral("children")] = childArray;
    obj[QStringLiteral("ratios")] = ratioArray;
    return true;
}

// Bounded structural comparison; see the header for why running out of depth
// answers "unequal" rather than recursing on.
bool equalNode(const SplitNode &lhs, const SplitNode &rhs, int depth)
{
    if (depth > SplitNode::kMaxDepth)
        return false;
    // Compare only the fields tryToJson() persists for each node kind, so
    // equality agrees with the JSON round-trip (see the header). A leaf and a
    // split are never equal; a leaf's identity is its paneId, url,
    // terminalPaneId and normalized customTitle; a split's is its orientation,
    // ratios, and children.
    if (lhs.isLeaf() != rhs.isLeaf())
        return false;
    if (lhs.isLeaf())
        return lhs.paneId == rhs.paneId && lhs.url == rhs.url
                && lhs.terminalPaneId == rhs.terminalPaneId
                && SplitNode::normalizeCustomTitle(lhs.customTitle)
                    == SplitNode::normalizeCustomTitle(rhs.customTitle);
    if (lhs.orientation != rhs.orientation || lhs.ratios != rhs.ratios
        || lhs.children.size() != rhs.children.size())
        return false;
    for (qsizetype i = 0; i < lhs.children.size(); ++i) {
        if (!equalNode(lhs.children.at(i), rhs.children.at(i), depth + 1))
            return false;
    }
    return true;
}

// Parses obj into out, returning false on ANY structural violation, including a
// malformed nested subtree or nesting deeper than kMaxDepth. This lets fromJson
// reject an otherwise well-formed split that contains an invalid child, rather
// than silently substituting an empty leaf for the bad subtree. A leaf always
// parses successfully (an empty paneId is a legitimate value), so failure is
// unambiguous and only ever originates from a split.
bool parseNode(const QJsonObject &obj, SplitNode &out, int depth)
{
    if (depth > SplitNode::kMaxDepth)
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
        // Normalize server-supplied titles on the way in. This keeps a
        // hand-edited or older layout from bypassing the same whitespace and
        // size limits applied by SessionLayouts::setPaneTitle().
        out.customTitle = SplitNode::normalizeCustomTitle(
            obj.value(QStringLiteral("customTitle")).toString());
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

std::optional<QJsonObject> SplitNode::tryToJson() const
{
    QJsonObject obj;
    if (!writeNode(*this, obj, 1))
        return std::nullopt;
    return obj;
}

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
    return equalNode(*this, other, 1);
}

} // namespace ch
