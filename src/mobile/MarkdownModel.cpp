#include "MarkdownModel.h"

#include <QRegularExpression>
#include <QStringList>
#include <QStringView>
#include <QUrl>

#include <algorithm>
#include <set>

namespace ch {

namespace {

// Inline style bits. Links are carried by a non-empty target rather than a bit,
// because a link needs the destination anyway and a bit without one would be a
// style nothing could act on.
constexpr int kStyleStrong = 1 << 0;
constexpr int kStyleEmphasis = 1 << 1;
constexpr int kStyleCode = 1 << 2;

// An inline span as the scanner produces it: possibly nested, possibly
// overlapping, half-open [start, end) over the OUTPUT string. flatten() turns
// these into the non-overlapping runs the model publishes.
struct RawSpan {
    int start = 0;
    int end = 0;
    int style = 0;
    QString target;
};

// The one scheme test used by both destination validators. Anchored, and
// deliberately strict about the scheme grammar (RFC 3986): anything that looks
// like a scheme is TREATED as one, so "javascript:alert(1)" and "data:text/html"
// cannot pass themselves off as relative paths.
const QRegularExpression &schemePattern()
{
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.\\-]*:"));
    return re;
}

bool isAsciiPunct(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'!' && u <= u'/') || (u >= u':' && u <= u'@')
           || (u >= u'[' && u <= u'`') || (u >= u'{' && u <= u'~');
}

// Columns of leading whitespace, tabs counted as 4 (CommonMark's tab stop), and
// the index of the first non-whitespace character.
int indentWidth(const QString &line, qsizetype *firstNonSpace)
{
    int width = 0;
    qsizetype i = 0;
    for (; i < line.size(); ++i) {
        if (line.at(i) == QLatin1Char(' '))
            ++width;
        else if (line.at(i) == QLatin1Char('\t'))
            width += 4 - (width % 4);
        else
            break;
    }
    if (firstNonSpace)
        *firstNonSpace = i;
    return width;
}

// Length of the run of `c` starting at `from`, measured to at most `limit`
// characters. `limit` matters where the caller only needs to tell a short run
// from a long one: measuring the WHOLE run at every character of a run of
// 200 000 asterisks is quadratic on server-controlled input, and the emphasis
// scanner cannot tell 4 from 200 000 anyway.
int runLength(QStringView s, qsizetype from, QChar c, qsizetype limit = -1)
{
    const qsizetype end =
        limit < 0 ? s.size() : qMin(s.size(), from + limit);
    qsizetype j = from;
    while (j < end && s.at(j) == c)
        ++j;
    return int(j - from);
}

// Work allowance for ONE block's inline scan, and the reason it exists.
//
// Every delimiter finder below is a linear scan, and a delimiter that never
// closes makes the scanner pay for that scan at every candidate opener. A
// document consisting of 200 000 asterisks — 400 KB, well inside the viewer's
// 8 MiB read cap — therefore cost 200 000 full-length scans, and a document of
// 500 000 '[' cost the same. setMarkdown() runs on the QML thread, so that is
// not slow rendering: it is the whole client frozen until Android's watchdog
// kills it, on a file the user only asked to LOOK at, chosen by whoever wrote
// the file. The budget converts that from a hang into a degradation.
//
// Two independent allowances, because there are two ways to run away:
//   * `steps` bounds the characters the finders may inspect for one block.
//     Exhausted, a finder reports "no close found", which the scanner already
//     handles the only safe way it can: the delimiter becomes a literal
//     character. 4x the block's own length is far more than well-formed prose
//     needs (each finder scan there is local, and every character is inside at
//     most a couple of them), and it makes the worst case linear.
//   * `depth` bounds recursion. scanInline() recurses per nested span, so
//     "*****...x...*****" with 50 000 matched pairs is 50 000 stack frames — a
//     stack overflow, not an exception, and on a mobile UI thread with a
//     smaller stack than a desktop's. Real documents nest emphasis inside a
//     link inside a list item: single digits.
struct InlineLimits {
    qint64 steps = 0;
    int depth = 0;
};

constexpr int kMaxInlineDepth = 24;

InlineLimits limitsFor(qsizetype sourceLength)
{
    return InlineLimits{qint64(4) * sourceLength + 4096, kMaxInlineDepth};
}

// Charge one finder step. False means the allowance is gone and the caller must
// give up looking rather than keep scanning.
bool spend(InlineLimits *limits)
{
    if (limits->steps <= 0)
        return false;
    --limits->steps;
    return true;
}

// Length of the run of `c` at `from`, charged against the allowance. Measuring
// a run is linear work like any other scan, and left uncharged it was the last
// quadratic path in here: a line of 200 000 asterisks re-measured a shrinking
// run at every candidate delimiter, which cost seconds of frozen UI on a file
// the user only asked to look at.
int chargedRunLength(QStringView s, qsizetype from, QChar c,
                     InlineLimits *limits)
{
    const int len = runLength(s, from, c);
    limits->steps = qMax<qint64>(0, limits->steps - len);
    return len;
}

// Closing backtick run of EXACTLY `n` backticks, or -1.
qsizetype findCodeClose(QStringView s, qsizetype from, int n,
                        InlineLimits *limits)
{
    qsizetype i = from;
    while (i < s.size()) {
        if (!spend(limits))
            return -1;
        if (s.at(i) == QLatin1Char('`')) {
            const int len = chargedRunLength(s, i, QLatin1Char('`'), limits);
            if (len == n)
                return i;
            i += len;
            continue;
        }
        ++i;
    }
    return -1;
}

// Closing emphasis run of `c`, at least `n` long, not preceded by whitespace.
// Code spans are SKIPPED whole: a '*' inside `a*b` is not a delimiter, and a
// scanner that thought it was would strip characters out of code.
//
// `underscore` asks for CommonMark's extra right-flanking rule, which applies
// to '_' and not to '*': see the opener's own note in scanInline().
qsizetype findEmphasisClose(QStringView s, qsizetype from, QChar c, int n,
                            bool underscore, InlineLimits *limits)
{
    qsizetype i = from;
    while (i < s.size()) {
        if (!spend(limits))
            return -1;
        const QChar ch = s.at(i);
        if (ch == QLatin1Char('\\')) {
            i += 2;
            continue;
        }
        if (ch == QLatin1Char('`')) {
            const int len = chargedRunLength(s, i, QLatin1Char('`'), limits);
            const qsizetype close = findCodeClose(s, i + len, len, limits);
            i = close < 0 ? i + len : close + len;
            continue;
        }
        if (ch == c) {
            const int len = chargedRunLength(s, i, c, limits);
            const bool wordAfter =
                i + len < s.size() && s.at(i + len).isLetterOrNumber();
            if (len >= n && i > 0 && !s.at(i - 1).isSpace()
                && !(underscore && wordAfter))
                return i;
            i += len;
            continue;
        }
        ++i;
    }
    return -1;
}

// Matching ']' for the '[' at `from - 1`, honouring nesting and escapes.
qsizetype findLinkTextEnd(QStringView s, qsizetype from, InlineLimits *limits)
{
    int depth = 1;
    for (qsizetype i = from; i < s.size(); ++i) {
        if (!spend(limits))
            return -1;
        const QChar ch = s.at(i);
        if (ch == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (ch == QLatin1Char('['))
            ++depth;
        else if (ch == QLatin1Char(']') && --depth == 0)
            return i;
    }
    return -1;
}

// Matching ')' for the '(' at `from - 1`, honouring nesting and escapes.
qsizetype findDestEnd(QStringView s, qsizetype from, InlineLimits *limits)
{
    int depth = 1;
    for (qsizetype i = from; i < s.size(); ++i) {
        if (!spend(limits))
            return -1;
        const QChar ch = s.at(i);
        if (ch == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (ch == QLatin1Char('('))
            ++depth;
        else if (ch == QLatin1Char(')') && --depth == 0)
            return i;
    }
    return -1;
}

// The destination out of a link/image target, discarding any title. Handles the
// <angle-bracketed> spelling, which is how a destination containing spaces is
// written.
QString destinationOf(QStringView raw)
{
    QStringView t = raw.trimmed();
    if (t.startsWith(QLatin1Char('<'))) {
        const qsizetype close = t.indexOf(QLatin1Char('>'));
        // An UNTERMINATED angle destination is not a destination at all —
        // CommonMark does not make "[a](<https://x)" a link. Falling through to
        // the whitespace scan below (which is what happened) handed the page
        // "<https://x" as a destination: a string that names nothing, that
        // safeLinkTarget() cannot recognise as a scheme so it passed as a
        // "relative reference", and that the delegate then drew as a tappable
        // link to a path that does not exist.
        if (close < 0)
            return {};
        return close > 1 ? t.mid(1, close - 1).toString() : QString();
    }
    for (qsizetype i = 0; i < t.size(); ++i) {
        if (t.at(i).isSpace())
            return t.left(i).toString();
    }
    return t.toString();
}

// Matching '>' for the '<' at `from - 1`, or -1. A manual charged loop rather
// than QStringView::indexOf() for the reason InlineLimits documents: indexOf()
// is a full linear scan, and a document made of unmatched '<' would pay for one
// at every single character.
qsizetype findAngleClose(QStringView s, qsizetype from, InlineLimits *limits)
{
    for (qsizetype i = from; i < s.size(); ++i) {
        if (!spend(limits))
            return -1;
        if (s.at(i) == QLatin1Char('>'))
            return i;
    }
    return -1;
}

// Inline scanner. Appends the PLAIN text of `src` to `*out` and records the
// styles it removed in `*spans`, indexed against `*out`.
//
// Everything it does not recognise is COPIED. That is the security property, not
// a shortcut: a '<' that begins an HTML tag, an unmatched '*', a stray '`' and a
// "javascript:" destination all end up as literal characters in the output
// string, which the page renders with textFormat: Text.PlainText. There is no
// path from this function to markup.
//
// `limits` is shared by the whole recursive scan of one block. When it runs out
// the scanner does not fail and does not truncate: every remaining delimiter
// simply stops being recognised and becomes a literal character, which is the
// same degradation an unmatched delimiter already gets.
void scanInline(QStringView src, QString *out, QList<RawSpan> *spans,
                InlineLimits *limits)
{
    // Nothing below may recurse. Refusing to descend makes this level's
    // delimiter literal — see the InlineLimits note on why a stack overflow is
    // the alternative.
    const bool mayRecurse = limits->depth > 0;
    --limits->depth;

    qsizetype i = 0;
    while (i < src.size()) {
        const QChar c = src.at(i);

        if (c == QLatin1Char('\\') && i + 1 < src.size()
            && isAsciiPunct(src.at(i + 1))) {
            out->append(src.at(i + 1));
            i += 2;
            continue;
        }

        if (c == QLatin1Char('`')) {
            const int n = runLength(src, i, QLatin1Char('`'));
            const qsizetype close = findCodeClose(src, i + n, n, limits);
            if (close < 0) {
                out->append(QString(n, QLatin1Char('`')));
                i += n;
                continue;
            }
            QStringView content = src.mid(i + n, close - (i + n));
            // CommonMark strips one space from each end when both ends are
            // spaces and the content is not entirely spaces, so "` `` `" is a
            // code span containing "``".
            const bool allSpaces =
                std::all_of(content.cbegin(), content.cend(),
                            [](QChar ch) { return ch == QLatin1Char(' '); });
            if (content.size() >= 2 && !allSpaces
                && content.front() == QLatin1Char(' ')
                && content.back() == QLatin1Char(' '))
                content = content.mid(1, content.size() - 2);
            const int start = int(out->size());
            // NOT recursed: the contents of a code span are literal by
            // definition, which is also why "<b>" inside one is safe.
            out->append(content);
            spans->append(RawSpan{start, int(out->size()), kStyleCode, {}});
            i = close + n;
            continue;
        }

        if (c == QLatin1Char('*') || c == QLatin1Char('_')) {
            // Runs of 1, 2 and 3 are distinguished; a longer run is treated as
            // 3 and its remainder falls through as literal characters. Capping
            // at 2 instead would leave "***both***" as a strong span whose text
            // still carried a stray asterisk.
            const int n = qMin(runLength(src, i, c, 4), 3);
            const int style = n >= 3 ? (kStyleStrong | kStyleEmphasis)
                                     : (n == 2 ? kStyleStrong : kStyleEmphasis);
            const bool leftFlanking =
                i + n < src.size() && !src.at(i + n).isSpace();
            // CommonMark forbids INTRAWORD '_' emphasis and allows intraword
            // '*', and that asymmetry is not a nicety here: a repository's prose
            // is full of identifiers like `snake_case_name` and `__init__`, and
            // treating those underscores as delimiters DELETED them from the
            // text — "snake_case_name" reached the screen as "snakecasename"
            // with the middle word italicised. The mirror-image rule for the
            // closing run lives in findEmphasisClose().
            const bool underscore = c == QLatin1Char('_');
            const bool wordBefore = i > 0 && src.at(i - 1).isLetterOrNumber();
            if (leftFlanking && mayRecurse && !(underscore && wordBefore)) {
                const qsizetype close =
                    findEmphasisClose(src, i + n, c, n, underscore, limits);
                if (close >= 0) {
                    const int start = int(out->size());
                    scanInline(src.mid(i + n, close - (i + n)), out, spans,
                               limits);
                    spans->append(RawSpan{start, int(out->size()), style, {}});
                    i = close + n;
                    continue;
                }
            }
            out->append(c);
            ++i;
            continue;
        }

        if (c == QLatin1Char('!') && i + 1 < src.size()
            && src.at(i + 1) == QLatin1Char('[') && mayRecurse) {
            // An image EMBEDDED in prose. Only a standalone image becomes an
            // "image" block; one inside a sentence is reduced to its alt text,
            // because a native ListView delegate cannot flow a remote image
            // through a line of text and a half-drawn one is worse than the alt
            // text the author wrote for exactly this case. The destination is
            // dropped entirely — nothing is fetched.
            const qsizetype textEnd = findLinkTextEnd(src, i + 2, limits);
            if (textEnd > 0 && textEnd + 1 < src.size()
                && src.at(textEnd + 1) == QLatin1Char('(')) {
                const qsizetype destEnd = findDestEnd(src, textEnd + 2, limits);
                if (destEnd > 0) {
                    scanInline(src.mid(i + 2, textEnd - (i + 2)), out, spans,
                               limits);
                    i = destEnd + 1;
                    continue;
                }
            }
            out->append(c);
            ++i;
            continue;
        }

        if (c == QLatin1Char('[') && mayRecurse) {
            const qsizetype textEnd = findLinkTextEnd(src, i + 1, limits);
            if (textEnd > 0 && textEnd + 1 < src.size()
                && src.at(textEnd + 1) == QLatin1Char('(')) {
                const qsizetype destEnd = findDestEnd(src, textEnd + 2, limits);
                if (destEnd > 0) {
                    const int start = int(out->size());
                    scanInline(src.mid(i + 1, textEnd - (i + 1)), out, spans,
                               limits);
                    const QString target = MarkdownModel::safeLinkTarget(
                        destinationOf(src.mid(textEnd + 2,
                                              destEnd - (textEnd + 2))));
                    // A refused destination leaves NO span: the text stays, the
                    // link does not exist, and there is nothing for the page to
                    // offer to open.
                    if (!target.isEmpty())
                        spans->append(
                            RawSpan{start, int(out->size()), 0, target});
                    i = destEnd + 1;
                    continue;
                }
            }
            out->append(c);
            ++i;
            continue;
        }

        if (c == QLatin1Char('<')) {
            const qsizetype close = findAngleClose(src, i + 1, limits);
            if (close > i + 1) {
                const QString inner = src.mid(i + 1, close - i - 1).toString();
                const bool looksLikeAutolink =
                    !inner.contains(QLatin1Char(' '))
                    && !inner.contains(QLatin1Char('<'))
                    && schemePattern().match(inner).hasMatch();
                if (looksLikeAutolink) {
                    const QString target =
                        MarkdownModel::safeLinkTarget(inner);
                    if (!target.isEmpty()) {
                        const int start = int(out->size());
                        out->append(inner);
                        spans->append(
                            RawSpan{start, int(out->size()), 0, target});
                        i = close + 1;
                        continue;
                    }
                }
            }
            // Not an autolink: copied verbatim. This is the branch that puts
            // "<script>" on screen as five visible characters and a word.
            out->append(c);
            ++i;
            continue;
        }

        out->append(c);
        ++i;
    }

    ++limits->depth;
}

// Collapse possibly nested raw spans into ascending, non-overlapping runs.
//
// A BOUNDARY sweep rather than one entry per character. The per-character form
// this replaces allocated an int of style bits plus a QString destination for
// every character of every block — 28 bytes each, so over 200 MiB of transient
// allocation for a document at the viewer's 8 MiB read cap, on a phone, for a
// document that in practice carries a handful of spans. This is O(spans log
// spans) in time and O(spans) in memory, and produces byte-for-byte the same
// runs.
QVector<MarkdownModel::Span> flatten(const QString &text,
                                     const QList<RawSpan> &raw)
{
    const int n = int(text.size());
    if (raw.isEmpty() || n == 0)
        return {};

    struct Event {
        int at = 0;
        int span = 0;
        bool opening = false;
    };
    QVector<Event> events;
    events.reserve(raw.size() * 2);
    for (int i = 0; i < raw.size(); ++i) {
        const int from = qBound(0, raw.at(i).start, n);
        const int to = qBound(0, raw.at(i).end, n);
        if (from >= to)
            continue;
        events.append(Event{from, i, true});
        events.append(Event{to, i, false});
    }
    if (events.isEmpty())
        return {};
    // By POSITION only. Opens and closes at the same position are all applied
    // before the run starting there is emitted, so their relative order cannot
    // matter, and a stable_sort would buy nothing.
    std::sort(events.begin(), events.end(),
              [](const Event &a, const Event &b) { return a.at < b.at; });

    // Counts, not a single OR: a style bit must survive until the LAST span
    // carrying it closes, and a plain OR could not be undone on a close.
    int strongDepth = 0;
    int emphasisDepth = 0;
    int codeDepth = 0;
    // Open spans that carry a destination, keyed by their index in `raw`, so
    // begin() is the lowest — i.e. the FIRST target recorded for this position
    // wins, which is the rule the per-character sweep implemented by only
    // writing an empty slot. The scanner appends a span after recursing into its
    // content, so inner spans come first and the innermost destination is the
    // one kept. CommonMark forbids nesting links at all; this only decides the
    // answer for a document that tried.
    std::set<int> linked;

    QVector<MarkdownModel::Span> out;
    qsizetype e = 0;
    while (e < events.size()) {
        const int at = events.at(e).at;
        while (e < events.size() && events.at(e).at == at) {
            const Event &event = events.at(e);
            const RawSpan &span = raw.at(event.span);
            const int delta = event.opening ? 1 : -1;
            if (span.style & kStyleStrong)
                strongDepth += delta;
            if (span.style & kStyleEmphasis)
                emphasisDepth += delta;
            if (span.style & kStyleCode)
                codeDepth += delta;
            if (!span.target.isEmpty()) {
                if (event.opening)
                    linked.insert(event.span);
                else
                    linked.erase(event.span);
            }
            ++e;
        }
        if (e >= events.size())
            break; // the last boundary closes everything; nothing left to cover
        MarkdownModel::Span span;
        span.start = at;
        span.length = events.at(e).at - at;
        span.strong = strongDepth > 0;
        span.emphasis = emphasisDepth > 0;
        span.code = codeDepth > 0;
        if (!linked.empty())
            span.linkTarget = raw.at(*linked.begin()).target;
        // A gap between spans is a plain run and is not published at all.
        if (!span.strong && !span.emphasis && !span.code
            && span.linkTarget.isEmpty())
            continue;
        // Adjacent boundaries that changed nothing observable are merged, so the
        // published runs are the MAXIMAL ones a page can slice `text` by.
        if (!out.isEmpty()) {
            MarkdownModel::Span &last = out.last();
            if (last.start + last.length == span.start
                && last.strong == span.strong && last.emphasis == span.emphasis
                && last.code == span.code
                && last.linkTarget == span.linkTarget) {
                last.length += span.length;
                continue;
            }
        }
        out.append(span);
    }
    return out;
}

// Fill `text` and `spans` of `block` from one markdown source string.
void applyInline(MarkdownModel::Block *block, const QString &source)
{
    QString text;
    text.reserve(source.size());
    QList<RawSpan> raw;
    InlineLimits limits = limitsFor(source.size());
    scanInline(QStringView(source), &text, &raw, &limits);
    block->spans = flatten(text, raw);
    block->text = text;
}

// ---- block-level recognisers -----------------------------------------------

struct Fence {
    bool valid = false;
    QChar ch;
    int length = 0;
    int indent = 0;
    QString language;
    bool hasInfo = false;
};

Fence fenceAt(const QString &line)
{
    Fence fence;
    qsizetype first = 0;
    const int indent = indentWidth(line, &first);
    if (indent > 3 || first >= line.size())
        return fence;
    const QChar c = line.at(first);
    if (c != QLatin1Char('`') && c != QLatin1Char('~'))
        return fence;
    const int len = runLength(QStringView(line), first, c);
    if (len < 3)
        return fence;
    const QString info = line.mid(first + len).trimmed();
    // A backtick fence's info string may not contain a backtick, which is what
    // keeps "``inline``" from being read as a fence.
    if (c == QLatin1Char('`') && info.contains(QLatin1Char('`')))
        return fence;
    fence.valid = true;
    fence.ch = c;
    fence.length = len;
    fence.indent = indent;
    fence.hasInfo = !info.isEmpty();
    // The first word only, lowercased: a LABEL, never anything acted upon.
    for (qsizetype i = 0; i < info.size(); ++i) {
        if (info.at(i).isSpace())
            break;
        fence.language.append(info.at(i).toLower());
    }
    return fence;
}

bool isThematicBreak(const QString &line)
{
    qsizetype first = 0;
    if (indentWidth(line, &first) > 3 || first >= line.size())
        return false;
    const QChar marker = line.at(first);
    if (marker != QLatin1Char('-') && marker != QLatin1Char('*')
        && marker != QLatin1Char('_'))
        return false;
    int count = 0;
    for (qsizetype i = first; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == marker)
            ++count;
        else if (!c.isSpace())
            return false;
    }
    return count >= 3;
}

bool atxHeading(const QString &line, int *level, QString *text)
{
    qsizetype first = 0;
    if (indentWidth(line, &first) > 3 || first >= line.size())
        return false;
    if (line.at(first) != QLatin1Char('#'))
        return false;
    const int hashes = runLength(QStringView(line), first, QLatin1Char('#'));
    if (hashes > 6)
        return false;
    const qsizetype after = first + hashes;
    if (after < line.size() && !line.at(after).isSpace())
        return false;
    QString body = line.mid(after).trimmed();
    // A closing sequence of '#' is decoration and is dropped, but only when it
    // is separated from the text ("# a #" -> "a", "# a#" -> "a#").
    qsizetype end = body.size();
    while (end > 0 && body.at(end - 1) == QLatin1Char('#'))
        --end;
    if (end < body.size() && (end == 0 || body.at(end - 1).isSpace()))
        body = body.left(end).trimmed();
    *level = hashes;
    *text = body;
    return true;
}

// 1 for a '=' underline, 2 for a '-' underline, 0 for anything else. Only
// meaningful with an open paragraph, which is the caller's business.
int setextLevel(const QString &line)
{
    qsizetype first = 0;
    if (indentWidth(line, &first) > 3 || first >= line.size())
        return 0;
    const QChar marker = line.at(first);
    if (marker != QLatin1Char('=') && marker != QLatin1Char('-'))
        return 0;
    for (qsizetype i = first; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c != marker && !c.isSpace())
            return 0;
        // Trailing spaces are allowed, interior ones are not: "- - -" is a
        // thematic break, not an underline.
        if (c.isSpace()) {
            for (qsizetype j = i; j < line.size(); ++j) {
                if (!line.at(j).isSpace())
                    return 0;
            }
            break;
        }
    }
    return marker == QLatin1Char('=') ? 1 : 2;
}

bool quoteLine(const QString &line, int *depth, QString *rest)
{
    qsizetype first = 0;
    if (indentWidth(line, &first) > 3 || first >= line.size())
        return false;
    if (line.at(first) != QLatin1Char('>'))
        return false;
    int levels = 0;
    qsizetype i = first;
    while (i < line.size()) {
        if (line.at(i) == QLatin1Char('>')) {
            ++levels;
            ++i;
            // One optional space after each marker belongs to the marker.
            if (i < line.size() && line.at(i) == QLatin1Char(' '))
                ++i;
            continue;
        }
        break;
    }
    *depth = levels;
    *rest = line.mid(i);
    return true;
}

struct ListMarker {
    bool valid = false;
    bool ordered = false;
    int indent = 0;        // columns before the marker
    int contentIndent = 0; // columns before the item's content
    QString text;
};

ListMarker listMarkerAt(const QString &line)
{
    ListMarker marker;
    qsizetype first = 0;
    const int indent = indentWidth(line, &first);
    if (first >= line.size())
        return marker;
    qsizetype i = first;
    const QChar c = line.at(i);
    if (c == QLatin1Char('-') || c == QLatin1Char('+') || c == QLatin1Char('*')) {
        ++i;
    } else if (c.isDigit()) {
        int digits = 0;
        while (i < line.size() && line.at(i).isDigit() && digits < 9) {
            ++i;
            ++digits;
        }
        if (i >= line.size()
            || (line.at(i) != QLatin1Char('.') && line.at(i) != QLatin1Char(')')))
            return marker;
        ++i;
        marker.ordered = true;
    } else {
        return marker;
    }
    // A marker must be followed by whitespace or end the line; "-5" and "1.5"
    // are not list items.
    if (i < line.size() && !line.at(i).isSpace())
        return marker;
    const qsizetype markerEnd = i;
    while (i < line.size() && line.at(i).isSpace())
        ++i;
    marker.valid = true;
    marker.indent = indent;
    marker.contentIndent = indent + int(i - first);
    if (i == markerEnd) // marker ends the line
        marker.contentIndent = indent + int(markerEnd - first) + 1;
    marker.text = line.mid(i);
    return marker;
}

// Split a leading task marker off a list item's text. Returns an invalid
// QVariant when there is none — three states, see CheckedRole.
QVariant takeTaskMarker(QString *text)
{
    if (text->size() < 3 || text->at(0) != QLatin1Char('['))
        return {};
    if (text->at(2) != QLatin1Char(']'))
        return {};
    const QChar box = text->at(1);
    bool checked = false;
    if (box == QLatin1Char(' '))
        checked = false;
    else if (box == QLatin1Char('x') || box == QLatin1Char('X'))
        checked = true;
    else
        return {};
    if (text->size() > 3 && !text->at(3).isSpace())
        return {};
    *text = text->mid(3).trimmed();
    return checked;
}

// Whether `line` is a standalone image, i.e. a paragraph consisting of nothing
// but "![alt](dest)".
bool standaloneImage(const QString &line, QString *alt, QString *destination)
{
    const QString trimmed = line.trimmed();
    if (!trimmed.startsWith(QLatin1String("![")))
        return false;
    const QStringView view(trimmed);
    // An allowance of its own. These are two single passes over one line, not
    // the recursive scan applyInline() runs, but the finders are shared and they
    // account unconditionally.
    InlineLimits limits = limitsFor(view.size());
    const qsizetype textEnd = findLinkTextEnd(view, 2, &limits);
    if (textEnd < 0 || textEnd + 1 >= view.size()
        || view.at(textEnd + 1) != QLatin1Char('('))
        return false;
    const qsizetype destEnd = findDestEnd(view, textEnd + 2, &limits);
    // Anything after the closing paren means this is prose that happens to
    // start with an image, not an image block.
    if (destEnd != view.size() - 1)
        return false;
    *alt = view.mid(2, textEnd - 2).toString();
    *destination = destinationOf(view.mid(textEnd + 2, destEnd - (textEnd + 2)));
    return true;
}

// Remove up to `columns` columns of leading whitespace.
QString stripIndent(const QString &line, int columns)
{
    int removed = 0;
    qsizetype i = 0;
    while (i < line.size() && removed < columns) {
        const QChar c = line.at(i);
        if (c == QLatin1Char(' '))
            ++removed;
        else if (c == QLatin1Char('\t'))
            removed += 4 - (removed % 4);
        else
            break;
        ++i;
    }
    return line.mid(i);
}

// A line that ends a blockquote's lazy continuation: it starts a block of its
// own, so swallowing it into the quote would hide a heading or a code fence.
bool startsNewBlock(const QString &line)
{
    int level = 0;
    QString text;
    return fenceAt(line).valid || isThematicBreak(line)
           || atxHeading(line, &level, &text) || listMarkerAt(line).valid;
}

QVariantList spansOf(const MarkdownModel::Block &block)
{
    QVariantList out;
    out.reserve(block.spans.size());
    for (const MarkdownModel::Span &span : block.spans) {
        out.append(QVariantMap{
            {QStringLiteral("start"), span.start},
            {QStringLiteral("length"), span.length},
            {QStringLiteral("strong"), span.strong},
            {QStringLiteral("emphasis"), span.emphasis},
            {QStringLiteral("code"), span.code},
            {QStringLiteral("link"), !span.linkTarget.isEmpty()},
            {QStringLiteral("target"), span.linkTarget},
        });
    }
    return out;
}

} // namespace

MarkdownModel::MarkdownModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

QVector<MarkdownModel::Block> MarkdownModel::parse(const QString &source)
{
    static const QRegularExpression lineBreak(QStringLiteral("\r\n|\n|\r"));
    QStringList lines = source.split(lineBreak);
    // A document that ends in a newline — which is nearly all of them — splits
    // into a final EMPTY element that is not a line of the file at all. It is
    // invisible in prose, which gets trimmed, but a code fence copies its body
    // verbatim: left in, an unterminated fence gains a trailing blank line that
    // a terminated one (whose closing marker stops the scan earlier) never has.
    // Same document, two different code blocks, decided by whether the author
    // remembered the closing ```.
    if (lines.size() > 1 && lines.last().isEmpty())
        lines.removeLast();

    QVector<Block> blocks;
    QStringList paragraph;
    // Indent columns of the enclosing list markers, innermost last. The DEPTH of
    // this stack is a list item's `level`; the columns themselves only decide
    // whether the next marker is a sibling, a child or a pop.
    QVector<int> listIndents;

    const auto flushParagraph = [&]() {
        if (paragraph.isEmpty())
            return;
        QStringList trimmed;
        trimmed.reserve(paragraph.size());
        for (const QString &line : std::as_const(paragraph))
            trimmed.append(line.trimmed());
        paragraph.clear();
        // A paragraph at top level ends any list that was open.
        listIndents.clear();

        QString alt;
        QString destination;
        if (trimmed.size() == 1 && standaloneImage(trimmed.first(), &alt, &destination)) {
            Block block;
            block.kind = QStringLiteral("image");
            applyInline(&block, alt);
            // Spans are dropped for an image: the alt text is a fallback label,
            // and styling a fallback would be noise.
            block.spans.clear();
            QString decoded;
            if (isSafeRelativePath(destination, &decoded))
                block.imagePath = decoded;
            blocks.append(block);
            return;
        }

        Block block;
        block.kind = QStringLiteral("paragraph");
        // Joined with '\n', not ' ': the author's line structure is the only
        // layout information a plain-text renderer has, and a hard-wrapped
        // paragraph reads correctly either way while an ASCII table or a signed
        // block does not.
        applyInline(&block, trimmed.join(QLatin1Char('\n')));
        blocks.append(block);
    };

    qsizetype i = 0;
    while (i < lines.size()) {
        const QString &line = lines.at(i);

        if (line.trimmed().isEmpty()) {
            // A blank line closes a paragraph but NOT a list: a loose list has
            // blank lines between its items.
            flushParagraph();
            ++i;
            continue;
        }

        const Fence fence = fenceAt(line);
        if (fence.valid) {
            flushParagraph();
            listIndents.clear();
            QStringList body;
            qsizetype j = i + 1;
            bool closed = false;
            for (; j < lines.size(); ++j) {
                const Fence closing = fenceAt(lines.at(j));
                if (closing.valid && closing.ch == fence.ch
                    && closing.length >= fence.length && !closing.hasInfo) {
                    closed = true;
                    break;
                }
                body.append(stripIndent(lines.at(j), fence.indent));
            }
            Block block;
            block.kind = QStringLiteral("code");
            block.language = fence.language;
            // Literal: no inline scan, no spans. The contents of a code block
            // are exactly the file's bytes.
            block.text = body.join(QLatin1Char('\n'));
            blocks.append(block);
            // An UNTERMINATED fence runs to end of document, which is what
            // CommonMark says and also the safe reading: the alternative is to
            // fall back to treating the rest of the file as prose, and then a
            // truncated document's code would be re-interpreted as markup.
            i = closed ? j + 1 : j;
            continue;
        }

        // A setext underline only exists relative to an open paragraph, and it
        // beats a thematic break ("text\n---" is an H2, not text plus a rule).
        if (!paragraph.isEmpty()) {
            const int level = setextLevel(line);
            if (level > 0) {
                QStringList trimmed;
                trimmed.reserve(paragraph.size());
                for (const QString &pending : std::as_const(paragraph))
                    trimmed.append(pending.trimmed());
                paragraph.clear();
                listIndents.clear();
                Block block;
                block.kind = QStringLiteral("heading");
                block.level = level;
                applyInline(&block, trimmed.join(QLatin1Char(' ')));
                blocks.append(block);
                ++i;
                continue;
            }
        }

        if (isThematicBreak(line)) {
            flushParagraph();
            listIndents.clear();
            Block block;
            block.kind = QStringLiteral("rule");
            blocks.append(block);
            ++i;
            continue;
        }

        int level = 0;
        QString headingText;
        if (atxHeading(line, &level, &headingText)) {
            flushParagraph();
            listIndents.clear();
            Block block;
            block.kind = QStringLiteral("heading");
            block.level = level;
            applyInline(&block, headingText);
            blocks.append(block);
            ++i;
            continue;
        }

        int quoteDepth = 0;
        QString quoteRest;
        if (quoteLine(line, &quoteDepth, &quoteRest)) {
            flushParagraph();
            listIndents.clear();
            QStringList body{quoteRest};
            qsizetype j = i + 1;
            while (j < lines.size()) {
                int depth = 0;
                QString rest;
                if (quoteLine(lines.at(j), &depth, &rest)) {
                    body.append(rest);
                    ++j;
                    continue;
                }
                if (lines.at(j).trimmed().isEmpty() || startsNewBlock(lines.at(j)))
                    break;
                // Lazy continuation: a quote's paragraph may run onto an
                // unmarked line.
                body.append(lines.at(j).trimmed());
                ++j;
            }
            Block block;
            block.kind = QStringLiteral("quote");
            block.level = quoteDepth;
            applyInline(&block, body.join(QLatin1Char('\n')).trimmed());
            blocks.append(block);
            i = j;
            continue;
        }

        const ListMarker marker = listMarkerAt(line);
        if (marker.valid) {
            flushParagraph();
            while (!listIndents.isEmpty() && marker.indent < listIndents.last())
                listIndents.removeLast();
            if (listIndents.isEmpty() || marker.indent > listIndents.last())
                listIndents.append(marker.indent);

            QString text = marker.text;
            qsizetype j = i + 1;
            while (j < lines.size()) {
                const QString &continuation = lines.at(j);
                if (continuation.trimmed().isEmpty())
                    break;
                if (listMarkerAt(continuation).valid || startsNewBlock(continuation))
                    break;
                qsizetype first = 0;
                if (indentWidth(continuation, &first) < marker.contentIndent)
                    break;
                text += QLatin1Char('\n');
                text += continuation.trimmed();
                ++j;
            }

            Block block;
            block.kind = QStringLiteral("listItem");
            block.ordered = marker.ordered;
            block.level = int(listIndents.size()) - 1;
            block.checked = takeTaskMarker(&text);
            applyInline(&block, text);
            blocks.append(block);
            i = j;
            continue;
        }

        qsizetype firstNonSpace = 0;
        if (paragraph.isEmpty() && listIndents.isEmpty()
            && indentWidth(line, &firstNonSpace) >= 4) {
            QStringList body;
            qsizetype j = i;
            for (; j < lines.size(); ++j) {
                const QString &candidate = lines.at(j);
                if (candidate.trimmed().isEmpty()) {
                    body.append(QString());
                    continue;
                }
                qsizetype first = 0;
                if (indentWidth(candidate, &first) < 4)
                    break;
                body.append(stripIndent(candidate, 4));
            }
            while (!body.isEmpty() && body.last().isEmpty())
                body.removeLast();
            Block block;
            block.kind = QStringLiteral("code");
            // Indented code carries no info string, so there is no language to
            // report and none is guessed from the file's extension.
            block.text = body.join(QLatin1Char('\n'));
            blocks.append(block);
            i = j;
            continue;
        }

        paragraph.append(line);
        ++i;
    }

    flushParagraph();
    return blocks;
}

void MarkdownModel::setMarkdown(const QString &source)
{
    beginResetModel();
    m_blocks = parse(source);
    endResetModel();
    emit blockCountChanged();
}

void MarkdownModel::clear()
{
    if (m_blocks.isEmpty())
        return;
    beginResetModel();
    m_blocks.clear();
    endResetModel();
    emit blockCountChanged();
}

int MarkdownModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_blocks.size());
}

QVariant MarkdownModel::data(const QModelIndex &index, int role) const
{
    if (index.parent().isValid() || index.row() < 0
        || index.row() >= m_blocks.size())
        return {};
    const Block &block = m_blocks.at(index.row());
    switch (role) {
    case BlockKindRole:
        return block.kind;
    case TextRole:
        return block.text;
    case LevelRole:
        return block.level;
    case LanguageRole:
        return block.language;
    case ImagePathRole:
        return block.imagePath;
    case OrderedRole:
        return block.ordered;
    case CheckedRole:
        return block.checked;
    case SpansRole:
        return spansOf(block);
    default:
        return {};
    }
}

QHash<int, QByteArray> MarkdownModel::roleNames() const
{
    return {
        {BlockKindRole, QByteArrayLiteral("blockKind")},
        {TextRole, QByteArrayLiteral("text")},
        {LevelRole, QByteArrayLiteral("level")},
        {LanguageRole, QByteArrayLiteral("language")},
        {ImagePathRole, QByteArrayLiteral("imagePath")},
        {OrderedRole, QByteArrayLiteral("ordered")},
        {CheckedRole, QByteArrayLiteral("checked")},
        {SpansRole, QByteArrayLiteral("spans")},
    };
}

QVariantMap MarkdownModel::blockAt(int row) const
{
    if (row < 0 || row >= m_blocks.size())
        return {};
    const Block &block = m_blocks.at(row);
    return QVariantMap{
        {QStringLiteral("blockKind"), block.kind},
        {QStringLiteral("text"), block.text},
        {QStringLiteral("level"), block.level},
        {QStringLiteral("language"), block.language},
        {QStringLiteral("imagePath"), block.imagePath},
        {QStringLiteral("ordered"), block.ordered},
        {QStringLiteral("checked"), block.checked},
        {QStringLiteral("spans"), spansOf(block)},
    };
}

bool MarkdownModel::isSafeRelativePath(const QString &destination,
                                       QString *decoded)
{
    const QString trimmed = destination.trimmed();
    if (trimmed.isEmpty())
        return false;
    // Decoded FIRST, so "..%2Fetc%2Fpasswd" cannot smuggle a parent-directory
    // segment past the check below. Everything after this point reasons about
    // the path the RPC would actually be given.
    const QString path = QUrl::fromPercentEncoding(trimmed.toUtf8());
    if (path.isEmpty())
        return false;
    for (const QChar c : path) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return false;
    }
    // ANY scheme disqualifies an image. http(s) included: a document must not be
    // able to make this client fetch from the network on its behalf (SPEC 7.5),
    // and the mobile client has no sandboxed surface to fetch into.
    if (schemePattern().match(path).hasMatch())
        return false;
    // Absolute in either spelling. '/' covers POSIX and "//host/x" with it;
    // '\\' covers a Windows-rooted path and a "\\\\host\\share" UNC name.
    if (path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1Char('\\')))
        return false;
    if (path.startsWith(QLatin1Char('#')))
        return false;
    // Segments are split on BOTH separators. A POSIX daemon reads "a\..\..\b" as
    // one oddly-named file, so this looks like paranoia — but the daemon is
    // whatever `codeharbord` is running on, Windows OpenSSH included, and there
    // the backslash form is a REAL parent-directory traversal that the '/'-only
    // split waved through. Deciding that here rather than trusting the remote
    // host's path grammar costs a filename nobody writes its picture, and the
    // refusal only means the page shows the alt text.
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList segments = normalized.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (segment == QLatin1String(".."))
            return false;
    }
    if (decoded)
        *decoded = path;
    return true;
}

QString MarkdownModel::safeLinkTarget(const QString &destination)
{
    const QString target = destination.trimmed();
    if (target.isEmpty())
        return {};
    for (const QChar c : target) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || c.isSpace())
            return {};
    }
    // Protocol-relative: no scheme to inspect, but still a network address.
    if (target.startsWith(QLatin1String("//")))
        return {};
    const auto match = schemePattern().match(target);
    if (match.hasMatch()) {
        const QString scheme =
            target.left(match.capturedLength() - 1).toLower();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
            return {};
    }
    return target;
}

} // namespace ch
