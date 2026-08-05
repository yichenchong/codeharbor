#pragma once

#include <QString>
#include <QVector>

namespace ch {

// A linear-free sRGB value used by the palette algorithm. Keeping the core
// colour representation as three doubles lets ch_models stay a QtCore-only
// library; the QML-facing adapter converts these components to QColor at the
// boundary where QtGui is already part of the application.
struct SrgbColor {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;

    bool operator==(const SrgbColor &) const = default;
};

// Cylindrical OKLab. L is perceptual lightness, C is chroma, and h is a
// circular angle in radians in [0, 2*pi). Keeping this as a small value type
// makes the conversion and interpolation tests independent of a GUI or QML
// engine.
struct Oklch {
    double lightness = 0.0;
    double chroma = 0.0;
    double hue = 0.0;

    bool operator==(const Oklch &) const = default;
};

// Deterministic group colours. The conversion and palette expansion are pure
// static operations so model tests can exercise them without creating a GUI.
// GroupPaletteService (src/app) is the QObject surface main.cpp publishes to
// QML; it accepts preference values as arguments instead of owning a second
// copy of AppSettings.
class GroupPalette final {
public:
    static Oklch srgbToOklch(const SrgbColor &color);
    static SrgbColor oklchToSrgb(const Oklch &color);

    // Largest palette this generator will produce. Expansion compares every
    // pair of colours already chosen for each new one, so the work grows with
    // the CUBE of the requested count: asking for a few thousand colours would
    // freeze the user interface for minutes while it computed a palette nobody
    // could tell apart. Group tints need dozens, not thousands, so an
    // out-of-range request is refused outright instead.
    //
    // This must stay at or above ch::AppSettings::kMaxPaletteSize (64), the
    // upper end of the user-facing preference; if that preference is ever
    // widened past this number, raise this one with it or the affected sizes
    // silently produce no palette at all.
    static constexpr int kMaxPaletteSize = 256;

    // A generator request must add at least one colour beyond the seed, and
    // must not ask for more than kMaxPaletteSize in total. The QML adapter
    // handles an exact seed-sized preference by returning the seed unchanged,
    // while this predicate catches a too-small or absurdly large request.
    // Callers that receive user preferences should check it first so a
    // hand-edited settings file can neither abort the application in a release
    // build nor hang it in any build.
    static bool canGenerate(const QVector<SrgbColor> &seed, int requestedCount);
    // Precondition: canGenerate(seed, requestedCount). Returns an empty vector
    // when that does not hold.
    static QVector<SrgbColor> generatePalette(const QVector<SrgbColor> &seed,
                                              int requestedCount);
    static QVector<SrgbColor> tokyoNightSeed();
    static int stableIndexForName(const QString &name, int paletteSize);
};

} // namespace ch
