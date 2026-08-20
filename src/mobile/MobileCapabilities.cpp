#include "MobileCapabilities.h"

// Both macros are always defined by src/mobile/CMakeLists.txt, which normalises
// an undefined probe to 0 before adding the definition. They are re-defaulted
// here anyway so this translation unit also compiles in a host that includes it
// without that target's definitions (an IDE's standalone index, a future test
// that links the source directly) rather than failing on an unknown identifier.
#ifndef CH_HAVE_QTPDF
#define CH_HAVE_QTPDF 0
#endif
#ifndef CH_HAVE_QTWEBVIEW
#define CH_HAVE_QTWEBVIEW 0
#endif

namespace ch {

MobileCapabilities::MobileCapabilities(QObject* parent)
    : QObject(parent)
{
}

bool MobileCapabilities::hasPdf() const
{
    return CH_HAVE_QTPDF != 0;
}

bool MobileCapabilities::hasWebView() const
{
    return CH_HAVE_QTWEBVIEW != 0;
}

} // namespace ch
