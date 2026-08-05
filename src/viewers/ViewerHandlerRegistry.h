#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>
namespace ch {

// Resolution type produced by the handler registry for a given URL/MIME/ext
// (SPEC 7.5). The lightweight registry is the initial extensibility mechanism
// in place of a full plugin system.
//
// Every value here is PRODUCED by resolve(); the enumeration is the registry's
// output alphabet, not a wish list. SPEC 7.5's resolution algorithm is
// browser-first — web navigation is the default disposition and a handler only
// takes a resource when it positively claims it — and its four steps between
// them mint exactly these values. "OpenExternally" used to sit here and was
// never returned by any path: handing a file to the desktop's own application
// is an ACTION the binary/download view offers (SPEC 7.5's "Binary | metadata
// and download/open actions"), taken after resolution has already landed on
// Download. It is not a disposition the browser can resolve a URL to, so it is
// not one of these.
enum class ViewerResolution {
    DirectWebNavigation,
    InternalHtmlRenderer,
    TextEditor,
    ImageViewer,
    PdfViewer,
    DirectoryViewer,
    Download,
    Error,
};

// The viewer handler registry (SPEC 7.5). Resolution is a pure function of the
// URL: the scheme decides http/https vs. the privileged internal scheme, and
// remote file:// URLs are classified by trailing-slash (directory), then file
// extension, then — only when the extension settled nothing — by a table of
// well-known text file NAMES. The tables are intentionally data-driven and side
// effect free so they are exhaustively unit-testable without a live server or
// WebEngine.
class ViewerHandlerRegistry {
public:
    // Full URL resolution. Scheme is consulted first (http/https ->
    // DirectWebNavigation; codeharbor-internal -> InternalHtmlRenderer, or by
    // extension when the internal URL carries a recognizable one), then for
    // file:// URLs a trailing '/' means DirectoryViewer and everything else is
    // classified by extension. A file whose extension resolves to Download
    // (unknown, absent, or known-binary) gets one more chance against the
    // well-known text NAMES — "Makefile", ".gitignore", "Dockerfile.dev",
    // ".env.local" — matched case-insensitively. Unknown schemes -> Error.
    static ViewerResolution resolve(const QUrl &url);

    // The explorer's "Open as" menu is derived from the same positive claims
    // as resolve(), not from a second extension table. The returned strings
    // are ch::ViewerKinds words: "markdown", "text", "image", "pdf", "binary",
    // "directory", or "web". The first item is the default. "editor" was listed
    // here once and is NOT one of them — it is not returned by any path and not
    // in ch::ViewerKinds::all(), so a persisted default spelled that way could
    // never be saved (see the note in the .cpp).
    static QStringList applicableViewKinds(const QUrl &url);

    // Application schemes are user/plugin input, so they are stricter than
    // QUrl's permissive parsing. Reserved and never handed to the desktop: the
    // built-in CodeHarbor scheme, the browser/file schemes the viewer handles
    // itself, and the pseudo-schemes whose "path" is a program or a document
    // body rather than a locator (javascript:, data:, vbscript:, ...).
    static bool isValidApplicationScheme(const QString &scheme);

    // Build the URL handed to a desktop application handler. `remotePath` is
    // data from the remote server; setPath() performs URL escaping rather than
    // concatenating it into a scheme string.
    static QUrl applicationUrl(const QString &scheme, const QString &remotePath);

    // Classify a bare file extension (without the leading dot, case
    // insensitive). Unknown / known-binary extensions resolve to Download.
    static ViewerResolution resolveByExtension(const QString &ext);
};

} // namespace ch
