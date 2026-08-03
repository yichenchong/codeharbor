#pragma once

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
QString normaliseExtension(const QString& extension);

// The handlers that can display a file with this extension. text and binary
// are intentionally present for every valid extension; the specialised kinds
// are limited to what their handlers can actually render.
QStringList assignableForExtension(const QString& extension);

bool canAssign(const QString& extension, const QString& kind);

} // namespace ch::ViewerKinds
