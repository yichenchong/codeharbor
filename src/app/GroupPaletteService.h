#pragma once

#include "GroupPalette.h"

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace ch {

// QObject adapter for the pure GroupPalette maths. It is published as the
// `groupPalette` context property because QML context objects are already the
// application's pattern for long-lived C++ services. The adapter owns the
// cache; changing either palette name or requested size invalidates the
// expanded colours and all name slots, while repeated reads for the same pair
// only perform one stable hash lookup per distinct group name.
class GroupPaletteService final : public QObject {
    Q_OBJECT

public:
    explicit GroupPaletteService(QObject *parent = nullptr);

    Q_INVOKABLE QColor colorFor(const QString &name, const QString &paletteName,
                                int requestedCount);

private:
    const QVector<SrgbColor> &expandedPalette(const QString &paletteName,
                                              int requestedCount);

    QString m_cachedPaletteName;
    int m_cachedPaletteSize = 0;
    QVector<SrgbColor> m_cachedPalette;
    QHash<QString, int> m_cachedNameIndices;
};

} // namespace ch
