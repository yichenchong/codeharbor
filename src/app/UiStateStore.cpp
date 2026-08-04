#include "UiStateStore.h"

#include <QSettings>
#include <algorithm>

namespace ch {

namespace {
constexpr int kDefaultSidebarWidth = 260;
constexpr int kDefaultViewerWidth = 0; // 0 => fill remaining space
constexpr int kDefaultTerminalWidth = 520;
// A region with no panes yet mints "<region>-1"; see nextPaneSuffix().
constexpr int kFirstPaneSuffix = 1;

const QString kSidebarKey = QStringLiteral("layout/sidebarWidth");
const QString kViewerKey = QStringLiteral("layout/viewerWidth");
const QString kTerminalKey = QStringLiteral("layout/terminalWidth");
const QString kPinnedOnlyKey = QStringLiteral("sidebar/pinnedOnly");
const QString kShowArchivedKey = QStringLiteral("sidebar/showArchived");
// A stored value, or `fallback` when the key is absent, holds something that is
// not a whole number, or is smaller than `minimum`.
//
// value(key, default).toInt() alone is not enough: QVariant::toInt() answers 0
// for any text it cannot parse, so ONE hand-edited or half-written line
// ("sidebarWidth=" after a crash mid-write, "sidebarWidth=wide") silently
// collapses the region to zero instead of falling back to the documented
// default. `minimum` is 1 for the two regions whose width is their whole
// presence on screen and 0 for the viewer, where 0 legitimately means "fill
// whatever the other two leave"; for a pane suffix it is 1, because
// "<region>-0" is not a pane id this application has ever minted.
int storedInt(const QSettings& settings, const QString& key, int fallback,
              int minimum)
{
    const QVariant raw = settings.value(key);
    if (!raw.isValid())
        return fallback;
    bool ok = false;
    const int value = raw.toInt(&ok);
    if (!ok || value < minimum)
        return fallback;
    return value;
}
// QSettings can return a native bool or the textual form written by an INI
// editor. Accept only the two explicit values; QVariant::toBool() would turn
// arbitrary non-empty garbage into true and make a damaged settings file hide
// the whole sidebar.
bool storedBool(const QSettings& settings, const QString& key, bool fallback)
{
    const QVariant raw = settings.value(key);
    if (!raw.isValid())
        return fallback;
    const QString text = raw.toString().trimmed().toLower();
    if (text == QStringLiteral("true") || text == QStringLiteral("1"))
        return true;
    if (text == QStringLiteral("false") || text == QStringLiteral("0"))
        return false;
    return fallback;
}

QString selectedPaneKey(const QString& devSessionId)
{
    return QStringLiteral("selectedPane/") + devSessionId;
}

QString paneSuffixKey(const QString& devSessionId, const QString& region)
{
    return QStringLiteral("paneSuffix/") + devSessionId + QLatin1Char('/')
           + region;
}

QString activeSessionKey(const QString& serverId)
{
    return QStringLiteral("session/") + serverId + QStringLiteral("/active");
}

bool isAddressableRegion(const QString& region)
{
    // Region names become one path component in QSettings. Reject an empty
    // component and separators so malformed QML input cannot create a key
    // outside the documented per-region counter shape.
    return !region.isEmpty() && !region.contains(QLatin1Char('/'));
}
} // namespace

UiStateStore::UiStateStore(QString iniPath, QObject* parent)
    : QObject(parent)
{
    if (iniPath.isEmpty()) {
        m_settings = std::make_unique<QSettings>(
            QStringLiteral("CodeHarbor"), QStringLiteral("CodeHarbor"));
    } else {
        m_settings =
            std::make_unique<QSettings>(iniPath, QSettings::IniFormat);
    }
}

UiStateStore::~UiStateStore() = default;

void UiStateStore::setRegionWidths(int sidebar, int viewer, int terminal)
{
    // No sync() here: this is called on every width change during a handle
    // drag, and a synchronous disk write per change causes jank. QSettings
    // flushes periodically and from its own destructor, which runs when the
    // m_settings unique_ptr is released with this object, so a fresh instance
    // still reads back the last written values.
    // Keep the write side aligned with the read-side minima: a transient
    // negative or zero sidebar/terminal width must not be persisted as a value
    // that the next launch rejects and silently replaces with a different
    // default.
    const int storedSidebar = std::max(1, sidebar);
    const int storedViewer = std::max(0, viewer);
    const int storedTerminal = std::max(1, terminal);
    m_settings->setValue(kSidebarKey, storedSidebar);
    m_settings->setValue(kViewerKey, storedViewer);
    m_settings->setValue(kTerminalKey, storedTerminal);
}

int UiStateStore::sidebarWidth() const
{
    return storedInt(*m_settings, kSidebarKey, kDefaultSidebarWidth, 1);
}

int UiStateStore::viewerWidth() const
{
    return storedInt(*m_settings, kViewerKey, kDefaultViewerWidth, 0);
}

int UiStateStore::terminalWidth() const
{
    return storedInt(*m_settings, kTerminalKey, kDefaultTerminalWidth, 1);
}

void UiStateStore::setPinnedOnly(bool pinnedOnly)
{
    // Writing a value that did not change would rewrite the settings file on
    // every launch: the sidebar re-applies its filter when it is created, so an
    // unconditional write turns "the user changed nothing" into a modified
    // config on disk. That is observable - a relaunch test watches this file's
    // timestamp precisely to prove a no-op launch leaves it alone.
    if (pinnedOnly == this->pinnedOnly())
        return;
    m_settings->setValue(kPinnedOnlyKey, pinnedOnly);
    // A discrete toolbar choice, not a drag stream; flush it before returning
    // so a restart cannot reopen the sidebar in the wrong mode after the
    // process exits immediately.
    m_settings->sync();
}

bool UiStateStore::pinnedOnly() const
{
    return storedBool(*m_settings, kPinnedOnlyKey, false);
}

void UiStateStore::setShowArchived(bool showArchived)
{
    // As with pinnedOnly, avoid rewriting the settings file when the sidebar
    // reapplies its persisted value during startup, but flush a real toggle
    // before returning so an immediate restart keeps the user's choice.
    if (showArchived == this->showArchived())
        return;
    m_settings->setValue(kShowArchivedKey, showArchived);
    m_settings->sync();
}

bool UiStateStore::showArchived() const
{
    return storedBool(*m_settings, kShowArchivedKey, false);
}

void UiStateStore::setSelectedPane(QString devSessionId, QString paneId)
{
    // Same rule as setActiveSession(): with no Dev Session there is nothing for
    // a selected pane to belong to, and writing it would park the value under
    // the bare "selectedPane/" key that every other empty-id read would then
    // pick up.
    if (devSessionId.isEmpty())
        return;
    m_settings->setValue(selectedPaneKey(devSessionId), paneId);
    m_settings->sync();
}

QString UiStateStore::selectedPane(QString devSessionId) const
{
    if (devSessionId.isEmpty())
        return {};
    return m_settings->value(selectedPaneKey(devSessionId)).toString();
}

void UiStateStore::setNextPaneSuffix(QString devSessionId, QString region,
                                     int suffix)
{
    // Same empty-id rule as the two accessors above. A counter parked under
    // "paneSuffix//<region>" would be read back by every OTHER Dev Session
    // whose id has not arrived yet, so one session's numbering would leak into
    // the next one's - and a pane id collision is exactly what this counter
    // exists to prevent.
    if (devSessionId.isEmpty() || !isAddressableRegion(region))
        return;
    // The same floor nextPaneSuffix() enforces on the way out. Without it the
    // two halves disagree: a caller could store 0 or a negative, the read side
    // would silently repair it to 1, and the counter would quietly forget every
    // id already spent - which is a duplicate pane label, the one thing it
    // exists to prevent. Refusing here makes the stored state the documented
    // one rather than something every reader has to keep patching up.
    if (suffix < kFirstPaneSuffix)
        return;
    m_settings->setValue(paneSuffixKey(devSessionId, region), suffix);
    // sync() here, unlike setRegionWidths(): this is called once per pane
    // created (not once per mouse move), and a counter lost to a crash before
    // the next periodic flush would let the following launch re-mint an id the
    // layout no longer shows - the very bug this replaces.
    m_settings->sync();
}

int UiStateStore::nextPaneSuffix(QString devSessionId, QString region) const
{
    if (devSessionId.isEmpty() || !isAddressableRegion(region))
        return kFirstPaneSuffix;
    return storedInt(*m_settings, paneSuffixKey(devSessionId, region),
                     kFirstPaneSuffix, kFirstPaneSuffix);
}

void UiStateStore::setActiveSession(QString serverId, QString devSessionId)
{
    // No server, no meaningful "active session": refuse to park a value under a
    // placeholder key that the next connected server would then read back.
    if (serverId.isEmpty())
        return;
    m_settings->setValue(activeSessionKey(serverId), devSessionId);
    m_settings->sync();
}

QString UiStateStore::activeSession(QString serverId) const
{
    if (serverId.isEmpty())
        return {};
    return m_settings->value(activeSessionKey(serverId)).toString();
}

} // namespace ch
