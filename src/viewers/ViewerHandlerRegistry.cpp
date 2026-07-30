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
    // editor / source viewer. The list is deliberately generous: anything a
    // developer would expect to read as text must NOT fall through to the
    // Download resolution, which hides the contents behind a binary pane.
    static const QSet<QString> kText = {
        QStringLiteral("txt"),        QStringLiteral("ts"),
        QStringLiteral("tsx"),        QStringLiteral("mts"),
        QStringLiteral("cts"),        QStringLiteral("js"),
        QStringLiteral("mjs"),        QStringLiteral("cjs"),
        QStringLiteral("jsx"),        QStringLiteral("c"),
        QStringLiteral("cc"),         QStringLiteral("cxx"),
        QStringLiteral("cpp"),        QStringLiteral("h"),
        QStringLiteral("hh"),         QStringLiteral("hxx"),
        QStringLiteral("hpp"),        QStringLiteral("py"),
        QStringLiteral("rs"),         QStringLiteral("go"),
        QStringLiteral("java"),       QStringLiteral("kt"),
        QStringLiteral("kts"),        QStringLiteral("rb"),
        QStringLiteral("php"),        QStringLiteral("swift"),
        QStringLiteral("scala"),      QStringLiteral("cs"),
        QStringLiteral("lua"),        QStringLiteral("pl"),
        QStringLiteral("sh"),         QStringLiteral("bash"),
        QStringLiteral("zsh"),        QStringLiteral("fish"),
        QStringLiteral("css"),        QStringLiteral("scss"),
        QStringLiteral("sass"),       QStringLiteral("less"),
        QStringLiteral("html"),       QStringLiteral("htm"),
        QStringLiteral("vue"),        QStringLiteral("svelte"),
        QStringLiteral("json"),       QStringLiteral("jsonc"),
        QStringLiteral("yaml"),       QStringLiteral("yml"),
        QStringLiteral("toml"),       QStringLiteral("ini"),
        QStringLiteral("cfg"),        QStringLiteral("conf"),
        QStringLiteral("properties"), QStringLiteral("env"),
        QStringLiteral("xml"),        QStringLiteral("sql"),
        QStringLiteral("csv"),        QStringLiteral("tsv"),
        QStringLiteral("log"),        QStringLiteral("diff"),
        QStringLiteral("patch"),      QStringLiteral("cmake"),
        QStringLiteral("gradle"),     QStringLiteral("mk"),
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
        const QString ext = extensionOf(path);
        // Extension-based dispatch cannot classify extensionless files, and
        // extensionOf() reports dotfiles as extensionless too. Match a table of
        // well-known extensionless names and dotfiles (case-sensitive as
        // written) by basename so they open as text instead of falling through
        // to the binary Download pane.
        if (ext.isEmpty()) {
            static const QSet<QString> kTextNames = {
                QStringLiteral("Makefile"),      QStringLiteral("Dockerfile"),
                QStringLiteral("LICENSE"),       QStringLiteral("COPYING"),
                QStringLiteral("README"),        QStringLiteral("CHANGELOG"),
                QStringLiteral("AUTHORS"),       QStringLiteral("NOTICE"),
                QStringLiteral(".bashrc"),       QStringLiteral(".zshrc"),
                QStringLiteral(".profile"),      QStringLiteral(".gitignore"),
                QStringLiteral(".gitattributes"),QStringLiteral(".editorconfig"),
                QStringLiteral(".env"),
            };
            const int slash = path.lastIndexOf(QLatin1Char('/'));
            const QString name = slash >= 0 ? path.mid(slash + 1) : path;
            if (kTextNames.contains(name))
                return ViewerResolution::TextEditor;
        }
        return resolveByExtension(ext);
    }

    return ViewerResolution::Error;
}

} // namespace ch
