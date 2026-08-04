#include "ViewerKinds.h"

#include <QSet>

namespace ch::ViewerKinds {
namespace {

const QStringList kAllKinds = {
    QStringLiteral("web"),       QStringLiteral("markdown"),
    QStringLiteral("text"),      QStringLiteral("image"),
    QStringLiteral("pdf"),       QStringLiteral("directory"),
    QStringLiteral("binary"),
};

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

bool isAsciiAlphaNumeric(const QChar ch)
{
    return (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
           || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
           || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'));
}

} // namespace

QStringList all()
{
    return kAllKinds;
}

bool isKnown(const QString& kind)
{
    return kAllKinds.contains(kind);
}

QString normaliseExtension(const QString& extension)
{
    QString result = extension.trimmed();
    if (result.startsWith(QLatin1Char('.')))
        result.remove(0, 1);
    if (result.isEmpty() || result.size() > 64)
        return {};
    for (const QChar ch : result) {
        if (!isAsciiAlphaNumeric(ch))
            return {};
    }
    return result.toLower();
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
