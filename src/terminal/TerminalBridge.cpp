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
    // signal nobody is connected to yet (SPEC 5.4). Routed through
    // applyVisibility() rather than a bare setViewVisible(false) so there is
    // exactly one place that decides the controller's visibility.
    applyVisibility();
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
    // The controller reports a refused write (no channel, or a closed one), but
    // there is nowhere to send that: the page-side contract is
    // `sendInput(data): void` and it is frozen. Dropping the keystroke is the
    // right outcome anyway — the pane's status strip is already showing
    // "disconnected" or "error", which is what the user needs to see, and
    // buffering keystrokes to replay into a future shell would type a command
    // the user typed minutes ago at whatever prompt happens to be there.
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
    //
    // SECURITY: cols/rows arrive from the PAGE. A renderer that has been taken
    // over (or simply broken) could ask for 2^31-1 columns, and the value is
    // not merely recorded — it becomes an SSH window-change request, and tmux
    // on the far side sizes its grid from it. Only the UPPER end is clamped:
    // a non-positive value must stay non-positive so the controller keeps
    // rejecting it outright (clamping 0 up to 1 would resize a live PTY to a
    // single cell every time an unmounted renderer reported its size).
    m_controller->resize(cols > kMaxDimension ? kMaxDimension : cols,
                         rows > kMaxDimension ? kMaxDimension : rows);
    if (m_controller->columns() != wasColumns || m_controller->rows() != wasRows)
        emit geometryChanged();
}

void TerminalBridge::notifyViewVisible(bool visible)
{
    m_viewVisible = visible;
    applyVisibility();
}

void TerminalBridge::ready()
{
    m_rendererReady = true;
    if (!m_controller)
        return;
    // Re-announce the state a page that loaded late (or reloaded) missed, then
    // release the buffer: becoming visible replays everything retained while
    // the renderer was absent as one flushReady batch.
    emit connectionStateChanged(toString(m_controller->state()));
    // The handshake IS a visibility report: the page says it has mounted and
    // wired up host.write(). It has to reset m_viewVisible rather than merely
    // re-apply it, because the PREVIOUS page reported hidden on its way out
    // (TerminalHost.dispose() in src/web/terminal/src/index.ts), and a reload
    // would otherwise leave the pane retaining output forever behind a page
    // that is very much on screen. The page's own observers correct this within
    // a frame if the pane really is hidden.
    m_viewVisible = true;
    applyVisibility();
}

void TerminalBridge::applyVisibility()
{
    if (m_controller)
        m_controller->setViewVisible(m_viewVisible && m_rendererReady);
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
