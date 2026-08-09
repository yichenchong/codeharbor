#include "GroupPalette.h"

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

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
    if (!std::isfinite(value))
        return 0.0;
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

double labDistance(const Oklab &first, const Oklab &second)
{
    // The gap metric is Euclidean distance in OKLab (the Cartesian form of
    // OKLCH), not distance between 8-bit RGB channels. That makes a colour's
    // perceptual lightness and chroma count in the same space, and working in
    // the Cartesian projection makes the hue seam at 0 == 2*pi disappear.
    return std::sqrt((first.lightness - second.lightness) * (first.lightness - second.lightness)
                     + (first.a - second.a) * (first.a - second.a)
                     + (first.b - second.b) * (first.b - second.b));
}

LinearRgb oklchToLinearRgb(const Oklch &color)
{
    const double lightness = clamp01(color.lightness);
    const double chroma = std::isfinite(color.chroma)
            ? std::clamp(color.chroma, 0.0, 1.0)
            : 0.0;
    const double hue = std::isfinite(color.hue) ? normalizeHue(color.hue) : 0.0;
    const double a = chroma * std::cos(hue);
    const double b = chroma * std::sin(hue);

    const double l = lightness + 0.3963377774 * a + 0.2158037573 * b;
    const double m = lightness - 0.1055613458 * a - 0.0638541728 * b;
    const double s = lightness - 0.0894841775 * a - 1.2914855480 * b;

    const double l3 = l * l * l;
    const double m3 = m * m * m;
    const double s3 = s * s * s;
    return {4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3,
            -1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3,
            -0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3};
}

bool insideSrgbGamut(const Oklch &color)
{
    // A colour whose linear components fall outside [0, 1] gets clipped on the
    // way to sRGB, and clipping moves it: two OKLab points that were far apart
    // can clip onto the very same displayable colour. Expansion therefore only
    // offers colours that survive the conversion untouched, so the separation
    // it measures is the separation the user actually sees.
    const LinearRgb rgb = oklchToLinearRgb(color);
    constexpr double tolerance = 1.0e-6;
    return rgb.red >= -tolerance && rgb.red <= 1.0 + tolerance
            && rgb.green >= -tolerance && rgb.green <= 1.0 + tolerance
            && rgb.blue >= -tolerance && rgb.blue <= 1.0 + tolerance;
}

struct Candidate {
    Oklch color;
    Oklab lab;
};

// The colours expansion is allowed to choose from. A fixed lattice over OKLCH
// is what keeps the generator both pure and well spread: the hue ring is walked
// in even steps at several lightness and chroma levels, so whatever the seed
// looks like there is always a genuinely distant colour left to pick. The
// ranges deliberately exclude near-black, near-white and near-grey, because a
// group tint has to stay legible against both the light and the dark surface
// colours in Theme.qml.
constexpr int kLightnessSteps = 7;
constexpr int kChromaSteps = 5;
constexpr int kHueSteps = 48;
constexpr double kMinLightness = 0.45;
constexpr double kMaxLightness = 0.87;
constexpr double kMinChroma = 0.06;
constexpr double kMaxChroma = 0.26;

const QVector<Candidate> &candidateColours()
{
    // Built once and reused. The lattice depends on nothing but the constants
    // above, so this is a cache of a constant rather than hidden state: every
    // call in every process sees the same colours in the same order.
    static const QVector<Candidate> candidates = [] {
        QVector<Candidate> built;
        built.reserve(kLightnessSteps * kChromaSteps * kHueSteps);
        for (int lightnessStep = 0; lightnessStep < kLightnessSteps; ++lightnessStep) {
            const double lightness = kMinLightness
                    + (kMaxLightness - kMinLightness) * lightnessStep / (kLightnessSteps - 1);
            for (int chromaStep = 0; chromaStep < kChromaSteps; ++chromaStep) {
                const double chroma = kMinChroma
                        + (kMaxChroma - kMinChroma) * chromaStep / (kChromaSteps - 1);
                for (int hueStep = 0; hueStep < kHueSteps; ++hueStep) {
                    const Oklch candidate{lightness, chroma, kTwoPi * hueStep / kHueSteps};
                    if (!insideSrgbGamut(candidate))
                        continue;
                    built.append({candidate, toLab(candidate)});
                }
            }
        }
        return built;
    }();
    return candidates;
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
    const LinearRgb rgb = oklchToLinearRgb(color);
    return {linearToSrgb(rgb.red), linearToSrgb(rgb.green), linearToSrgb(rgb.blue)};
}

double GroupPalette::perceptualDistance(const SrgbColor &first, const SrgbColor &second)
{
    return labDistance(toLab(srgbToOklch(first)), toLab(srgbToOklch(second)));
}

bool GroupPalette::canGenerate(const QVector<SrgbColor> &seed, int requestedCount)
{
    // The upper bound is not cosmetic: expansion measures every candidate
    // colour against every colour already chosen, so its cost grows with the
    // square of the requested count (see kMaxPaletteSize). An out-of-range
    // preference would hang the application rather than merely look wrong.
    return !seed.isEmpty() && requestedCount > seed.size()
            && requestedCount <= kMaxPaletteSize;
}

QVector<SrgbColor> GroupPalette::generatePalette(const QVector<SrgbColor> &seed,
                                                  int requestedCount)
{
    Q_ASSERT_X(canGenerate(seed, requestedCount), "GroupPalette::generatePalette",
               "requestedCount must exceed the number of seed colours and must "
               "not exceed kMaxPaletteSize");
    if (!canGenerate(seed, requestedCount))
        return {};

    QVector<SrgbColor> palette = seed;
    // Reserved for the same reason `chosen` below is: the loop appends exactly
    // one colour per pass, so without this the vector is reallocated and copied
    // on the way from seed.size() up to requestedCount.
    palette.reserve(requestedCount);
    // The OKLab view of `palette`, kept in step with it rather than rebuilt
    // from scratch on every pass: each pass adds exactly one colour, so one
    // conversion per pass is all that changes.
    QVector<Oklab> chosen;
    chosen.reserve(requestedCount);
    for (const SrgbColor &colour : palette)
        chosen.append(toLab(srgbToOklch(colour)));

    // Farthest-point selection: each new colour is the candidate whose nearest
    // already-chosen neighbour is as far away as possible, which is what makes
    // the whole palette spread out instead of merely differ.
    //
    // The earlier version halved the perceptual gap between the two most
    // distant colours instead. Inserting a midpoint does not shorten the
    // distance between the pair it came from, so that pair stayed the widest
    // apart on the next pass and the very same midpoint was appended again:
    // from the seventh colour onwards every entry in the palette was the
    // identical colour, which is why differently named groups kept sharing a
    // tint at every palette size.
    //
    // Ties keep the first candidate in lattice order, so the output is stable
    // even when two candidates are equally far away to the last bit.
    const QVector<Candidate> &candidates = candidateColours();
    while (palette.size() < requestedCount) {
        qsizetype best = 0;
        double bestSeparation = -1.0;
        for (qsizetype i = 0; i < candidates.size(); ++i) {
            double nearest = std::numeric_limits<double>::max();
            for (const Oklab &taken : chosen) {
                nearest = std::min(nearest, labDistance(candidates.at(i).lab, taken));
                // This candidate can no longer beat the leader, so there is no
                // point measuring it against the rest of the palette.
                if (nearest <= bestSeparation)
                    break;
            }
            if (nearest > bestSeparation) {
                bestSeparation = nearest;
                best = i;
            }
        }
        const SrgbColor appended = oklchToSrgb(candidates.at(best).color);
        palette.append(appended);
        // Measured back from the colour that will actually be displayed rather
        // than from the candidate, so the next choice accounts for the small
        // shift the OKLCH to sRGB conversion introduces.
        chosen.append(toLab(srgbToOklch(appended)));
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
