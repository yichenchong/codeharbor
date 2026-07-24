#include "ViewerHandlerRegistry.h"

namespace ch {

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
