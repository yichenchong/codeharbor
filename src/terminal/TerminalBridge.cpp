#include "TerminalBridge.h"

#include "SessionState.h"
#include "TerminalController.h"

namespace ch {

TerminalBridge::TerminalBridge(TerminalController* controller, QObject* parent)
    : QObject(parent), m_controller(controller)
{
    if (!m_controller)
        return;

    connect(m_controller, &TerminalController::flushReady, this,
            &TerminalBridge::onFlushReady);
    connect(m_controller, &TerminalController::stateChanged, this,
            [this](TerminalState state) { emit connectionStateChanged(toString(state)); });

    // No renderer until the page mounts and calls ready(): everything the pane
    // produces until then belongs in the controller's rolling buffer, not in a
    // signal nobody is connected to yet (SPEC 5.4).
    m_controller->setViewVisible(false);
}

TerminalController* TerminalBridge::controller() const
{
    return m_controller.data();
}

QString TerminalBridge::connectionState() const
{
    return toString(m_controller ? m_controller->state() : TerminalState::Unloaded);
}

bool TerminalBridge::rendererReady() const
{
    return m_rendererReady;
}

int TerminalBridge::columns() const
{
    return m_controller ? m_controller->columns() : 0;
}

int TerminalBridge::rows() const
{
    return m_controller ? m_controller->rows() : 0;
}

void TerminalBridge::requestClear()
{
    emit clearRequested();
}

void TerminalBridge::sendInput(const QString& data)
{
    if (m_controller)
        m_controller->sendInput(data.toUtf8());
}

void TerminalBridge::resize(int cols, int rows)
{
    if (!m_controller)
        return;
    const int wasColumns = m_controller->columns();
    const int wasRows = m_controller->rows();
    // Rejected sizes (a renderer that has not been laid out yet reports 0)
    // leave the recorded geometry untouched, hence the before/after compare.
    m_controller->resize(cols, rows);
    if (m_controller->columns() != wasColumns || m_controller->rows() != wasRows)
        emit geometryChanged();
}

void TerminalBridge::notifyViewVisible(bool visible)
{
    if (m_controller)
        m_controller->setViewVisible(visible);
}

void TerminalBridge::ready()
{
    m_rendererReady = true;
    if (!m_controller)
        return;
    // Re-announce the state a page that loaded late (or reloaded) missed, then
    // release the buffer: setViewVisible(true) replays everything retained
    // while the renderer was absent as one flushReady batch.
    emit connectionStateChanged(toString(m_controller->state()));
    m_controller->setViewVisible(true);
}

void TerminalBridge::onFlushReady(const QByteArray& batch)
{
    if (batch.isEmpty())
        return;
    // Stateful decode: a multi-byte sequence split across two flushes is held
    // back and completed by the next batch instead of becoming U+FFFD.
    const QString text = m_decoder.decode(batch);
    if (!text.isEmpty())
        emit write(text);
}

} // namespace ch
