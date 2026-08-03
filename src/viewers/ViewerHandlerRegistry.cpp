#include "ViewerHandlerRegistry.h"

#include <QSet>

namespace ch {

namespace {

// Final segment of a path ("/a/b/c.txt" -> "c.txt", "c.txt" -> "c.txt").
QString baseNameOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? path.mid(slash + 1) : path;
}

// Extension of the final path segment, lower-cased, without the leading dot.
// Returns empty for extensionless names and dotfiles (a leading dot is not an
// extension, so ".bashrc" has none).
QString extensionOf(const QString &path)
{
    const QString name = baseNameOf(path);
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return QString();
    return name.mid(dot + 1).toLower();
}

// Whether a file NAME (not its extension) is one of the well-known text files
// extension-based dispatch cannot see: extensionless ones such as "Makefile"
// and "LICENSE", and dotfiles such as ".gitignore", whose leading dot is not an
// extension.
//
// Matched case-INsensitively. "makefile", "Makefile" and "MAKEFILE" are all
// ordinary spellings on a Unix server (GNU make itself accepts every one), and
// a case-sensitive table sent the lower-cased ones to the binary download pane.
bool isWellKnownTextName(const QString &name)
{
    // Spelled lower case, because the lookup lower-cases what it is given.
    static const QSet<QString> kTextNames = {
        QStringLiteral("makefile"),        QStringLiteral("gnumakefile"),
        QStringLiteral("dockerfile"),      QStringLiteral("containerfile"),
        QStringLiteral("jenkinsfile"),     QStringLiteral("vagrantfile"),
        QStringLiteral("rakefile"),        QStringLiteral("gemfile"),
        QStringLiteral("procfile"),        QStringLiteral("brewfile"),
        QStringLiteral("justfile"),        QStringLiteral("license"),
        QStringLiteral("licence"),         QStringLiteral("copying"),
        QStringLiteral("readme"),          QStringLiteral("changelog"),
        QStringLiteral("changes"),         QStringLiteral("authors"),
        QStringLiteral("contributors"),    QStringLiteral("notice"),
        QStringLiteral("install"),         QStringLiteral("news"),
        QStringLiteral("todo"),            QStringLiteral("version"),
        QStringLiteral("codeowners"),      QStringLiteral(".bashrc"),
        QStringLiteral(".bash_profile"),   QStringLiteral(".bash_aliases"),
        QStringLiteral(".zshrc"),          QStringLiteral(".zprofile"),
        QStringLiteral(".zshenv"),         QStringLiteral(".profile"),
        QStringLiteral(".gitignore"),      QStringLiteral(".gitattributes"),
        QStringLiteral(".gitmodules"),     QStringLiteral(".gitconfig"),
        QStringLiteral(".editorconfig"),   QStringLiteral(".env"),
        QStringLiteral(".dockerignore"),   QStringLiteral(".npmrc"),
        QStringLiteral(".npmignore"),      QStringLiteral(".nvmrc"),
        QStringLiteral(".yarnrc"),         QStringLiteral(".eslintrc"),
        QStringLiteral(".eslintignore"),   QStringLiteral(".prettierrc"),
        QStringLiteral(".prettierignore"), QStringLiteral(".babelrc"),
        QStringLiteral(".stylelintrc"),    QStringLiteral(".clang-format"),
        QStringLiteral(".clang-tidy"),     QStringLiteral(".vimrc"),
        QStringLiteral(".inputrc"),        QStringLiteral(".gdbinit"),
        QStringLiteral(".hgignore"),       QStringLiteral(".flake8"),
        QStringLiteral(".pylintrc"),       QStringLiteral(".tool-versions"),
    };
    const QString lower = name.toLower();
    if (kTextNames.contains(lower))
        return true;
    // A well-known name with a suffix bolted on is still that file:
    // "Dockerfile.dev", "Makefile.am", "README.old", ".env.local". The search
    // starts at index 1 so a dotfile's leading dot stays part of the stem. Only
    // reached once the suffix itself has already been found meaningless, so
    // "archive.zip" and "photo.jpg" never get here.
    const qsizetype dot = lower.indexOf(QLatin1Char('.'), 1);
    return dot > 0 && kTextNames.contains(lower.left(dot));
}

} // namespace

ViewerResolution ViewerHandlerRegistry::resolveByExtension(const QString &ext)
{
    const QString e = ext.toLower();

    // Markdown uses the dedicated sanitized internal renderer (SPEC 7.5).
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
        QStringLiteral("mdx"),        QStringLiteral("rst"),
        QStringLiteral("adoc"),       QStringLiteral("tex"),
        QStringLiteral("bib"),        QStringLiteral("r"),
        QStringLiteral("jl"),         QStringLiteral("dart"),
        QStringLiteral("ex"),         QStringLiteral("exs"),
        QStringLiteral("erl"),        QStringLiteral("hs"),
        QStringLiteral("clj"),        QStringLiteral("groovy"),
        QStringLiteral("bat"),        QStringLiteral("cmd"),
        QStringLiteral("ps1"),        QStringLiteral("psm1"),
        QStringLiteral("vim"),        QStringLiteral("el"),
        QStringLiteral("nim"),        QStringLiteral("zig"),
        QStringLiteral("scm"),        QStringLiteral("lisp"),
        QStringLiteral("ml"),         QStringLiteral("elm"),
        QStringLiteral("tcl"),        QStringLiteral("awk"),
        QStringLiteral("proto"),      QStringLiteral("graphql"),
        QStringLiteral("gql"),        QStringLiteral("tf"),
        QStringLiteral("tfvars"),     QStringLiteral("hcl"),
        QStringLiteral("nix"),        QStringLiteral("bzl"),
        QStringLiteral("bazel"),      QStringLiteral("ninja"),
        QStringLiteral("m4"),         QStringLiteral("ac"),
        QStringLiteral("am"),         QStringLiteral("spec"),
        QStringLiteral("s"),          QStringLiteral("asm"),
        QStringLiteral("f90"),        QStringLiteral("pas"),
        QStringLiteral("vb"),         QStringLiteral("mod"),
        QStringLiteral("sum"),        QStringLiteral("lock"),
        QStringLiteral("service"),    QStringLiteral("desktop"),
        QStringLiteral("rules"),      QStringLiteral("ipynb"),
        QStringLiteral("qml"),        QStringLiteral("qrc"),
        QStringLiteral("pro"),        QStringLiteral("pri"),
        QStringLiteral("ui"),         QStringLiteral("srt"),
    };
    if (kText.contains(e))
        return ViewerResolution::TextEditor;

    // Raster formats Chromium renders inline. Anything it cannot decode (tiff,
    // heic, psd, ...) deliberately stays a download rather than an image pane
    // showing a permanent broken-image icon.
    static const QSet<QString> kImage = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("gif"),  QStringLiteral("svg"),  QStringLiteral("webp"),
        QStringLiteral("bmp"),  QStringLiteral("ico"),  QStringLiteral("avif"),
        QStringLiteral("apng"),
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
        const ViewerResolution byExtension = resolveByExtension(extensionOf(path));
        if (byExtension != ViewerResolution::Download)
            return byExtension;
        // The extension said nothing (there was none, or it is not one we
        // know). Before giving up and hiding the file behind the binary
        // download pane, check the NAME: extensionless files such as "Makefile"
        // and dotfiles such as ".gitignore" are plain text, and so are the same
        // names carrying a meaningless suffix ("Dockerfile.dev", ".env.local").
        if (isWellKnownTextName(baseNameOf(path)))
            return ViewerResolution::TextEditor;
        return byExtension;
    }

    return ViewerResolution::Error;
}
QStringList ViewerHandlerRegistry::applicableViewKinds(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        return {QStringLiteral("web")};

    if (scheme != QLatin1String("file"))
        return {};

    const QString path = url.path();
    if (path.endsWith(QLatin1Char('/')))
        return {QStringLiteral("directory")};

    const ViewerResolution resolution = resolve(url);
    switch (resolution) {
    case ViewerResolution::InternalHtmlRenderer:
    case ViewerResolution::TextEditor: {
        const QString ext = extensionOf(path);
        // Markdown has TWO real handlers and the user picks between them: the
        // rendered document (the default) and the source in the editor. Listing
        // the renderer first is what makes it the default, because the menu
        // marks the first entry - see `defaultKind` in ViewerDirectoryView.qml.
        if (ext == QLatin1String("md") || ext == QLatin1String("markdown"))
            return {QStringLiteral("markdown"), QStringLiteral("editor"),
                    QStringLiteral("text")};
        QStringList kinds = {QStringLiteral("editor"), QStringLiteral("text")};
        // HTML is positively claimed by the editor, but it can also be shown
        // as a rendered document through the privileged internal profile.
        if (ext == QLatin1String("html") || ext == QLatin1String("htm"))
            kinds.append(QStringLiteral("web"));
        return kinds;
    }
    case ViewerResolution::ImageViewer:
        return {QStringLiteral("image")};
    case ViewerResolution::PdfViewer:
        return {QStringLiteral("pdf")};
    case ViewerResolution::Download:
        return {QStringLiteral("binary")};
    case ViewerResolution::DirectWebNavigation:
    case ViewerResolution::DirectoryViewer:
    case ViewerResolution::Error:
        return {};
    }
    return {};
}

bool ViewerHandlerRegistry::isValidApplicationScheme(const QString &scheme)
{
    if (scheme.isEmpty() || !((scheme.front() >= QLatin1Char('a')
                               && scheme.front() <= QLatin1Char('z'))
                              || (scheme.front() >= QLatin1Char('A')
                                  && scheme.front() <= QLatin1Char('Z'))))
        return false;

    for (qsizetype i = 1; i < scheme.size(); ++i) {
        const QChar c = scheme.at(i);
        if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z'))
              || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
              || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
              || c == QLatin1Char('+') || c == QLatin1Char('-')
              || c == QLatin1Char('.')))
            return false;
    }

    const QString lower = scheme.toLower();
    return lower != QLatin1String("codeharbor-internal")
           && lower != QLatin1String("http")
           && lower != QLatin1String("https")
           && lower != QLatin1String("file");
}

QUrl ViewerHandlerRegistry::applicationUrl(const QString &scheme,
                                            const QString &remotePath)
{
    if (!isValidApplicationScheme(scheme) || remotePath.isEmpty())
        return {};

    // SECURITY: remotePath is server data and may contain spaces, '#', '?',
    // percent signs, or even a string resembling another URL. setPath() treats
    // it as ONE URL path and escapes delimiters; concatenating it after
    // "<scheme>://" would let those characters become a query, fragment, or
    // second scheme before the desktop handler receives the URL.
    QUrl url;
    url.setScheme(scheme);
    url.setPath(remotePath, QUrl::DecodedMode);
    return url;
}

} // namespace ch
