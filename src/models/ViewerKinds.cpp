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

const QSet<QString>& textExtensions()
{
    static const QSet<QString> extensions = {
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
    return extensions;
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
