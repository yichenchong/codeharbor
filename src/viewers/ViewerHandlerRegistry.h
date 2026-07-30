#pragma once

#include <QString>
#include <QUrl>

namespace ch {

// Resolution type produced by the handler registry for a given URL/MIME/ext
// (SPEC 7.5). The lightweight registry is the initial extensibility mechanism
// in place of a full plugin system.
enum class ViewerResolution {
    DirectWebNavigation,
    InternalHtmlRenderer,
    TextEditor,
    ImageViewer,
    PdfViewer,
    DirectoryViewer,
    Download,
    OpenExternally,
    Error,
};

// The viewer handler registry (SPEC 7.5). Resolution is a pure function of the
// URL: the scheme decides http/https vs. the privileged internal scheme, and
// remote file:// URLs are classified by trailing-slash (directory) then file
// extension. The table is intentionally data-driven and side-effect free so it
// is exhaustively unit-testable without a live server or WebEngine.
class ViewerHandlerRegistry {
public:
    // Full URL resolution. Scheme is consulted first (http/https ->
    // DirectWebNavigation; codeharbor-internal -> InternalHtmlRenderer, or by
    // extension when the internal URL carries a recognizable one), then for
    // file:// URLs a trailing '/' means DirectoryViewer and everything else is
    // classified by extension. Unknown schemes resolve to Error.
    static ViewerResolution resolve(const QUrl &url);

    // Classify a bare file extension (without the leading dot, case
    // insensitive). Unknown / known-binary extensions resolve to Download.
    static ViewerResolution resolveByExtension(const QString &ext);
};

} // namespace ch
