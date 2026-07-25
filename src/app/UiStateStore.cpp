#include "UiStateStore.h"

#include <QSettings>

namespace ch {

namespace {
constexpr int kDefaultSidebarWidth = 260;
constexpr int kDefaultViewerWidth = 0; // 0 => fill remaining space
constexpr int kDefaultTerminalWidth = 520;

const QString kSidebarKey = QStringLiteral("layout/sidebarWidth");
const QString kViewerKey = QStringLiteral("layout/viewerWidth");
const QString kTerminalKey = QStringLiteral("layout/terminalWidth");

QString selectedPaneKey(const QString& devSessionId)
{
    return QStringLiteral("selectedPane/") + devSessionId;
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
    // flushes periodically and on destruction (see ~UiStateStore), so a fresh
    // instance still reads back the last written values.
    m_settings->setValue(kSidebarKey, sidebar);
    m_settings->setValue(kViewerKey, viewer);
    m_settings->setValue(kTerminalKey, terminal);
}

int UiStateStore::sidebarWidth() const
{
    return m_settings->value(kSidebarKey, kDefaultSidebarWidth).toInt();
}

int UiStateStore::viewerWidth() const
{
    return m_settings->value(kViewerKey, kDefaultViewerWidth).toInt();
}

int UiStateStore::terminalWidth() const
{
    return m_settings->value(kTerminalKey, kDefaultTerminalWidth).toInt();
}

void UiStateStore::setSelectedPane(QString devSessionId, QString paneId)
{
    m_settings->setValue(selectedPaneKey(devSessionId), paneId);
    m_settings->sync();
}

QString UiStateStore::selectedPane(QString devSessionId) const
{
    return m_settings->value(selectedPaneKey(devSessionId)).toString();
}

} // namespace ch
