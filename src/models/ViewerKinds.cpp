#include "ViewerKinds.h"

#include <QSet>

namespace ch::ViewerKinds {
namespace {

// A function-local static, matching imageExtensions() below: a namespace-scope
// QStringList would be constructed by dynamic initialization before main(),
// with no ordering guarantee against other translation units that might read it
// from their own static initializers.
const QStringList& allKinds()
{
    static const QStringList kinds = {
        QStringLiteral("web"),       QStringLiteral("markdown"),
        QStringLiteral("text"),      QStringLiteral("image"),
        QStringLiteral("pdf"),       QStringLiteral("directory"),
        QStringLiteral("binary"),
    };
    return kinds;
}

bool isAsciiAlphaNumeric(QChar character)
{
    return (character >= QLatin1Char('a') && character <= QLatin1Char('z'))
           || (character >= QLatin1Char('A') && character <= QLatin1Char('Z'))
           || (character >= QLatin1Char('0') && character <= QLatin1Char('9'));
}

// Characters allowed after the first one in a canonical extension. See the
// header for why exactly these two punctuation marks and nothing else.
bool isExtensionBodyCharacter(QChar character)
{
    return isAsciiAlphaNumeric(character) || character == QLatin1Char('+')
           || character == QLatin1Char('-');
}

} // namespace

QStringList all()
{
    return allKinds();
}

bool isKnown(const QString& kind)
{
    return allKinds().contains(kind);
}

QString normaliseExtension(const QString& extension)
{
    QString result = extension.trimmed();
    if (result.startsWith(QLatin1Char('.')))
        result.remove(0, 1);
    if (result.isEmpty() || result.size() > 64)
        return {};
    // The FIRST character must be alphanumeric. That is what keeps a value made
    // only of punctuation ("+", "-x") out, and it is also what preserves the
    // single-leading-dot rule above: ".." strips to "." and "..md" to ".md",
    // both of which start with a character no extension may begin with.
    //
    // The loop variable is not named `ch`: that would shadow the enclosing ch::
    // namespace inside the loop body.
    if (!isAsciiAlphaNumeric(result.at(0)))
        return {};
    for (const QChar character : result) {
        if (!isExtensionBodyCharacter(character))
            return {};
    }
    return result.toLower();
}

const QSet<QString>& imageExtensions()
{
    static const QSet<QString> extensions = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("gif"),  QStringLiteral("svg"),  QStringLiteral("webp"),
        QStringLiteral("bmp"),  QStringLiteral("ico"),  QStringLiteral("avif"),
        QStringLiteral("apng"),
    };
    return extensions;
}

QStringList assignableForExtension(const QString& extension)
{
    const QString ext = normaliseExtension(extension);
    if (ext.isEmpty())
        return {};

    QStringList result;
    if (ext == QLatin1String("html") || ext == QLatin1String("htm"))
        result.append(QStringLiteral("web"));
    // Markdown is a FORMAT, not a generic way to look at text: offering it for
    // a .ts or .zig file would produce a pane that renders source as prose.
    // Any text file can still be opened as text, which is the honest option.
    if (ext == QLatin1String("md") || ext == QLatin1String("markdown"))
        result.append(QStringLiteral("markdown"));
    if (imageExtensions().contains(ext))
        result.append(QStringLiteral("image"));
    if (ext == QLatin1String("pdf"))
        result.append(QStringLiteral("pdf"));

    // Text and binary handlers are deliberately last: they can display any
    // extension, while the specialised entries above are only offered when
    // their actual handler claims that file type.
    result.append(QStringLiteral("text"));
    result.append(QStringLiteral("binary"));
    return result;
}

bool canAssign(const QString& extension, const QString& kind)
{
    return assignableForExtension(extension).contains(kind);
}

} // namespace ch::ViewerKinds
