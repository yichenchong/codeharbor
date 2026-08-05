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

TerminalBridge::~TerminalBridge()
{
    // The JavaScript page normally reports hidden during dispose(), but the
    // bridge can also be destroyed by the QML pane while a page is wedged or
    // navigating. Retain later output instead of leaving the controller
    // emitting into a bridge that no longer exists.
    if (m_controller)
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
    m_visibilityReported = true;
    m_viewVisible = visible;
    applyVisibility();
}

void TerminalBridge::notifyOutputConsumed(int bytes)
{
    if (m_controller)
        m_controller->acknowledgeOutput(bytes);
}

void TerminalBridge::ready()
{
    const bool replacingRenderer = m_rendererReady;
    m_rendererReady = true;
    if (!m_controller)
        return;
    // Re-announce the state a page that loaded late (or reloaded) missed, then
    // release the buffer: becoming visible replays everything retained while
    // the renderer was absent as one flushReady batch.
    emit connectionStateChanged(toString(m_controller->state()));
    // The handshake IS a visibility report for a replacement page: it has to
    // reset m_viewVisible rather than merely re-apply it, because the PREVIOUS
    // page reported hidden on its way out (TerminalHost.dispose() in
    // src/web/terminal/src/index.ts), and a reload would otherwise leave the
    // pane retaining output forever behind a page that is very much on screen.
    // On the first mount, however, preserve a QML/page report that arrived
    // before ready(); this covers a pane hidden while Chromium is still
    // loading. Hosts that never report visibility retain the historical
    // visible default.
    if (replacingRenderer || !m_visibilityReported)
        m_viewVisible = true;
    // A fresh renderer has consumed nothing and is owed nothing. Both sides of
    // the account are cleared explicitly rather than left to the visibility
    // change, because a page that vanished without reporting hidden (a crash, a
    // navigation the pagehide handler did not survive) leaves the controller
    // already visible — so its replacement's handshake is not a CHANGE, and the
    // new renderer would inherit a debt it can never pay off.
    //
    // The DECODER is part of that account. It can be holding the lead bytes of
    // a character whose tail went to the page that is being replaced; those
    // bytes were emitted, so they are not in the controller's retained buffer
    // and will never arrive. Left in place, that half-character would be
    // completed from the first bytes of the replay and paint one wrong glyph
    // at the top of the new renderer's screen.
    m_decoder.resetState();
    m_undeliveredBytes = 0;
    m_controller->resetOutputAcknowledgements();
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
    // Every byte the controller emitted is charged against the credit it gave
    // us, whether or not this batch decodes to anything, so the weight is
    // accumulated BEFORE the decode and carried forward when it does not.
    m_undeliveredBytes += static_cast<int>(batch.size());
    // Stateful decode: a multi-byte sequence split across two flushes is held
    // back and completed by the next batch instead of becoming U+FFFD.
    const QString text = m_decoder.decode(batch);
    if (text.isEmpty())
        return;
    const int bytes = m_undeliveredBytes;
    m_undeliveredBytes = 0;
    emit write(text, bytes);
}

} // namespace ch
