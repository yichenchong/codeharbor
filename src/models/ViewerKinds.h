#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace ch::ViewerKinds {

// The only viewer-kind words that cross the C++/QML boundary. `empty` is a
// pane-only placeholder and deliberately does not belong to this list.
QStringList all();

// Exact, case-sensitive check for a canonical kind word.
bool isKnown(const QString& kind);

// Return a canonical extension (trimmed and lower-cased). A single leading dot
// is accepted for typed UI input and removed; extensions never carry a dot in
// stored form. Invalid keys answer an empty string.
//
// Accepted shape: the first character must be an ASCII letter or digit, and
// the rest may be ASCII letters, digits, '+' or '-'; at most 64 characters.
// The two punctuation marks are there because real files carry them and
// refusing them made those files unrepresentable as a stored preference:
// '+' for the C++ suffixes c++ and h++, '-' for hyphenated tool-config names
// such as clang-format. Everything else is refused because the result is used
// as a key inside a settings group (see ch::AppSettings::setViewerDefault):
// '/' and '\' would let a key escape its own group, '[' ']' '=' ';' '#' are
// INI syntax, '.' would collide with the leading-dot rule above, and non-ASCII
// would lower-case into a key no handler table contains.
QString normaliseExtension(const QString& extension);

// The file extensions the image handler can actually draw, in canonical
// normaliseExtension() form (bare, lower-cased, no leading dot).
//
// Exported because it is the SINGLE source of truth for "is this an image":
// the viewer registry decides whether to render a file with it, and
// assignableForExtension() below decides whether "image" is even offered as
// that extension's stored default. A second copy of the list would let the two
// disagree - offering the image viewer for a file it refuses to draw, or
// withholding it for one it would draw perfectly well. Returns a reference to
// a static table; no copy is made.
const QSet<QString>& imageExtensions();

// The handlers that can display a file with this extension. text and binary
// are intentionally present for every valid extension; the specialised kinds
// are limited to what their handlers can actually render.
QStringList assignableForExtension(const QString& extension);

bool canAssign(const QString& extension, const QString& kind);

} // namespace ch::ViewerKinds
