#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace ch {

// A remote Markdown document, decomposed into a flat list of BLOCKS for a
// native Qt Quick ListView (mobile ViewerMarkdownPage).
//
// WHY THIS EXISTS AT ALL, and why it is hand written. On the desktop a markdown
// pane is HTML rendered by Chromium inside the privileged internal profile,
// behind a restrictive CSP (src/qml/ViewerMarkdownView.qml,
// InternalUrlSchemeHandler). Android and iOS have no Qt WebEngine, so there is
// no sandbox to render inside — which removes the only thing that made rendering
// server-controlled markup survivable. The mobile viewer therefore never
// produces markup at ALL: it produces DATA, and every string it produces is
// rendered by a Text item with textFormat: Text.PlainText (SPEC 7.5 / 2.4).
//
// Consequences, all deliberate:
//   * RAW HTML IS NEVER INTERPRETED. "<script>alert(1)</script>",
//     "<img onerror=...>", "<iframe>" and friends are ordinary paragraph text
//     and reach the screen as the literal characters the file contains. There is
//     no HTML block type, no tag stripping, and no sanitiser to get wrong: there
//     is nothing on the other end that would execute a tag even if one were
//     passed through.
//   * INLINE EMPHASIS IS NOT MARKUP EITHER. The delimiters are removed from
//     `text` and reported separately through the `spans` role as explicit
//     (start, length, style) triples over the FINAL string, which the page
//     renders as plain-text runs. No QML anywhere converts this model's output
//     into StyledText, MarkdownText or RichText.
//   * LINK DESTINATIONS ARE DATA. A destination that is not http(s) and not a
//     plain relative reference — javascript:, data:, vbscript:, mailto:,
//     protocol-relative "//host/x" — is DROPPED: the link text stays, the
//     destination does not, so nothing can be tapped into existence. Even an
//     accepted http(s) destination is never opened without the user confirming
//     it as visible text first (see ViewerMarkdownPage.qml).
//   * IMAGES CARRY A REPO-RELATIVE PATH AND NOTHING ELSE. An absolute path, a
//     path with a ".." segment, or any destination with a scheme yields an EMPTY
//     imagePath, and the page then renders the alt text. A document must not be
//     able to make the client read arbitrary paths, and it must not be able to
//     make the client touch the network, merely by naming them.
//
// SCOPE. A deliberate CommonMark subset — ATX and setext headings, fenced and
// indented code with a language, paragraphs, blockquotes, ordered/unordered/task
// list items with a nesting level, thematic breaks and standalone images — which
// is what a repository's documentation actually uses. Anything outside it
// degrades to paragraph text, which is the safe direction: an unsupported
// construct is shown verbatim rather than half-interpreted.
class MarkdownModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int blockCount READ blockCount NOTIFY blockCountChanged)
public:
    enum Roles {
        // "heading" | "paragraph" | "code" | "listItem" | "quote" | "rule" |
        // "image". Exactly these seven words; the page's delegate switches on
        // them.
        BlockKindRole = Qt::UserRole + 1,
        // The block's plain text, delimiters already removed. For an "image"
        // block this is the ALT text; for "rule" it is empty.
        TextRole,
        // heading: 1..6. listItem: nesting depth, 0 for a top-level item.
        // quote: '>' depth, 1 for a plain quote. Otherwise 0.
        LevelRole,
        // Fenced-code info string's first word, lowercased. Empty for indented
        // code and for every other kind. A LABEL only: nothing highlights, and
        // nothing is executed or fetched on the strength of it.
        LanguageRole,
        // "image" only: the document-relative path, percent-DECODED, already
        // validated by isSafeRelativePath(). Empty when the destination was
        // refused, which is the page's signal to show the alt text instead.
        ImagePathRole,
        // "listItem" only: true for 1. / 1) markers, false for - + *.
        OrderedRole,
        // "listItem" only, and INVALID (undefined in QML) unless the item
        // carries a task marker. Three states, not two: "no checkbox", "[ ]"
        // and "[x]" must be distinguishable, or every bullet would render as an
        // unchecked box.
        CheckedRole,
        // QVariantList of {start, length, strong, emphasis, code, link, target}.
        // Non-overlapping, ascending, and indexed into TextRole's final string
        // — the page can slice `text` by them directly. Positions are QString
        // indices (UTF-16 code units), which is what QML string slicing uses.
        SpansRole,
    };
    Q_ENUM(Roles)

    // One flattened inline style run. `link` with an empty `target` cannot
    // occur: a refused destination produces no span at all.
    struct Span {
        int start = 0;
        int length = 0;
        bool strong = false;
        bool emphasis = false;
        bool code = false;
        QString linkTarget; // non-empty => this run is a link
    };

    struct Block {
        QString kind;
        QString text;
        int level = 0;
        QString language;
        QString imagePath;
        bool ordered = false;
        QVariant checked; // invalid unless the item is a task item
        QVector<Span> spans;
    };

    explicit MarkdownModel(QObject *parent = nullptr);

    // Replace the document. Parsing is synchronous and allocation-bounded by the
    // source length; the source itself is bounded by
    // MobileViewerService::kMaxInlineReadBytes upstream.
    Q_INVOKABLE void setMarkdown(const QString &source);
    Q_INVOKABLE void clear();

    // One block as a map, for tests and for a page that wants the whole row at
    // once rather than eight role lookups.
    Q_INVOKABLE QVariantMap blockAt(int row) const;

    int blockCount() const { return int(m_blocks.size()); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The parser, exposed so it is testable without a model instance and so the
    // page never has a second one.
    static QVector<Block> parse(const QString &source);

    // Whether an IMAGE destination may be turned into a remote read.
    //
    // Requires: non-empty; no URL scheme at all (so http:, data:, javascript:
    // are all out — an image must not reach the network or a pseudo-scheme); not
    // absolute and not protocol-relative, in the '/' spelling OR the '\'
    // spelling; no ".." segment after percent-decoding, counting '\' as a
    // separator too (the decode happens FIRST, so "..%2Fetc" cannot smuggle one
    // past this, and the remote host may be a Windows one where "..\" traverses);
    // no control characters. `decoded`, when given, receives the percent-decoded
    // path that passed — with its original separators, since that is the string
    // the RPC is given.
    static bool isSafeRelativePath(const QString &destination,
                                   QString *decoded = nullptr);

    // The destination a LINK may carry, or an empty string when it must be
    // dropped. http/https absolute URLs and plain relative references pass;
    // every other scheme, protocol-relative "//host", and anything containing
    // whitespace or a control character is refused.
    static QString safeLinkTarget(const QString &destination);

signals:
    void blockCountChanged();

private:
    QVector<Block> m_blocks;
};

} // namespace ch
