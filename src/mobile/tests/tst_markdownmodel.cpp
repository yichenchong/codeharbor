#include "MarkdownModel.h"

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest/QtTest>

using ch::MarkdownModel;

namespace {

// One block as {role name -> value}, which is how every assertion below reads.
QVariantMap blockOf(const MarkdownModel &model, int row)
{
    return model.blockAt(row);
}

QString kindOf(const MarkdownModel &model, int row)
{
    return blockOf(model, row).value(QStringLiteral("blockKind")).toString();
}

QString textOf(const MarkdownModel &model, int row)
{
    return blockOf(model, row).value(QStringLiteral("text")).toString();
}

QVariantList spansOf(const MarkdownModel &model, int row)
{
    return blockOf(model, row).value(QStringLiteral("spans")).toList();
}

} // namespace

class TstMarkdownModel : public QObject {
    Q_OBJECT
private slots:
    void rawHtmlIsNeverInterpretedAndArrivesAsLiteralText();
    void dangerousLinkDestinationsAreDroppedNotRendered();
    void imageDestinationsThatEscapeTheProjectAreRefused();
    void anUnterminatedFenceStaysCodeToTheEndOfTheDocument();
    void fencedAndIndentedCodeCarryTheRightLanguage();
    void deeplyNestedListsCarryTheirNestingLevel();
    void taskMarkersHaveThreeStatesNotTwo();
    void headingsInBothSpellings();
    void aSetextUnderlineBeatsAThematicBreak();
    void quotesAndRules();
    void emphasisBecomesSpansAndNotMarkup();
    void inlineImagesInProseAreReducedToTheirAltText();
    void everyRoleIsPublishedUnderItsContractedName();
    void destinationValidatorsInIsolation();
    void intrawordUnderscoresAreNotEmphasis();
    void anUnterminatedAngleDestinationIsNotADestination();
    void adversarialInlineInputCannotHangOrOverflowTheParser();
};

// The single most important property of this whole model. A markdown document is
// SERVER-CONTROLLED data, and on mobile there is no Chromium sandbox to render it
// in — so it is never rendered as markup at all. Every tag below has to survive
// as the literal characters the file contains.
void TstMarkdownModel::rawHtmlIsNeverInterpretedAndArrivesAsLiteralText()
{
    const QStringList fixtures{
        QStringLiteral("<script>alert(\"pwned\")</script>"),
        QStringLiteral("<img src=x onerror=alert(1)>"),
        QStringLiteral("<iframe src=\"http://evil.example/x\"></iframe>"),
        QStringLiteral("<a href=\"javascript:alert(1)\">tap here</a>"),
        QStringLiteral("<b>not bold</b>"),
        QStringLiteral("<!-- a comment -->"),
        QStringLiteral("<style>body{display:none}</style>"),
        QStringLiteral("<![CDATA[<script>alert(1)</script>]]>"),
        QStringLiteral("<div onmouseover=\"steal()\" onload=x>hover</div>"),
        QStringLiteral("&lt;script&gt;alert(1)&lt;/script&gt;"),
        QStringLiteral("<svg/onload=alert(1)>"),
    };

    for (const QString &fixture : fixtures) {
        MarkdownModel model;
        model.setMarkdown(fixture);
        QCOMPARE(model.rowCount(), 1);
        // There is no "html" block kind, on purpose: an HTML block is prose.
        QCOMPARE(kindOf(model, 0), QStringLiteral("paragraph"));
        QCOMPARE(textOf(model, 0), fixture);
        // And nothing about it produced a style span either, so no delegate can
        // be talked into treating part of it specially.
        QCOMPARE(spansOf(model, 0).size(), 0);
    }

    // Tags mixed into real prose survive with the prose.
    MarkdownModel model;
    model.setMarkdown(QStringLiteral(
        "Run <script>fetch('/etc/passwd')</script> to see."));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(textOf(model, 0),
             QStringLiteral("Run <script>fetch('/etc/passwd')</script> to see."));
}

void TstMarkdownModel::dangerousLinkDestinationsAreDroppedNotRendered()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral(
        "[click](javascript:alert(1))\n"
        "\n"
        "[x](data:text/html;base64,PHNjcmlwdD4=)\n"
        "\n"
        "[y](vbscript:msgbox)\n"
        "\n"
        "[z](//evil.example/x)\n"
        "\n"
        "[m](mailto:someone@example.com)\n"
        "\n"
        "[ok](https://example.com/a)\n"
        "\n"
        "[rel](docs/a.md)\n"
        "\n"
        "<https://example.com/auto>\n"
        "\n"
        "<javascript:alert(1)>\n"));
    QCOMPARE(model.rowCount(), 9);

    // The refused five: the link TEXT survives (the author wrote it), the
    // destination does not exist at all, so there is nothing for a delegate to
    // offer to open.
    const QStringList refusedText{QStringLiteral("click"), QStringLiteral("x"),
                                  QStringLiteral("y"), QStringLiteral("z"),
                                  QStringLiteral("m")};
    for (int row = 0; row < refusedText.size(); ++row) {
        QCOMPARE(kindOf(model, row), QStringLiteral("paragraph"));
        QCOMPARE(textOf(model, row), refusedText.at(row));
        QCOMPARE(spansOf(model, row).size(), 0);
    }

    // https passes, as data.
    QCOMPARE(textOf(model, 5), QStringLiteral("ok"));
    QCOMPARE(spansOf(model, 5).size(), 1);
    QVariantMap span = spansOf(model, 5).first().toMap();
    QCOMPARE(span.value(QStringLiteral("start")).toInt(), 0);
    QCOMPARE(span.value(QStringLiteral("length")).toInt(), 2);
    QCOMPARE(span.value(QStringLiteral("link")).toBool(), true);
    QCOMPARE(span.value(QStringLiteral("target")).toString(),
             QStringLiteral("https://example.com/a"));

    // A plain relative reference passes too — it is a repository path, and the
    // page decides what to do with it (which is: show it, and ask).
    QCOMPARE(textOf(model, 6), QStringLiteral("rel"));
    span = spansOf(model, 6).first().toMap();
    QCOMPARE(span.value(QStringLiteral("target")).toString(),
             QStringLiteral("docs/a.md"));

    // An http(s) autolink becomes its own text plus a link span...
    QCOMPARE(textOf(model, 7), QStringLiteral("https://example.com/auto"));
    QCOMPARE(spansOf(model, 7).size(), 1);
    QCOMPARE(spansOf(model, 7).first().toMap().value(QStringLiteral("target")).toString(),
             QStringLiteral("https://example.com/auto"));

    // ...while a javascript: autolink is not an autolink at all: it falls through
    // to the literal-'<' branch, angle brackets and all.
    QCOMPARE(textOf(model, 8), QStringLiteral("<javascript:alert(1)>"));
    QCOMPARE(spansOf(model, 8).size(), 0);
}

void TstMarkdownModel::imageDestinationsThatEscapeTheProjectAreRefused()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral(
        "![up](../../etc/passwd)\n"
        "\n"
        "![abs](/etc/passwd)\n"
        "\n"
        "![encoded](..%2Fetc%2Fpasswd)\n"
        "\n"
        "![remote](http://evil.example/x.png)\n"
        "\n"
        "![proto](//evil.example/x.png)\n"
        "\n"
        "![inner](sub/../../logo.png)\n"
        "\n"
        "![ok](assets/logo.png)\n"));
    QCOMPARE(model.rowCount(), 7);

    // Six refusals. Each is an "image" block whose imagePath is EMPTY, which is
    // the page's signal to render the alt text and fetch nothing at all.
    const QStringList refusedAlt{
        QStringLiteral("up"),     QStringLiteral("abs"),
        QStringLiteral("encoded"), QStringLiteral("remote"),
        QStringLiteral("proto"),  QStringLiteral("inner")};
    for (int row = 0; row < refusedAlt.size(); ++row) {
        QCOMPARE(kindOf(model, row), QStringLiteral("image"));
        QCOMPARE(textOf(model, row), refusedAlt.at(row));
        QCOMPARE(blockOf(model, row).value(QStringLiteral("imagePath")).toString(),
                 QString());
    }

    QCOMPARE(kindOf(model, 6), QStringLiteral("image"));
    QCOMPARE(textOf(model, 6), QStringLiteral("ok"));
    QCOMPARE(blockOf(model, 6).value(QStringLiteral("imagePath")).toString(),
             QStringLiteral("assets/logo.png"));

    // A percent-encoded LEGITIMATE path is decoded, because that is the path the
    // RPC will actually be given.
    MarkdownModel spaced;
    spaced.setMarkdown(QStringLiteral("![s](assets/my%20logo.png)"));
    QCOMPARE(blockOf(spaced, 0).value(QStringLiteral("imagePath")).toString(),
             QStringLiteral("assets/my logo.png"));
}

void TstMarkdownModel::anUnterminatedFenceStaysCodeToTheEndOfTheDocument()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral("```js\n"
                                     "const x = \"<script>\";\n"
                                     "# not a heading\n"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(kindOf(model, 0), QStringLiteral("code"));
    QCOMPARE(blockOf(model, 0).value(QStringLiteral("language")).toString(),
             QStringLiteral("js"));
    // Everything to the end stays code. The alternative — falling back to prose
    // — would re-interpret a truncated document's code as markup, which is the
    // wrong direction for a viewer whose reads are capped at 8 MiB.
    QCOMPARE(textOf(model, 0),
             QStringLiteral("const x = \"<script>\";\n# not a heading"));
    QCOMPARE(spansOf(model, 0).size(), 0);
}

void TstMarkdownModel::fencedAndIndentedCodeCarryTheRightLanguage()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral("```C++ 17\n"
                                     "int main() { return 0; }\n"
                                     "```\n"
                                     "\n"
                                     "~~~\n"
                                     "no language here\n"
                                     "~~~\n"
                                     "\n"
                                     "    indented = true\n"
                                     "    still_code = true\n"));
    QCOMPARE(model.rowCount(), 3);

    // First word of the info string, lowercased. A LABEL, nothing more.
    QCOMPARE(blockOf(model, 0).value(QStringLiteral("language")).toString(),
             QStringLiteral("c++"));
    QCOMPARE(textOf(model, 0), QStringLiteral("int main() { return 0; }"));

    QCOMPARE(kindOf(model, 1), QStringLiteral("code"));
    QCOMPARE(blockOf(model, 1).value(QStringLiteral("language")).toString(),
             QString());
    QCOMPARE(textOf(model, 1), QStringLiteral("no language here"));

    // Indented code has no info string, so no language is reported and none is
    // guessed from the document's own extension.
    QCOMPARE(kindOf(model, 2), QStringLiteral("code"));
    QCOMPARE(blockOf(model, 2).value(QStringLiteral("language")).toString(),
             QString());
    QCOMPARE(textOf(model, 2),
             QStringLiteral("indented = true\nstill_code = true"));

    // Inline code is a SPAN, not markup, and its contents are literal — which is
    // also why a tag inside one is harmless.
    MarkdownModel inlineCode;
    inlineCode.setMarkdown(QStringLiteral("use `<b>` here"));
    QCOMPARE(textOf(inlineCode, 0), QStringLiteral("use <b> here"));
    const QVariantMap span = spansOf(inlineCode, 0).first().toMap();
    QCOMPARE(span.value(QStringLiteral("code")).toBool(), true);
    QCOMPARE(span.value(QStringLiteral("start")).toInt(), 4);
    QCOMPARE(span.value(QStringLiteral("length")).toInt(), 3);
}

void TstMarkdownModel::deeplyNestedListsCarryTheirNestingLevel()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral("- a\n"
                                     "  - b\n"
                                     "    - c\n"
                                     "      - d\n"
                                     "        - e\n"
                                     "- back to top\n"));
    QCOMPARE(model.rowCount(), 6);
    const QList<int> expectedLevels{0, 1, 2, 3, 4, 0};
    for (int row = 0; row < expectedLevels.size(); ++row) {
        QCOMPARE(kindOf(model, row), QStringLiteral("listItem"));
        QCOMPARE(blockOf(model, row).value(QStringLiteral("level")).toInt(),
                 expectedLevels.at(row));
        QCOMPARE(blockOf(model, row).value(QStringLiteral("ordered")).toBool(),
                 false);
    }
    QCOMPARE(textOf(model, 4), QStringLiteral("e"));
    QCOMPARE(textOf(model, 5), QStringLiteral("back to top"));

    // Ordered markers in both spellings, and a marker that is NOT one.
    MarkdownModel ordered;
    ordered.setMarkdown(QStringLiteral("1. one\n"
                                       "2) two\n"
                                       "\n"
                                       "1.5 is not a list item\n"));
    QCOMPARE(ordered.rowCount(), 3);
    QCOMPARE(blockOf(ordered, 0).value(QStringLiteral("ordered")).toBool(), true);
    QCOMPARE(blockOf(ordered, 1).value(QStringLiteral("ordered")).toBool(), true);
    QCOMPARE(kindOf(ordered, 2), QStringLiteral("paragraph"));
    QCOMPARE(textOf(ordered, 2), QStringLiteral("1.5 is not a list item"));
}

void TstMarkdownModel::taskMarkersHaveThreeStatesNotTwo()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral("- [ ] todo\n"
                                     "- [x] done\n"
                                     "- [X] also done\n"
                                     "- plain item\n"
                                     "- [z] not a marker\n"));
    QCOMPARE(model.rowCount(), 5);

    QCOMPARE(blockOf(model, 0).value(QStringLiteral("checked")), QVariant(false));
    QCOMPARE(textOf(model, 0), QStringLiteral("todo"));
    QCOMPARE(blockOf(model, 1).value(QStringLiteral("checked")), QVariant(true));
    QCOMPARE(blockOf(model, 2).value(QStringLiteral("checked")), QVariant(true));

    // INVALID, not false: "no checkbox" and "unchecked checkbox" have to be
    // distinguishable, or every bullet renders as an empty box.
    QCOMPARE(blockOf(model, 3).value(QStringLiteral("checked")), QVariant());
    QCOMPARE(blockOf(model, 4).value(QStringLiteral("checked")), QVariant());
    QCOMPARE(textOf(model, 4), QStringLiteral("[z] not a marker"));

    // ...and the role goes out invalid through data() too, which is what makes it
    // `undefined` in QML.
    const QModelIndex plain = model.index(3, 0);
    QVERIFY(!model.data(plain, MarkdownModel::CheckedRole).isValid());
}

void TstMarkdownModel::headingsInBothSpellings()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral("# One\n"
                                     "###### Six\n"
                                     "####### Seven is not a heading\n"
                                     "## Closed ##\n"
                                     "## Hash#Kept\n"
                                     "#NoSpace\n"));
    QCOMPARE(model.rowCount(), 6);

    QCOMPARE(kindOf(model, 0), QStringLiteral("heading"));
    QCOMPARE(blockOf(model, 0).value(QStringLiteral("level")).toInt(), 1);
    QCOMPARE(textOf(model, 0), QStringLiteral("One"));

    QCOMPARE(blockOf(model, 1).value(QStringLiteral("level")).toInt(), 6);
    QCOMPARE(textOf(model, 1), QStringLiteral("Six"));

    // Seven hashes is not a heading in CommonMark, and it must not become one
    // here either — it is prose.
    QCOMPARE(kindOf(model, 2), QStringLiteral("paragraph"));
    QCOMPARE(textOf(model, 2), QStringLiteral("####### Seven is not a heading"));

    // A closing run is decoration only when it is separated from the text.
    QCOMPARE(textOf(model, 3), QStringLiteral("Closed"));
    QCOMPARE(textOf(model, 4), QStringLiteral("Hash#Kept"));

    QCOMPARE(kindOf(model, 5), QStringLiteral("paragraph"));
    QCOMPARE(textOf(model, 5), QStringLiteral("#NoSpace"));
}

void TstMarkdownModel::aSetextUnderlineBeatsAThematicBreak()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral("Title\n"
                                     "=====\n"
                                     "\n"
                                     "Sub\n"
                                     "---\n"
                                     "\n"
                                     "---\n"));
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(kindOf(model, 0), QStringLiteral("heading"));
    QCOMPARE(blockOf(model, 0).value(QStringLiteral("level")).toInt(), 1);
    QCOMPARE(textOf(model, 0), QStringLiteral("Title"));

    // "Sub\n---" is an H2, not a paragraph followed by a rule.
    QCOMPARE(kindOf(model, 1), QStringLiteral("heading"));
    QCOMPARE(blockOf(model, 1).value(QStringLiteral("level")).toInt(), 2);
    QCOMPARE(textOf(model, 1), QStringLiteral("Sub"));

    // With no paragraph open, the same three dashes ARE a rule.
    QCOMPARE(kindOf(model, 2), QStringLiteral("rule"));
    QCOMPARE(textOf(model, 2), QString());
}

void TstMarkdownModel::quotesAndRules()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral("> quoted\n"
                                     "> more quoted\n"
                                     "\n"
                                     ">> deeper\n"
                                     "\n"
                                     "***\n"
                                     "\n"
                                     "- - -\n"
                                     "\n"
                                     "___\n"));
    QCOMPARE(model.rowCount(), 5);

    QCOMPARE(kindOf(model, 0), QStringLiteral("quote"));
    QCOMPARE(blockOf(model, 0).value(QStringLiteral("level")).toInt(), 1);
    QCOMPARE(textOf(model, 0), QStringLiteral("quoted\nmore quoted"));

    QCOMPARE(kindOf(model, 1), QStringLiteral("quote"));
    QCOMPARE(blockOf(model, 1).value(QStringLiteral("level")).toInt(), 2);
    QCOMPARE(textOf(model, 1), QStringLiteral("deeper"));

    // All three thematic-break spellings, including the spaced one that must NOT
    // be read as a list item.
    for (int row = 2; row < 5; ++row)
        QCOMPARE(kindOf(model, row), QStringLiteral("rule"));
}

void TstMarkdownModel::emphasisBecomesSpansAndNotMarkup()
{
    MarkdownModel model;
    model.setMarkdown(
        QStringLiteral("**bold** and _it_ and `code` and ***both***"));
    QCOMPARE(model.rowCount(), 1);

    // The delimiters are GONE from the text. Nothing downstream has to strip
    // them, and nothing downstream is asked to interpret them.
    QCOMPARE(textOf(model, 0), QStringLiteral("bold and it and code and both"));

    const QVariantList spans = spansOf(model, 0);
    QCOMPARE(spans.size(), 4);

    const auto at = [&spans](int i) { return spans.at(i).toMap(); };
    QCOMPARE(at(0).value(QStringLiteral("start")).toInt(), 0);
    QCOMPARE(at(0).value(QStringLiteral("length")).toInt(), 4);
    QCOMPARE(at(0).value(QStringLiteral("strong")).toBool(), true);
    QCOMPARE(at(0).value(QStringLiteral("emphasis")).toBool(), false);

    QCOMPARE(at(1).value(QStringLiteral("start")).toInt(), 9);
    QCOMPARE(at(1).value(QStringLiteral("length")).toInt(), 2);
    QCOMPARE(at(1).value(QStringLiteral("emphasis")).toBool(), true);
    QCOMPARE(at(1).value(QStringLiteral("strong")).toBool(), false);

    QCOMPARE(at(2).value(QStringLiteral("start")).toInt(), 16);
    QCOMPARE(at(2).value(QStringLiteral("length")).toInt(), 4);
    QCOMPARE(at(2).value(QStringLiteral("code")).toBool(), true);

    // A triple run is both, over one flattened run — not a strong span with a
    // stray asterisk left inside the text.
    QCOMPARE(at(3).value(QStringLiteral("start")).toInt(), 25);
    QCOMPARE(at(3).value(QStringLiteral("length")).toInt(), 4);
    QCOMPARE(at(3).value(QStringLiteral("strong")).toBool(), true);
    QCOMPARE(at(3).value(QStringLiteral("emphasis")).toBool(), true);

    // Spans are ascending and non-overlapping, which is what lets a page slice
    // `text` by them directly.
    int previousEnd = 0;
    for (const QVariant &value : spans) {
        const QVariantMap span = value.toMap();
        const int start = span.value(QStringLiteral("start")).toInt();
        QVERIFY(start >= previousEnd);
        previousEnd = start + span.value(QStringLiteral("length")).toInt();
    }
    QVERIFY(previousEnd <= textOf(model, 0).size());

    // An UNMATCHED delimiter is a literal character, never a dangling style.
    MarkdownModel unmatched;
    unmatched.setMarkdown(QStringLiteral("2 * 3 * 4 and a stray ` tick"));
    QCOMPARE(textOf(unmatched, 0),
             QStringLiteral("2 * 3 * 4 and a stray ` tick"));
    QCOMPARE(spansOf(unmatched, 0).size(), 0);

    // An escaped delimiter yields the character and no span.
    MarkdownModel escaped;
    escaped.setMarkdown(QStringLiteral("\\*not emphasis\\*"));
    QCOMPARE(textOf(escaped, 0), QStringLiteral("*not emphasis*"));
    QCOMPARE(spansOf(escaped, 0).size(), 0);
}

void TstMarkdownModel::inlineImagesInProseAreReducedToTheirAltText()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral(
        "See ![the badge](../../../etc/passwd) in the corner."));
    QCOMPARE(model.rowCount(), 1);
    // A paragraph, not an image block: only a STANDALONE image becomes one.
    QCOMPARE(kindOf(model, 0), QStringLiteral("paragraph"));
    QCOMPARE(textOf(model, 0), QStringLiteral("See the badge in the corner."));
    // And no imagePath is carried, so this destination is never read at all.
    QCOMPARE(blockOf(model, 0).value(QStringLiteral("imagePath")).toString(),
             QString());
}

void TstMarkdownModel::everyRoleIsPublishedUnderItsContractedName()
{
    MarkdownModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    QCOMPARE(roles.value(MarkdownModel::BlockKindRole), QByteArrayLiteral("blockKind"));
    QCOMPARE(roles.value(MarkdownModel::TextRole), QByteArrayLiteral("text"));
    QCOMPARE(roles.value(MarkdownModel::LevelRole), QByteArrayLiteral("level"));
    QCOMPARE(roles.value(MarkdownModel::LanguageRole), QByteArrayLiteral("language"));
    QCOMPARE(roles.value(MarkdownModel::ImagePathRole), QByteArrayLiteral("imagePath"));
    QCOMPARE(roles.value(MarkdownModel::OrderedRole), QByteArrayLiteral("ordered"));
    QCOMPARE(roles.value(MarkdownModel::CheckedRole), QByteArrayLiteral("checked"));
    QCOMPARE(roles.value(MarkdownModel::SpansRole), QByteArrayLiteral("spans"));
    QCOMPARE(roles.size(), 8);

    QSignalSpy counts(&model, &MarkdownModel::blockCountChanged);
    model.setMarkdown(QStringLiteral("# Title\n\ntext\n"));
    QCOMPARE(model.blockCount(), 2);
    QCOMPARE(counts.size(), 1);
    QCOMPARE(model.data(model.index(0, 0), MarkdownModel::BlockKindRole).toString(),
             QStringLiteral("heading"));
    // Out-of-range and nested indices answer nothing rather than crashing.
    QVERIFY(!model.data(model.index(9, 0), MarkdownModel::TextRole).isValid());
    QCOMPARE(model.blockAt(9), QVariantMap());

    model.clear();
    QCOMPARE(model.blockCount(), 0);
    QCOMPARE(counts.size(), 2);
    // An empty document is an empty model, not a model with one empty paragraph.
    model.setMarkdown(QString());
    QCOMPARE(model.rowCount(), 0);
}

// The two validators, exercised directly: they are the security boundary, and a
// caller reading them out of a rendered block cannot tell WHY something was
// refused.
void TstMarkdownModel::destinationValidatorsInIsolation()
{
    // Links: http(s) and plain relative references only.
    QCOMPARE(MarkdownModel::safeLinkTarget(QStringLiteral("https://example.com")),
             QStringLiteral("https://example.com"));
    QCOMPARE(MarkdownModel::safeLinkTarget(QStringLiteral("HTTP://Example.com")),
             QStringLiteral("HTTP://Example.com"));
    QCOMPARE(MarkdownModel::safeLinkTarget(QStringLiteral("docs/a.md")),
             QStringLiteral("docs/a.md"));
    for (const QString &bad :
         {QStringLiteral("javascript:alert(1)"), QStringLiteral("JavaScript:x"),
          QStringLiteral("data:text/html,x"), QStringLiteral("vbscript:x"),
          QStringLiteral("file:///etc/passwd"),
          QStringLiteral("codeharbor-internal://file/abc"),
          QStringLiteral("//evil.example/x"), QStringLiteral("http://a b"),
          QStringLiteral("http://a\nb"), QString()}) {
        QVERIFY2(MarkdownModel::safeLinkTarget(bad).isEmpty(),
                 qPrintable(QStringLiteral("accepted %1").arg(bad)));
    }

    // Images: no scheme AT ALL — http included, because a document must not be
    // able to make this client touch the network.
    QString decoded;
    QVERIFY(MarkdownModel::isSafeRelativePath(QStringLiteral("a/b.png"), &decoded));
    QCOMPARE(decoded, QStringLiteral("a/b.png"));
    QVERIFY(MarkdownModel::isSafeRelativePath(QStringLiteral("./b.png")));
    for (const QString &bad :
         {QStringLiteral("https://example.com/x.png"),
          QStringLiteral("http://example.com/x.png"),
          QStringLiteral("data:image/png;base64,AAAA"),
          QStringLiteral("/etc/passwd"), QStringLiteral("//host/x.png"),
          QStringLiteral("../x.png"), QStringLiteral("a/../../x.png"),
          QStringLiteral("..%2Fx.png"), QStringLiteral("#anchor"), QString(),
          // Windows separators. The daemon may be running on a host where
          // "..\" traverses, and the client must not be the component that
          // assumed otherwise.
          QStringLiteral("..\\..\\windows\\win.ini"),
          QStringLiteral("a\\..\\..\\x.png"),
          QStringLiteral("\\\\host\\share\\x.png"),
          QStringLiteral("\\etc\\passwd"),
          QStringLiteral("..%5Cx.png")}) {
        QVERIFY2(!MarkdownModel::isSafeRelativePath(bad),
                 qPrintable(QStringLiteral("accepted %1").arg(bad)));
    }
}

// CommonMark forbids intraword '_' emphasis and allows intraword '*'. Here that
// is not pedantry: a repository's prose is full of identifiers, and the parser
// used to DELETE the underscores out of them — "snake_case_name" reached the
// screen as "snakecasename" with its middle italicised.
void TstMarkdownModel::intrawordUnderscoresAreNotEmphasis()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral("call snake_case_name and other_var_here"));
    QCOMPARE(textOf(model, 0),
             QStringLiteral("call snake_case_name and other_var_here"));
    QCOMPARE(spansOf(model, 0).size(), 0);

    // A '_' at a word BOUNDARY is still emphasis, and a doubled one is still
    // strong, so Python's dunder names read correctly either way.
    MarkdownModel boundaries;
    boundaries.setMarkdown(QStringLiteral("an _emphasis_ and __init__ here"));
    QCOMPARE(textOf(boundaries, 0),
             QStringLiteral("an emphasis and init here"));
    const QVariantList spans = spansOf(boundaries, 0);
    QCOMPARE(spans.size(), 2);
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("emphasis")).toBool(),
             true);
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("strong")).toBool(),
             false);
    QCOMPARE(spans.at(1).toMap().value(QStringLiteral("strong")).toBool(), true);

    // '*' keeps its intraword spelling: "a*b*c" is emphasis in CommonMark, and
    // changing that would have been a different bug.
    MarkdownModel stars;
    stars.setMarkdown(QStringLiteral("a*b*c and a__b__c"));
    QCOMPARE(textOf(stars, 0), QStringLiteral("abc and a__b__c"));
    QCOMPARE(spansOf(stars, 0).size(), 1);
}

// "[a](<https://x)" is not a link in CommonMark: the angle destination never
// closes. Falling through to the plain whitespace scan handed the page
// "<https://x" — a target that names nothing, that safeLinkTarget() read as a
// harmless relative reference, and that the delegate then drew as tappable.
void TstMarkdownModel::anUnterminatedAngleDestinationIsNotADestination()
{
    MarkdownModel model;
    model.setMarkdown(QStringLiteral("[a](<https://example.com/x)"));
    QCOMPARE(kindOf(model, 0), QStringLiteral("paragraph"));
    QCOMPARE(textOf(model, 0), QStringLiteral("a"));
    QCOMPARE(spansOf(model, 0).size(), 0);

    // An EMPTY angle destination is refused the same way, and the terminated
    // spelling — the whole point of the angle form — still works.
    MarkdownModel empty;
    empty.setMarkdown(QStringLiteral("[a](<>)"));
    QCOMPARE(spansOf(empty, 0).size(), 0);

    MarkdownModel spaced;
    spaced.setMarkdown(QStringLiteral("![a](<my logo.png>)"));
    QCOMPARE(kindOf(spaced, 0), QStringLiteral("image"));
    QCOMPARE(blockOf(spaced, 0).value(QStringLiteral("imagePath")).toString(),
             QStringLiteral("my logo.png"));
}

// setMarkdown() runs on the QML thread over a string the SERVER chose, so a
// document that makes the inline scanner quadratic is not slow rendering — it is
// the whole client frozen until the platform kills it. Each fixture below took
// minutes (or never finished) before the scanner was given a work allowance and
// a recursion depth; the bound here is deliberately loose, because what is being
// pinned is "terminates quickly", not a particular millisecond count.
void TstMarkdownModel::adversarialInlineInputCannotHangOrOverflowTheParser()
{
    struct Fixture {
        const char *what;
        QString source;
    };
    const QList<Fixture> fixtures{
        // 200 000 unmatched emphasis delimiters: one full-length close scan per
        // candidate opener.
        {"unmatched emphasis run",
         QString(200000, QLatin1Char('*')) + QStringLiteral("x")
             + QString(200000, QLatin1Char('*'))},
        // 500 000 unmatched '[': one full-length link-text scan each.
        {"unmatched brackets",
         QString(500000, QLatin1Char('[')) + QStringLiteral("x")},
        // Unmatched '<': the autolink probe used to be a full indexOf().
        {"unmatched angle brackets",
         QString(300000, QLatin1Char('<')) + QStringLiteral("x")},
        // Unmatched code fences inline.
        {"unmatched backticks",
         QString(200000, QLatin1Char('`')) + QStringLiteral("x")},
        // 50 000 MATCHED pairs: one recursion level each, which is a stack
        // overflow rather than a slow parse.
        {"deeply nested matched emphasis",
         QString(50000, QLatin1Char('*')) + QStringLiteral("x")
             + QString(50000, QLatin1Char('*'))},
        // A single very long line, which the old flatten() sized a 28-byte
        // per-character table against.
        {"one very long line",
         QString(2 * 1024 * 1024, QLatin1Char('a')) + QStringLiteral(" *e*")},
    };

    for (const Fixture &fixture : fixtures) {
        QElapsedTimer timer;
        timer.start();
        MarkdownModel model;
        model.setMarkdown(fixture.source);
        const qint64 elapsed = timer.elapsed();
        QVERIFY2(elapsed < 10000,
                 qPrintable(QStringLiteral("%1 took %2 ms")
                                .arg(QLatin1String(fixture.what))
                                .arg(elapsed)));
        // And it still produced a document rather than giving up: the
        // degradation is that delimiters stop being recognised and become
        // literal characters, which is what every unmatched delimiter already
        // does.
        QVERIFY(model.rowCount() >= 1);
        for (int row = 0; row < model.rowCount(); ++row) {
            const QVariantList spans = spansOf(model, row);
            const int length = textOf(model, row).size();
            int previousEnd = 0;
            for (const QVariant &value : spans) {
                const QVariantMap span = value.toMap();
                const int start = span.value(QStringLiteral("start")).toInt();
                const int span_length =
                    span.value(QStringLiteral("length")).toInt();
                QVERIFY(start >= previousEnd);
                QVERIFY(span_length > 0);
                QVERIFY(start + span_length <= length);
                previousEnd = start + span_length;
            }
        }
    }
}

QTEST_GUILESS_MAIN(TstMarkdownModel)
#include "tst_markdownmodel.moc"
