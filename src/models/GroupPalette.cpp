#include "GroupPalette.h"
#include <QByteArray>

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ch {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kHueEpsilon = 1.0e-12;

struct Oklab {
    double lightness = 0.0;
    double a = 0.0;
    double b = 0.0;
};

struct LinearRgb {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

double srgbToLinear(double value)
{
    return value <= 0.04045 ? value / 12.92
                            : std::pow((value + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double value)
{
    // OKLab interpolation can briefly leave the sRGB gamut. Clamping happens
    // only at this final boundary; keeping the intermediate OKLCH values
    // unclipped preserves the requested perceptual midpoint as closely as the
    // destination colour space permits.
    const double encoded = value <= 0.0031308 ? 12.92 * value
                                              : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    return clamp01(encoded);
}

double normalizeHue(double hue)
{
    hue = std::fmod(hue, kTwoPi);
    if (hue < 0.0)
        hue += kTwoPi;
    return hue;
}

double shortestHueDelta(double from, double to)
{
    double delta = std::fmod(to - from, kTwoPi);
    if (delta > kPi)
        delta -= kTwoPi;
    else if (delta < -kPi)
        delta += kTwoPi;
    return delta;
}

Oklab toLab(const Oklch &color)
{
    return {color.lightness, color.chroma * std::cos(color.hue),
            color.chroma * std::sin(color.hue)};
}

Oklch fromLab(const Oklab &color)
{
    const double chroma = std::hypot(color.a, color.b);
    const double hue = chroma <= kHueEpsilon ? 0.0 : normalizeHue(std::atan2(color.b, color.a));
    return {color.lightness, chroma, hue};
}

double labDistance(const Oklch &first, const Oklch &second)
{
    // The gap metric is Euclidean distance in OKLab (the Cartesian form of
    // OKLCH), not distance between 8-bit RGB channels. That makes a colour's
    // perceptual lightness and chroma count in the same space, and the
    // Cartesian projection makes the hue seam at 0 == 2*pi disappear.
    const Oklab a = toLab(first);
    const Oklab b = toLab(second);
    return std::sqrt((a.lightness - b.lightness) * (a.lightness - b.lightness)
                     + (a.a - b.a) * (a.a - b.a) + (a.b - b.b) * (a.b - b.b));
}

Oklch halfway(const Oklch &first, const Oklch &second)
{
    double firstHue = first.hue;
    double secondHue = second.hue;

    // Hue has no direction at C == 0. Borrow the chromatic endpoint's hue in
    // that case, then use the shortest circular arc so red/purple hues near the
    // 0/2*pi seam do not travel through every unrelated colour in between.
    if (first.chroma <= kHueEpsilon && second.chroma > kHueEpsilon)
        firstHue = secondHue;
    else if (second.chroma <= kHueEpsilon && first.chroma > kHueEpsilon)
        secondHue = firstHue;

    const double delta = shortestHueDelta(firstHue, secondHue);
    return {(first.lightness + second.lightness) * 0.5,
            (first.chroma + second.chroma) * 0.5,
            normalizeHue(firstHue + delta * 0.5)};
}

SrgbColor fromHex(unsigned int red, unsigned int green, unsigned int blue)
{
    return {red / 255.0, green / 255.0, blue / 255.0};
}

} // namespace

Oklch GroupPalette::srgbToOklch(const SrgbColor &color)
{
    const LinearRgb rgb{srgbToLinear(clamp01(color.red)), srgbToLinear(clamp01(color.green)),
                        srgbToLinear(clamp01(color.blue))};

    // Bjorn Ottosson's OKLab matrices. The first matrix converts linear sRGB
    // to LMS; the cube-rooted LMS values then become OKLab coordinates.
    const double l = std::cbrt(0.4122214708 * rgb.red + 0.5363325363 * rgb.green
                               + 0.0514459929 * rgb.blue);
    const double m = std::cbrt(0.2119034982 * rgb.red + 0.6806995451 * rgb.green
                               + 0.1073969566 * rgb.blue);
    const double s = std::cbrt(0.0883024619 * rgb.red + 0.2817188376 * rgb.green
                               + 0.6299787005 * rgb.blue);

    const Oklab lab{0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                    1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                    0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
    return fromLab(lab);
}

SrgbColor GroupPalette::oklchToSrgb(const Oklch &color)
{
    const double hue = std::isfinite(color.hue) ? normalizeHue(color.hue) : 0.0;
    const double a = color.chroma * std::cos(hue);
    const double b = color.chroma * std::sin(hue);

    const double l = color.lightness + 0.3963377774 * a + 0.2158037573 * b;
    const double m = color.lightness - 0.1055613458 * a - 0.0638541728 * b;
    const double s = color.lightness - 0.0894841775 * a - 1.2914855480 * b;

    const double l3 = l * l * l;
    const double m3 = m * m * m;
    const double s3 = s * s * s;
    const double red = 4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3;
    const double green = -1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3;
    const double blue = -0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3;

    return {linearToSrgb(red), linearToSrgb(green), linearToSrgb(blue)};
}

bool GroupPalette::canGenerate(const QVector<SrgbColor> &seed, int requestedCount)
{
    return !seed.isEmpty() && requestedCount > seed.size();
}

QVector<SrgbColor> GroupPalette::generatePalette(const QVector<SrgbColor> &seed,
                                                  int requestedCount)
{
    Q_ASSERT_X(canGenerate(seed, requestedCount), "GroupPalette::generatePalette",
               "requestedCount must be greater than the number of seed colours");
    if (!canGenerate(seed, requestedCount))
        return {};

    QVector<SrgbColor> palette = seed;
    while (palette.size() < requestedCount) {
        int first = 0;
        int second = 1;
        double largestGap = -1.0;
        QVector<Oklch> converted;
        converted.reserve(palette.size());
        for (const SrgbColor &colour : palette)
            converted.append(srgbToOklch(colour));

        // This is recursive expansion in iterative form: each loop starts with
        // the complete (n-1)-colour result and appends exactly one midpoint.
        // Ties retain the first pair in palette order, making output stable
        // even when two gaps have the same floating-point distance.
        for (int i = 0; i < converted.size(); ++i) {
            for (int j = i + 1; j < converted.size(); ++j) {
                const double gap = labDistance(converted.at(i), converted.at(j));
                if (gap > largestGap) {
                    largestGap = gap;
                    first = i;
                    second = j;
                }
            }
        }
        palette.append(oklchToSrgb(halfway(converted.at(first), converted.at(second))));
    }
    return palette;
}

QVector<SrgbColor> GroupPalette::tokyoNightSeed()
{
    // These are the five Tokyo Night accent colours rather than its background
    // neutrals: a group label needs to remain a visible tint on either theme's
    // surface, while the surrounding surfaces already come from Theme.qml.
    return {fromHex(0x7a, 0xa2, 0xf7), fromHex(0xbb, 0x9a, 0xf7),
            fromHex(0x7d, 0xcf, 0xff), fromHex(0x9e, 0xce, 0x6a),
            fromHex(0xf7, 0x76, 0x8e)};
}

int GroupPalette::stableIndexForName(const QString &name, int paletteSize)
{
    Q_ASSERT(paletteSize > 0);
    if (paletteSize <= 0)
        return 0;

    // FNV-1a over UTF-8 is deliberately boring: unlike qHash(QString), it is
    // not seeded per process, so the same name maps to the same slot on every
    // run and machine. The modulo is the requested [0, n) natural-number
    // range; palette expansion and the per-name lookup are cached by the QML
    // adapter, not by this pure function.
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const QByteArray bytes = name.toUtf8();
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= prime;
    }
    return static_cast<int>(hash % static_cast<std::uint64_t>(paletteSize));
}

} // namespace ch
