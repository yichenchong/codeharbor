#include "AppSettings.h"

#include "ViewerKinds.h"

#include <QMetaType>
#include <QSet>
#include <QSettings>
#include <cmath>
#include <algorithm>

namespace ch {

namespace {

const QString kPrefix = QStringLiteral("settings/");

const QString kThemeKey = QStringLiteral("settings/appearance/theme");
const QString kPaletteKey = QStringLiteral("settings/appearance/groupPalette");
const QString kPaletteSizeKey =
    QStringLiteral("settings/appearance/groupPaletteSize");
const QString kToolbarKey = QStringLiteral("settings/appearance/toolbarOrder");
const QString kFontSizeKey =
    QStringLiteral("settings/appearance/terminalFontSize");
const QString kPixelRatioKey =
    QStringLiteral("settings/appearance/terminalPixelRatio");
const QString kViewerDefaultsGroup =
    QStringLiteral("settings/viewerDefaults");

const QString kDefaultTheme = QStringLiteral("dark");
const QString kDefaultPalette = QStringLiteral("plain");
// The set of names each choice preference accepts. A stored value outside the
// set is a hand-edited file or a preference written by a NEWER build, and both
// have the same right answer: fall back to the default rather than hand a
// binding a name nothing can resolve.
bool isKnownTheme(const QString& value)
{
    return value == QLatin1String("dark") || value == QLatin1String("light");
}

bool isKnownPalette(const QString& value)
{
    return value == QLatin1String("plain")
           || value == QLatin1String("tokyonight");
}

// A whole number from the store, clamped into [minimum, maximum]. As in
// UiStateStore, QVariant::toInt() answering 0 for unparseable text is the thing
// being guarded: one half-written line must not become a zero-point font.
int storedInt(const QSettings& settings, const QString& key, int fallback,
              int minimum, int maximum)
{
    const QVariant raw = settings.value(key);
    if (!raw.isValid())
        return fallback;
    bool ok = false;
    const int value = raw.toInt(&ok);
    if (!ok)
        return fallback;
    return std::clamp(value, minimum, maximum);
}

// Empty and repeated ids are dropped: an id appearing twice would place one
// button in two positions, and an empty one names nothing at all.
QStringList sanitiseOrder(const QStringList& order)
{
    QStringList clean;
    clean.reserve(order.size());
    QSet<QString> seen;
    for (const QString& raw : order) {
        const QString id = raw.trimmed();
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        clean.append(id);
    }
    return clean;
}

// A stored key is stricter than a typed UI value: a leading dot is rejected
// when it comes from the hand-editable file, while ViewerKinds accepts one for
// the convenience setter used by the settings page.
QString storedExtension(const QString& raw)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.startsWith(QLatin1Char('.')))
        return {};
    return ViewerKinds::normaliseExtension(trimmed);
}

bool isStringVariant(const QVariant& value)
{
    return value.isValid() && value.metaType().id() == QMetaType::QString;
}

QVariantMap sanitiseViewerDefaults(const QVariantMap& input)
{
    QVariantMap clean;
    for (auto it = input.cbegin(); it != input.cend(); ++it) {
        const QString extension = ViewerKinds::normaliseExtension(it.key());
        if (extension.isEmpty() || !isStringVariant(it.value()))
            continue;
        const QString kind = it.value().toString();
        if (!ViewerKinds::canAssign(extension, kind))
            continue;
        clean.insert(extension, kind);
    }
    return clean;
}

QVariantMap readViewerDefaults(QSettings& settings)
{
    QVariantMap clean;
    settings.beginGroup(kViewerDefaultsGroup);
    const QStringList keys = settings.childKeys();
    for (const QString& rawKey : keys) {
        const QString extension = storedExtension(rawKey);
        if (extension.isEmpty())
            continue;
        const QVariant rawValue = settings.value(rawKey);
        if (!isStringVariant(rawValue))
            continue;
        const QString kind = rawValue.toString();
        if (!ViewerKinds::canAssign(extension, kind))
            continue;

        // If a hand-edited file contains both `MD` and `md`, prefer the
        // canonical spelling regardless of QSettings' key ordering.
        if (!clean.contains(extension)
            || rawKey.trimmed() == extension)
            clean.insert(extension, kind);
    }
    settings.endGroup();
    return clean;
}

// A generic pair may not carry '/': the separator is what keeps one group's
// keys inside its own subtree, and a key containing it could otherwise be spelt
// so that it lands on one of the validated appearance keys above (and be
// written without passing that key's validation).
bool isAddressable(const QString& group, const QString& key)
{
    return !group.isEmpty() && !key.isEmpty()
           && !group.contains(QLatin1Char('/'))
           && !key.contains(QLatin1Char('/'));
}

} // namespace

AppSettings::AppSettings(QString iniPath, QObject* parent)
    : QObject(parent)
    , m_settings(iniPath.isEmpty()
                     ? std::make_unique<QSettings>()
                     : std::make_unique<QSettings>(iniPath,
                                                   QSettings::IniFormat))
{
}

AppSettings::~AppSettings() = default;

QString AppSettings::theme() const
{
    const QString stored = m_settings->value(kThemeKey).toString();
    return isKnownTheme(stored) ? stored : kDefaultTheme;
}

void AppSettings::setTheme(const QString& theme)
{
    if (!isKnownTheme(theme) || theme == this->theme())
        return;
    m_settings->setValue(kThemeKey, theme);
    emit themeChanged();
}

QString AppSettings::groupPalette() const
{
    const QString stored = m_settings->value(kPaletteKey).toString();
    return isKnownPalette(stored) ? stored : kDefaultPalette;
}

void AppSettings::setGroupPalette(const QString& palette)
{
    if (!isKnownPalette(palette) || palette == groupPalette())
        return;
    m_settings->setValue(kPaletteKey, palette);
    emit groupPaletteChanged();
}

int AppSettings::groupPaletteSize() const
{
    return storedInt(*m_settings, kPaletteSizeKey, kDefaultPaletteSize,
                     kMinPaletteSize, kMaxPaletteSize);
}

void AppSettings::setGroupPaletteSize(int size)
{
    const int clamped = std::clamp(size, kMinPaletteSize, kMaxPaletteSize);
    if (clamped == groupPaletteSize())
        return;
    m_settings->setValue(kPaletteSizeKey, clamped);
    emit groupPaletteSizeChanged();
}

QStringList AppSettings::toolbarOrder() const
{
    return sanitiseOrder(m_settings->value(kToolbarKey).toStringList());
}

void AppSettings::setToolbarOrder(const QStringList& order)
{
    const QStringList clean = sanitiseOrder(order);
    if (clean == toolbarOrder())
        return;
    m_settings->setValue(kToolbarKey, clean);
    emit toolbarOrderChanged();
}

int AppSettings::terminalFontSize() const
{
    return storedInt(*m_settings, kFontSizeKey, kDefaultTerminalFontSize,
                     kMinTerminalFontSize, kMaxTerminalFontSize);
}

void AppSettings::setTerminalFontSize(int points)
{
    const int clamped =
        std::clamp(points, kMinTerminalFontSize, kMaxTerminalFontSize);
    if (clamped == terminalFontSize())
        return;
    m_settings->setValue(kFontSizeKey, clamped);
    emit terminalFontSizeChanged();
}

qreal AppSettings::terminalPixelRatio() const
{
    // Default 0 means "whatever the screen reports": the renderer asks the
    // window for its own ratio, which is right on every ordinary display and is
    // what this preference exists to OVERRIDE, not to replace.
    const QVariant raw = m_settings->value(kPixelRatioKey);
    if (!raw.isValid())
        return 0.0;
    bool ok = false;
    const qreal value = raw.toDouble(&ok);
    if (!ok || !std::isfinite(value) || value <= 0.0)
        return 0.0;
    return std::clamp(value, kMinTerminalPixelRatio, kMaxTerminalPixelRatio);
}

void AppSettings::setTerminalPixelRatio(qreal ratio)
{
    // A non-positive ratio is how the caller says "follow the screen again",
    // and is stored as the absent value rather than as a number.
    const qreal wanted =
        !std::isfinite(ratio) || ratio <= 0.0
            ? 0.0
            : std::clamp(ratio, kMinTerminalPixelRatio, kMaxTerminalPixelRatio);
    if (qFuzzyCompare(wanted + 1.0, terminalPixelRatio() + 1.0))
        return;
    if (wanted <= 0.0)
        m_settings->remove(kPixelRatioKey);
    else
        m_settings->setValue(kPixelRatioKey, wanted);
    emit terminalPixelRatioChanged();
}

QVariantMap AppSettings::viewerDefaults() const
{
    return readViewerDefaults(*m_settings);
}

QHash<QString, QString> AppSettings::viewerDefaultKinds() const
{
    QHash<QString, QString> result;
    const QVariantMap map = viewerDefaults();
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        result.insert(it.key(), it.value().toString());
    return result;
}

void AppSettings::setViewerDefaults(const QVariantMap& defaults)
{
    const QVariantMap clean = sanitiseViewerDefaults(defaults);
    if (clean == viewerDefaults())
        return;

    m_settings->remove(kViewerDefaultsGroup);
    for (auto it = clean.cbegin(); it != clean.cend(); ++it)
        m_settings->setValue(kViewerDefaultsGroup + QLatin1Char('/') + it.key(),
                             it.value());
    emit viewerDefaultsChanged();
}

bool AppSettings::setViewerDefault(const QString& extension,
                                   const QString& kind)
{
    const QString canonical = ViewerKinds::normaliseExtension(extension);
    if (canonical.isEmpty() || !ViewerKinds::canAssign(canonical, kind))
        return false;

    QVariantMap next = viewerDefaults();
    if (next.value(canonical).toString() == kind
        && next.contains(canonical))
        return true;
    next.insert(canonical, kind);
    setViewerDefaults(next);
    return true;
}

bool AppSettings::clearViewerDefault(const QString& extension)
{
    const QString canonical = ViewerKinds::normaliseExtension(extension);
    if (canonical.isEmpty())
        return false;

    QVariantMap next = viewerDefaults();
    if (!next.contains(canonical))
        return false;
    next.remove(canonical);
    setViewerDefaults(next);
    return true;
}

QVariant AppSettings::value(const QString& group, const QString& key,
                            const QVariant& fallback) const
{
    if (!isAddressable(group, key))
        return fallback;
    return m_settings->value(kPrefix + group + QLatin1Char('/') + key,
                             fallback);
}

void AppSettings::setValue(const QString& group, const QString& key,
                           const QVariant& value)
{
    if (!isAddressable(group, key))
        return;
    const QString path = kPrefix + group + QLatin1Char('/') + key;
    if (m_settings->value(path) == value)
        return;
    // An invalid QVariant is how QML spells "clear this": storing it would park
    // an empty line in the file that later reads back as a present-but-blank
    // value rather than as an absent key.
    if (!value.isValid())
        m_settings->remove(path);
    else
        m_settings->setValue(path, value);
    emit settingChanged(group, key);
}

void AppSettings::resetToDefaults()
{
    // Read first: the signals below must reflect what actually moved, and every
    // getter answers the default once the keys are gone.
    const QString theme = this->theme();
    const QString palette = groupPalette();
    const int paletteSize = groupPaletteSize();
    const QStringList toolbar = toolbarOrder();
    const int fontSize = terminalFontSize();
    const qreal pixelRatio = terminalPixelRatio();
    const QVariantMap viewerDefaults = this->viewerDefaults();

    // The whole subtree, so the generic groups go too. Anything outside
    // `settings/` (UiStateStore's own keys share this file) is untouched.
    bool hadGeneric = false;
    for (const QString& key : m_settings->allKeys()) {
        if (!key.startsWith(kPrefix))
            continue;
        if (key == kThemeKey || key == kPaletteKey
            || key == kPaletteSizeKey || key == kToolbarKey
            || key == kFontSizeKey || key == kPixelRatioKey
            || key.startsWith(kViewerDefaultsGroup + QLatin1Char('/'))) {
            continue;
        }
        hadGeneric = true;
        break;
    }
    m_settings->remove(QStringLiteral("settings"));

    if (theme != this->theme())
        emit themeChanged();
    if (palette != groupPalette())
        emit groupPaletteChanged();
    if (paletteSize != groupPaletteSize())
        emit groupPaletteSizeChanged();
    if (toolbar != toolbarOrder())
        emit toolbarOrderChanged();
    if (fontSize != terminalFontSize())
        emit terminalFontSizeChanged();
    if (!qFuzzyCompare(pixelRatio + 1.0, terminalPixelRatio() + 1.0))
        emit terminalPixelRatioChanged();
    if (!viewerDefaults.isEmpty() && this->viewerDefaults().isEmpty())
        emit viewerDefaultsChanged();
    if (hadGeneric) {
        // Generic pairs have no per-key record of what existed, so one blanket
        // notification with empty names says "re-read everything".
        emit settingChanged(QString(), QString());
    }
}

} // namespace ch
