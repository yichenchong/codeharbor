#include "TerminalBridge.h"

#include "SessionState.h"
#include "TerminalController.h"

#include <QByteArray>

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
    connect(m_controller, &TerminalController::transportChanged, this, [this]() {
        ++m_inputGeneration;
        const QPointer<TerminalBridge> self(this);
        emit inputGenerationChanged();
        if (self)
            emit inputTransportChanged(m_inputGeneration);
    });

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

quint64 TerminalBridge::inputGeneration() const
{
    return m_inputGeneration;
}

void TerminalBridge::requestClear()
{
    emit clearRequested();
}

bool TerminalBridge::sendInput(const QString& data, quint64 generation)
{
    if (!m_controller)
        return false;
    const QPointer<TerminalBridge> self(this);
    if (generation == m_inputGeneration
        && (data.isEmpty() || m_controller->sendInput(data.toUtf8())))
        return true;
    // A stale chunk belongs to the preceding PTY. A partial write may already
    // have reached that program; neither case may replay its suffix elsewhere.
    if (self)
        emit inputRejected();
    return false;
}

bool TerminalBridge::sendBinaryInput(const QString& base64, quint64 generation)
{
    if (!m_controller)
        return false;
    if (generation != m_inputGeneration) {
        emit inputRejected();
        return false;
    }
    // The page's btoa() encoder produces padded four-character groups. Qt's
    // abort flag can still accept a dangling sextet (for example "A") as an
    // empty successful decode, so validate the encoded byte count as well.
    if (base64.size() % 4 != 0)
        return false;
    qsizetype padding = 0;
    if (base64.endsWith(QLatin1Char('='))) {
        padding = 1;
        if (base64.size() >= 2 && base64.at(base64.size() - 2) == QLatin1Char('='))
            padding = 2;
    }
    const qsizetype expectedBytes = base64.size() / 4 * 3 - padding;
    // STRICT decoding, with the abort flag rather than Qt's default tolerance.
    // Base64's alphabet is entirely ASCII, so toLatin1() is lossless for every
    // legitimate payload while anything outside it (a page sending UTF-16 text
    // by mistake, or an injected value) becomes '?' — which is not in the
    // alphabet either and is therefore rejected by the same check. Without
    // AbortOnBase64DecodingErrors Qt would silently skip the offending
    // characters and hand back a shorter byte string, which is exactly the
    // "write garbage to the shell" outcome this slot must not produce.
    const QByteArray::FromBase64Result decoded =
        QByteArray::fromBase64Encoding(base64.toLatin1(),
                                       QByteArray::Base64Encoding
                                           | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() != expectedBytes)
        return false;
    const QPointer<TerminalBridge> self(this);
    if (decoded.decoded.isEmpty() || m_controller->sendInput(decoded.decoded))
        return true;
    if (self)
        emit inputRejected();
    return false;
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

void TerminalBridge::notifyOutputConsumed(int bytes)
{
    if (m_controller)
        m_controller->acknowledgeOutput(bytes);
}

void TerminalBridge::ready(bool visible)
{
    m_rendererReady = true;
    m_viewVisible = visible;
    if (!m_controller)
        return;
    // Reset while hidden: resetting the old renderer's credit while still
    // visible can replay retained output before the incoming page's hidden
    // visibility takes effect. The incoming page explicitly owns visibility,
    // so no outgoing-page report or replacement heuristic can override it.
    m_controller->setViewVisible(false);
    m_decoder.resetState();
    m_undeliveredBytes = 0;
    m_controller->resetOutputAcknowledgements();
    const QPointer<TerminalBridge> self(this);
    emit inputTransportChanged(m_inputGeneration);
    if (!self || !m_controller)
        return;
    emit connectionStateChanged(toString(m_controller->state()));
    if (self)
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
