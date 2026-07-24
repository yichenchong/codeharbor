#pragma once

#include <QString>

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

// Bootstrap placeholder resolving by URL scheme only. Extension/MIME rules and
// the registry data structure land in docs/PLAN.md workstream V.
class ViewerHandlerRegistry {
public:
    static ViewerResolution resolveScheme(const QString &scheme);
};

} // namespace ch
