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
// Depth invariant: kMaxDepth below is the ONE nesting bound for this type. Every
// recursive operation here — the parser, the writer and operator== — enforces
// it, so no operation on a SplitNode can be driven into stack exhaustion by a
// hostile or corrupt tree, whatever its provenance.
struct SplitNode {
    QString paneId;
    // What this leaf has open (SPEC 4.5 pane content). Empty means "nothing
    // open", which is also what an older tree written before this field existed
    // loads as. Persisted ONLY for leaves and ONLY when non-empty, so a tree
    // with no urls serializes byte-identically to one produced before the field
    // existed - a pane's content rides along with the structure it belongs to
    // instead of needing a second, separately reconciled store.
    QString url;
    // The server-minted `terminal_panes.id` this leaf's terminal belongs to
    // (SPEC 5.2), and the ONLY thing that identifies a terminal. Empty on a
    // viewer leaf, and empty on a terminal leaf stored before this field
    // existed - see ch::SessionLayouts for what happens then. Persisted ONLY
    // for leaves and ONLY when non-empty, on the same rule as `url`, so a tree
    // written before the field serializes byte-identically after it.
    //
    // Why it is HERE, in the layout, rather than derived from `paneId`: the
    // pane id is a slot LABEL minted per client ("terminal-1", "terminal-2", …)
    // and it is recycled - closing a pane leaves the row and its tmux session
    // alive on purpose, and the next split on any client may hand the freed
    // label to a brand new pane. Keying a terminal on the label therefore lets
    // a new pane silently adopt a closed pane's shell. A row id is minted by
    // the server, never reused, and travels to every client inside the shared
    // tree, so all of them agree on which shell a leaf owns.
    QString terminalPaneId;
    // Optional user-chosen title for a terminal leaf. Empty means the UI falls
    // back to the generated paneId label. It lives in the layout leaf so every
    // client sharing the server-side tree sees the same title without changing
    // the server-minted terminalPaneId identity.
    QString customTitle;
    // Keep persisted titles bounded: a pane header is compact, and allowing an
    // unbounded value here would make a user-controlled layout grow forever.
    static constexpr int kMaxCustomTitleLength = 128;
    static QString normalizeCustomTitle(QString title)
    {
        title = title.trimmed();
        if (title.size() > kMaxCustomTitleLength)
            title.truncate(kMaxCustomTitleLength);
        return title;
    }
    
    SplitOrientation orientation = SplitOrientation::Horizontal;
    QVector<SplitNode> children;
    QVector<double> ratios;

    bool isLeaf() const { return children.isEmpty(); }

    // Hard cap on nesting depth, counting the root as level 1. Split trees are
    // persisted and may arrive from the remote server (SPEC 2.1 remote-first),
    // so they are not all this client's own work: without a bound, adversarial
    // or corrupt deeply-nested input would recurse until the stack overflows
    // and takes the process with it. Real layouts nest a handful of levels — a
    // user splitting a region one level at a time — so 256 is far beyond any
    // genuine use while staying well inside the stack a default thread gets.
    static constexpr int kMaxDepth = 256;

    // Serialize, or std::nullopt when this tree is not one tryFromJson() would
    // accept back.
    //
    // THE WRITER AND THE READER AGREE, and this signature is what enforces it:
    // a SplitNode is an aggregate, so nothing stops a caller assembling a split
    // with the wrong number of ratios, a NaN/negative ratio, or nesting past
    // kMaxDepth. Emitting such a tree would persist bytes that no client —
    // including the one that wrote them — can ever load again, and the loss
    // would only surface on the next launch. So the invalid shape is not
    // written at all; the caller gets a clean std::nullopt to report instead.
    // For every tree this does serialize, fromJson(*tryToJson()) == *this.
    std::optional<QJsonObject> tryToJson() const;

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

    // Structural equality mirroring tryToJson(): leaf identity is its paneId,
    // url, terminalPaneId and customTitle (orientation/ratios are meaningless
    // and dropped for leaves), while a split is defined by orientation, ratios,
    // and children (paneId, url, terminalPaneId and customTitle are dropped).
    // This keeps fromJson(*tryToJson(x)) == x for any serializable tree, which a
    // defaulted operator== would break by comparing fields tryToJson() does not
    // persist.
    //
    // Bounded by kMaxDepth like every other recursion here: two nodes still
    // nested deeper than that compare UNEQUAL rather than recursing on. No tree
    // that deep can be parsed or serialized, so the guard only fires on a
    // hand-assembled monster, and "unequal" is the safe verdict — it never
    // claims two trees are the same on the strength of a walk we cut short.
    bool operator==(const SplitNode &other) const;
};

} // namespace ch
