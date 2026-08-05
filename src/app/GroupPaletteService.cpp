#include "GroupPaletteService.h"

namespace ch {

GroupPaletteService::GroupPaletteService(QObject *parent)
    : QObject(parent)
{
}

const QVector<SrgbColor> &GroupPaletteService::expandedPalette(const QString &paletteName,
                                                                 int requestedCount)
{
    if (paletteName == m_cachedPaletteName && requestedCount == m_cachedPaletteSize)
        return m_cachedPalette;

    // The settings object emits changes, and QML re-evaluates the call with the
    // new arguments. Those are the only invalidators: keeping this cache on the
    // service means every group row shares one expansion rather than rebuilding
    // the OKLCH palette during each paint/binding pass.
    m_cachedPaletteName = paletteName;
    m_cachedPaletteSize = requestedCount;
    m_cachedNameIndices.clear();
    m_cachedPalette.clear();

    if (paletteName != QStringLiteral("tokyonight"))
        return m_cachedPalette;

    const QVector<SrgbColor> seed = GroupPalette::tokyoNightSeed();
    if (requestedCount == seed.size()) {
        // Palette size 5 is the documented lower bound; it is already the
        // complete seed, so no generator call (which must add a colour) is
        // needed.
        m_cachedPalette = seed;
    } else if (GroupPalette::canGenerate(seed, requestedCount)) {
        m_cachedPalette = GroupPalette::generatePalette(seed, requestedCount);
    }
    return m_cachedPalette;
}

QColor GroupPaletteService::colorFor(const QString &name, const QString &paletteName,
                                     int requestedCount)
{
    const QVector<SrgbColor> &palette = expandedPalette(paletteName, requestedCount);
    if (palette.isEmpty())
        return {};

    const auto existing = m_cachedNameIndices.constFind(name);
    int index = 0;
    if (existing != m_cachedNameIndices.cend()) {
        index = existing.value();
    } else {
        index = GroupPalette::stableIndexForName(name, palette.size());
        m_cachedNameIndices.insert(name, index);
    }

    const SrgbColor &colour = palette.at(index);
    return QColor::fromRgbF(colour.red, colour.green, colour.blue);
}

} // namespace ch
