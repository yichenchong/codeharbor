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

    // A generator request must add at least one colour beyond the seed. The
    // QML adapter handles an exact seed-sized preference by returning the seed
    // unchanged, while this assertion catches a too-small programmer request.
    // Callers that receive user preferences should check this predicate first
    // so a hand-edited settings file cannot abort the application in a release
    // build.
    static bool canGenerate(const QVector<SrgbColor> &seed, int requestedCount);
    static QVector<SrgbColor> generatePalette(const QVector<SrgbColor> &seed,
                                              int requestedCount);
    static QVector<SrgbColor> tokyoNightSeed();
    static int stableIndexForName(const QString &name, int paletteSize);
};

} // namespace ch
