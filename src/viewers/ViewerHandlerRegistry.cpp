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
    // starts at index 1 so a dotfile's leading dot stays part of the stem.
    //
    // The suffix must be MEANINGLESS for the stem to speak, and reaching this
    // function is not enough to establish that. resolveByExtension() answers
    // Download for two different reasons — "we do not recognise this
    // extension" and "we recognise it and it is binary" — and only the first
    // lets the stem decide. Without this veto "readme.zip", "news.tar.gz" and
    // "version.o" are poured into the text editor as if they were prose,
    // because their stems are in the table above. The set below exists ONLY to
    // silence the stem rule; it does not change any resolution on its own,
    // since resolveByExtension() already sends every one of these to Download.
    static const QSet<QString> kBinarySuffixes = {
        QStringLiteral("zip"),    QStringLiteral("gz"),
        QStringLiteral("bz2"),    QStringLiteral("xz"),
        QStringLiteral("zst"),    QStringLiteral("tar"),
        QStringLiteral("tgz"),    QStringLiteral("7z"),
        QStringLiteral("rar"),    QStringLiteral("jar"),
        QStringLiteral("war"),    QStringLiteral("whl"),
        QStringLiteral("deb"),    QStringLiteral("rpm"),
        QStringLiteral("dmg"),    QStringLiteral("iso"),
        QStringLiteral("img"),    QStringLiteral("exe"),
        QStringLiteral("dll"),    QStringLiteral("so"),
        QStringLiteral("dylib"),  QStringLiteral("a"),
        QStringLiteral("o"),      QStringLiteral("obj"),
        QStringLiteral("lib"),    QStringLiteral("bin"),
        QStringLiteral("class"),  QStringLiteral("pyc"),
        QStringLiteral("pyo"),    QStringLiteral("wasm"),
        QStringLiteral("node"),   QStringLiteral("db"),
        QStringLiteral("sqlite"), QStringLiteral("sqlite3"),
        QStringLiteral("doc"),    QStringLiteral("docx"),
        QStringLiteral("xls"),    QStringLiteral("xlsx"),
        QStringLiteral("ppt"),    QStringLiteral("pptx"),
        QStringLiteral("odt"),    QStringLiteral("ods"),
        QStringLiteral("rtf"),    QStringLiteral("epub"),
        QStringLiteral("mobi"),   QStringLiteral("mp3"),
        QStringLiteral("mp4"),    QStringLiteral("mkv"),
        QStringLiteral("mov"),    QStringLiteral("avi"),
        QStringLiteral("wav"),    QStringLiteral("flac"),
        QStringLiteral("ogg"),    QStringLiteral("webm"),
        QStringLiteral("tiff"),   QStringLiteral("tif"),
        QStringLiteral("heic"),   QStringLiteral("psd"),
        QStringLiteral("eps"),    QStringLiteral("ttf"),
        QStringLiteral("otf"),    QStringLiteral("woff"),
        QStringLiteral("woff2"),  QStringLiteral("p12"),
        QStringLiteral("pfx"),
    };
    if (kBinarySuffixes.contains(extensionOf(lower)))
        return false;
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
        //
        // The source handler is spelled "text", the ONE canonical word for it.
        // "editor" used to be listed beside it as a separate choice, from a
        // time when a second, read-only text pane existed; that pane was
        // removed (see the note on ch::ViewerModel) and the two words have
        // meant the same handler ever since — ViewerPane.qml maps both onto the
        // same component. Offering both put two menu entries in front of the
        // user, "Editor" and "Text", that did exactly the same thing, and
        // "editor" is not even in ch::ViewerKinds::all(), which is documented
        // as the only viewer-kind words that cross the C++/QML boundary and is
        // what a persisted per-extension default is validated against. So a
        // user could be shown a default ("Editor") they could never save.
        if (ext == QLatin1String("md") || ext == QLatin1String("markdown"))
            return {QStringLiteral("markdown"), QStringLiteral("text")};
        QStringList kinds = {QStringLiteral("text")};
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

    // Reserved spellings, all refused. Two groups, for two different reasons.
    //
    // The first is CodeHarbor's own scheme plus the browser/file schemes the
    // viewer handles itself: handing one of these to the desktop would route a
    // resource the application owns back out to whatever else claims it.
    //
    // The second is the pseudo-schemes whose "path" IS a program or a document
    // body rather than a locator. applicationUrl() percent-escapes the remote
    // path so it stays one URL path, but escaping does not disarm anything
    // here: the receiving browser un-escapes it again and then EXECUTES it. A
    // desktop handler is launched with the user's full privileges, so a scheme
    // that turns a server-supplied string into code is never handed over.
    static const QSet<QString> kReserved = {
        QStringLiteral("codeharbor-internal"),
        QStringLiteral("http"),
        QStringLiteral("https"),
        QStringLiteral("file"),
        QStringLiteral("javascript"),
        QStringLiteral("vbscript"),
        QStringLiteral("data"),
        QStringLiteral("blob"),
        QStringLiteral("about"),
        QStringLiteral("view-source"),
        QStringLiteral("filesystem"),
    };
    return !kReserved.contains(scheme.toLower());
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
