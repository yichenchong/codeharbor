#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>

class QSettings;

namespace ch {

// Client-local user preferences: the things the Settings window edits (SPEC
// 4.1). Persisted with QSettings, exactly like UiStateStore, and never sent to
// the codeharbord host - a preference is a property of THIS desktop, not of the
// workspace.
//
// Deliberately separate from UiStateStore even though both are client-local and
// both use QSettings. UiStateStore holds state the application writes BY ITSELF
// as a side effect of being used (region widths, which pane was selected, which
// session was last open); this holds choices the USER made on purpose. The two
// have different lifetimes in practice - "reset my layout" must not discard a
// theme - and mixing them under one object makes that impossible to offer.
//
// Every named preference below is validated on the way OUT rather than trusted:
// the backing file is a plain .ini a person can hand-edit, and a value that is
// missing, misspelled or out of range must read back as the documented default
// instead of propagating into a binding as an empty string or a zero-sized
// font. Writes are validated the same way, so the file cannot be made to hold
// something the reader would reject.
//
// Storage keys (all under the one `settings/` prefix):
//   settings/appearance/theme
//   settings/appearance/groupPalette
//   settings/appearance/groupPaletteSize
//   settings/appearance/toolbarOrder
//   settings/appearance/terminalFontSize
//   settings/appearance/terminalPixelRatio
//   settings/<group>/<key>            (generic, for groups with no named
//                                      property yet - server, tmux)
class AppSettings : public QObject {
    Q_OBJECT

    // "dark" or "light". Anything else reads back as "dark".
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)

    // Which palette tints Dev Session group names. "plain" (the historical
    // look: no tint at all) or "tokyonight". An unknown name reads back as
    // "plain", so a palette removed in a later build degrades to the plainest
    // possible presentation rather than to no colour resolution at all.
    Q_PROPERTY(QString groupPalette READ groupPalette WRITE setGroupPalette
                   NOTIFY groupPaletteChanged)

    // How many colours the palette is expanded to before a group name is
    // hashed into it. Clamped to [kMinPaletteSize, kMaxPaletteSize]: below the
    // minimum there are fewer colours than the seed palette carries, and the
    // generator's contract is that it only ever ADDS to its seed.
    Q_PROPERTY(int groupPaletteSize READ groupPaletteSize WRITE
                   setGroupPaletteSize NOTIFY groupPaletteSizeChanged)

    // Identifiers of the toolbar buttons, in the order the user wants them.
    // Stored verbatim apart from empty and duplicate entries, which are
    // dropped: this class cannot know which ids a given build has, so
    // reconciling the list against the buttons that actually exist (ignore
    // unknown ids, append known ones the list is missing) belongs to the
    // toolbar and is what keeps the list working across a build that adds or
    // removes a button.
    Q_PROPERTY(QStringList toolbarOrder READ toolbarOrder WRITE setToolbarOrder
                   NOTIFY toolbarOrderChanged)

    // Terminal cell text size in points, and the device pixel ratio its
    // renderer draws at. The ratio is separate from the size on purpose: a
    // larger ratio makes the SAME text sharper (more physical pixels per cell)
    // rather than bigger.
    Q_PROPERTY(int terminalFontSize READ terminalFontSize WRITE
                   setTerminalFontSize NOTIFY terminalFontSizeChanged)
    Q_PROPERTY(qreal terminalPixelRatio READ terminalPixelRatio WRITE
                   setTerminalPixelRatio NOTIFY terminalPixelRatioChanged)

public:
    // Empty iniPath -> the native per-user store (org "CodeHarbor", app
    // "CodeHarbor"), i.e. the same file UiStateStore uses. Non-empty iniPath ->
    // an explicit .ini in IniFormat, for tests that need an isolated store.
    explicit AppSettings(QString iniPath = QString(), QObject* parent = nullptr);
    ~AppSettings() override;

    static constexpr int kMinPaletteSize = 5;
    static constexpr int kMaxPaletteSize = 64;
    static constexpr int kDefaultPaletteSize = 8;
    static constexpr int kMinTerminalFontSize = 6;
    static constexpr int kMaxTerminalFontSize = 48;
    static constexpr int kDefaultTerminalFontSize = 13;
    static constexpr qreal kMinTerminalPixelRatio = 1.0;
    static constexpr qreal kMaxTerminalPixelRatio = 4.0;

    QString theme() const;
    void setTheme(const QString& theme);

    QString groupPalette() const;
    void setGroupPalette(const QString& palette);

    int groupPaletteSize() const;
    void setGroupPaletteSize(int size);

    QStringList toolbarOrder() const;
    void setToolbarOrder(const QStringList& order);

    int terminalFontSize() const;
    void setTerminalFontSize(int points);

    qreal terminalPixelRatio() const;
    void setTerminalPixelRatio(qreal ratio);

    // Generic access for settings groups that have no named property yet (the
    // server and tmux groups). `group` and `key` must both be non-empty and
    // must not contain '/', so a caller cannot reach outside the `settings/`
    // subtree or forge one of the validated keys above; a rejected write is
    // dropped and a rejected read answers `fallback`.
    Q_INVOKABLE QVariant value(const QString& group, const QString& key,
                               const QVariant& fallback = QVariant()) const;
    Q_INVOKABLE void setValue(const QString& group, const QString& key,
                              const QVariant& value);

    // Every named preference back to its documented default, and every generic
    // key removed. Emits one change signal per property that actually moved.
    Q_INVOKABLE void resetToDefaults();

signals:
    void themeChanged();
    void groupPaletteChanged();
    void groupPaletteSizeChanged();
    void toolbarOrderChanged();
    void terminalFontSizeChanged();
    void terminalPixelRatioChanged();
    // One signal for the generic pairs, carrying what moved: a settings pane
    // binding to `value(group, key)` has nothing else to re-evaluate on.
    void settingChanged(const QString& group, const QString& key);

private:
    std::unique_ptr<QSettings> m_settings;
};

} // namespace ch
