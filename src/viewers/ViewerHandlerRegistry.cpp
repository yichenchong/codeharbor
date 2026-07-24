#include "ViewerHandlerRegistry.h"

#include <QSet>

namespace ch {

namespace {

// Extension of the final path segment, lower-cased, without the leading dot.
// Returns empty for extensionless names and dotfiles (a leading dot is not an
// extension, so ".bashrc" has none).
QString extensionOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const QString name = slash >= 0 ? path.mid(slash + 1) : path;
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return QString();
    return name.mid(dot + 1).toLower();
}

} // namespace

ViewerResolution ViewerHandlerRegistry::resolveByExtension(const QString &ext)
{
    const QString e = ext.toLower();

    // Markdown renders through the internal HTML renderer (SPEC 7.5).
    if (e == QLatin1String("md") || e == QLatin1String("markdown"))
        return ViewerResolution::InternalHtmlRenderer;

    // Source code, plain text, and structured-data formats open in the text
    // editor / source viewer.
    static const QSet<QString> kText = {
        QStringLiteral("txt"),  QStringLiteral("ts"),   QStringLiteral("tsx"),
        QStringLiteral("js"),   QStringLiteral("mjs"),  QStringLiteral("c"),
        QStringLiteral("cc"),   QStringLiteral("cpp"),  QStringLiteral("h"),
        QStringLiteral("hpp"),  QStringLiteral("py"),   QStringLiteral("rs"),
        QStringLiteral("go"),   QStringLiteral("sh"),   QStringLiteral("css"),
        QStringLiteral("html"), QStringLiteral("json"), QStringLiteral("yaml"),
        QStringLiteral("yml"),  QStringLiteral("toml"),
    };
    if (kText.contains(e))
        return ViewerResolution::TextEditor;

    static const QSet<QString> kImage = {
        QStringLiteral("png"), QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("svg"),  QStringLiteral("webp"),
    };
    if (kImage.contains(e))
        return ViewerResolution::ImageViewer;

    if (e == QLatin1String("pdf"))
        return ViewerResolution::PdfViewer;

    // Unknown or known-binary content is offered for download.
    return ViewerResolution::Download;
}

ViewerResolution ViewerHandlerRegistry::resolve(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();

    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        return ViewerResolution::DirectWebNavigation;

    if (scheme == QLatin1String("codeharbor-internal")) {
        // Internal URLs are opaque (codeharbor-internal://file/<id>), but honor
        // an explicit extension when one is present; otherwise render as HTML.
        const QString ext = extensionOf(url.path());
        if (!ext.isEmpty()) {
            const ViewerResolution r = resolveByExtension(ext);
            if (r != ViewerResolution::Download)
                return r;
        }
        return ViewerResolution::InternalHtmlRenderer;
    }

    if (scheme == QLatin1String("file")) {
        const QString path = url.path();
        // A remote directory is expressed as a file URL with a trailing slash.
        if (path.endsWith(QLatin1Char('/')))
            return ViewerResolution::DirectoryViewer;
        return resolveByExtension(extensionOf(path));
    }

    return ViewerResolution::Error;
}

ViewerResolution ViewerHandlerRegistry::resolveScheme(const QString &scheme)
{
    const QString s = scheme.toLower();
    if (s == QLatin1String("http") || s == QLatin1String("https"))
        return ViewerResolution::DirectWebNavigation;
    if (s == QLatin1String("codeharbor-internal"))
        return ViewerResolution::InternalHtmlRenderer;
    if (s == QLatin1String("file"))
        // Remote file:// is resolved by MIME/extension in a later pass.
        return ViewerResolution::TextEditor;
    return ViewerResolution::Error;
}

} // namespace ch
