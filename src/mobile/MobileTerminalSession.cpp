#include "MobileTerminalSession.h"

#include <QClipboard>
#include <QGuiApplication>

#include "TerminalController.h"
#include "TerminalFactory.h"
#include "VtKeys.h"
#include "VtScreen.h"

namespace ch {

MobileTerminalSession::MobileTerminalSession(TerminalFactory *factory, QObject *parent)
    : QObject(parent)
    , m_factory(factory)
{
    // The screen exists BEFORE the controller can produce anything, which is
    // what lets this class acknowledge synchronously and skip the bridge's
    // mount handshake entirely.
    m_screen = new VtScreen(this);

    // ANSWERBACK. Some sequences are QUESTIONS: a Device Status Report or a
    // primary Device Attributes request expects the terminal to answer, and a
    // program that asked (readline probing the cursor position, `tput`, an
    // editor detecting the terminal type) blocks or misdraws until it does. The
    // desktop path answers because xterm.js answers; here the screen's parser
    // hands the bytes over and they go straight back into the PTY, in the order
    // they were produced — ch::VtScreen emits reply() INLINE for exactly that
    // reason, so an answer cannot be reordered behind the user's keystrokes.
    connect(m_screen, &VtScreen::reply, this, [this](const QByteArray &bytes) {
        if (m_controller && !bytes.isEmpty())
            m_controller->sendInput(bytes);
    });

    // Parented to this session so the controller, its rolling buffer, its timers
    // and its channel are released when the page that owns the session goes
    // away. The factory's own create() does the parenting; a session built
    // without a factory still has a controller so QML bindings on state and
    // geometry stay valid on inert chrome.
    m_controller = m_factory ? m_factory->create(this) : new TerminalController(this);

    connect(m_controller, &TerminalController::flushReady,
            this, &MobileTerminalSession::onFlushReady);
    connect(m_controller, &TerminalController::stateChanged,
            this, &MobileTerminalSession::onStateChanged);
    connect(m_controller, &TerminalController::attachTimedOut, this, [this]() {
        // The controller has already moved the pane to Error; it has no message
        // channel of its own, so the sentence the user reads is written here —
        // the same division of labour ch::TerminalFactory::error() has on the
        // desktop.
        setStatusText(tr("This terminal did not finish attaching. Tap Reattach to try again."));
    });

    if (m_factory) {
        connect(m_factory, &TerminalFactory::targetResolved,
                this, &MobileTerminalSession::onTargetResolved);
        connect(m_factory, &TerminalFactory::error,
                this, &MobileTerminalSession::onFactoryError);
    } else {
        setStatusText(tr("No terminal service in this window."));
    }
}

VtScreen *MobileTerminalSession::screen() const
{
    return m_screen;
}

TerminalState MobileTerminalSession::state() const
{
    return m_controller ? m_controller->state() : TerminalState::Unloaded;
}

QString MobileTerminalSession::statusText() const
{
    return m_statusText;
}

TerminalController *MobileTerminalSession::controller() const
{
    return m_controller;
}

void MobileTerminalSession::onFlushReady(const QByteArray &batch)
{
    if (batch.isEmpty())
        return;
    // ORDER AND UNIT both matter.
    //
    // The screen parses the batch synchronously and keeps whatever it could not
    // finish (a split UTF-8 character, a truncated escape sequence) inside its
    // own parser state, so from the controller's point of view every byte handed
    // over IS consumed the moment write() returns. The acknowledgement is
    // therefore issued right here and carries batch.size() — the exact PTY byte
    // count the controller charged against kMaxUnacknowledgedBytes.
    //
    // NOT the decoded character count, and not the number of cells touched: a
    // batch of UTF-8 text decodes to fewer characters than it has bytes, a batch
    // cut mid-character decodes to fewer still, and a batch of escape sequences
    // credit on every flush until the window is exhausted and the pane silently
    // stops receiving output while the remote process is still printing.
    //
    // THE ORDER IS LOAD-BEARING TOO, and not merely tidy: acknowledgeOutput()
    // releases whatever the credit window was holding back, SYNCHRONOUSLY, by
    // emitting flushReady() again and re-entering this function. Acknowledging
    // before the write would therefore hand the screen the NEXT batch first and
    // interleave the stream, which in a VT stream is not a cosmetic problem: half
    // an escape sequence parsed after the bytes that follow it paints garbage.
    m_screen->write(batch);
    m_controller->acknowledgeOutput(batch.size());
}

void MobileTerminalSession::open(const QString &devSessionId,
                                 const QString &paneId,
                                 const QString &terminalPaneId,
                                 const QString &workingDir,
                                 int cols,
                                 int rows)
{
    // A session is created per page, so in practice it is opened on ONE pane for
    // its whole life. Re-opening it on a different pane is still handled rather
    // than trusted: a remembered tmux target belongs to the pane it was resolved
    // for, and re-using it for another one would attach this page to somebody
    // else's shell.
    const bool identityChanged = devSessionId != m_devSessionId
        || paneId != m_paneId
        || terminalPaneId != m_terminalPaneId;
    if (identityChanged) {
        m_tmuxTarget.clear();
        m_attached = false;
    }
    m_devSessionId = devSessionId;
    m_paneId = paneId;
    m_terminalPaneId = terminalPaneId;
    m_workingDir = workingDir;

    // Record the view's geometry without pushing a window-change: there is no
    // PTY yet, and the factory reads these when it opens one.
    if (cols > 0 && rows > 0) {
        m_columns = qMin(cols, kMaxColumns);
        m_rows = qMin(rows, kMaxRows);
        m_screen->resize(m_columns, m_rows);
    }

    if (!m_factory) {
        setStatusText(tr("No terminal service in this window."));
        return;
    }
    if (m_attaching || m_attached)
        return;
    // A resolution already in flight FOR THIS PANE is joined rather than
    // duplicated. A pane this session has since been re-pointed at is a different
    // question and gets asked below; see m_resolvingIdentity for what the answer
    // to the abandoned one can and cannot be prevented from doing.
    const QString identity = identityOf(m_devSessionId, m_paneId, m_terminalPaneId);
    if (!m_resolvingIdentity.isEmpty() && m_resolvingIdentity == identity)
        return;
    if (m_devSessionId.isEmpty() || m_paneId.isEmpty()) {
        setStatusText(tr("No terminal pane selected."));
        return;
    }
    if (!m_factory->connected()) {
        setStatusText(tr("Not connected to a server."));
        return;
    }
    if (!m_tmuxTarget.isEmpty()) {
        // Already resolved once — a retry, or a reconnect into the same pane.
        attachNow();
        return;
    }
    // Phase one. The identity is recorded BEFORE the call, not from its return
    // value: the factory documents its answers as posted rather than delivered
    // inline precisely so a caller can finish assigning its own state first, and
    // a caller that depends on that ordering should not also be the thing that
    // breaks if it ever changes.
    m_resolvingIdentity = identity;
    if (!m_factory->resolveTarget(m_controller, m_devSessionId, m_paneId,
                                  m_terminalPaneId, m_workingDir)) {
        // Refused outright; error() has already written the reason.
        m_resolvingIdentity.clear();
        return;
    }
    setStatusText(tr("Finding this terminal on the server\u2026"));
}

void MobileTerminalSession::onTargetResolved(TerminalController *controller,
                                             const QString &target)
{
    // The factory serves every pane in the window; answers are tagged with the
    // controller they belong to. That tag is not enough on its own: an answer can
    // outlive the request, and one that arrives when this session is waiting for
    // nothing — because close() released the pane — must not attach. See
    // m_resolvingIdentity.
    if (controller != m_controller)
        return;
    if (m_resolvingIdentity.isEmpty()
        || m_resolvingIdentity != identityOf(m_devSessionId, m_paneId, m_terminalPaneId)) {
        return;
    }
    m_resolvingIdentity.clear();
    if (target.isEmpty()) {
        // The resolution failed and error() carried the reason immediately
        // before this, so whatever onFactoryError() wrote is the better
        // sentence. Only a resolution that somehow reported no reason at all
        // gets the generic one.
        if (m_statusText.isEmpty()
            || m_statusText == tr("Finding this terminal on the server\u2026")) {
            setStatusText(tr("This terminal could not be found on the server."));
        }
        return;
    }
    m_tmuxTarget = target;
    attachNow();
}

void MobileTerminalSession::onFactoryError(TerminalController *controller,
                                           const QString &message)
{
    if (controller != m_controller || message.isEmpty())
        return;
    setStatusText(message);
}

void MobileTerminalSession::attachNow()
{
    if (!m_factory || !m_controller || m_attaching || m_attached || m_tmuxTarget.isEmpty())
        return;

    m_attaching = true;
    const bool attached = m_factory->attach(m_controller, m_tmuxTarget, m_workingDir,
                                            m_columns, m_rows);
    m_attaching = false;
    m_attached = attached;
    if (attached) {
        setStatusText(QString());
        return;
    }
    // REFUSED. The factory only lets a pane attach a target IT resolved for THIS
    // controller, and it drops that authorization when the pane's remote session
    // is killed or the client is pointed at another server. The remembered
    // target outlives the authorization, so it is forgotten here — otherwise
    // every later open() would take the "already resolved" shortcut above and
    // re-offer the same dead name for ever, and the pane could never recover.
    m_tmuxTarget.clear();
    if (m_statusText.isEmpty())
        setStatusText(tr("This terminal could not be attached."));
}

void MobileTerminalSession::onStateChanged(TerminalState state)
{
    switch (state) {
    case TerminalState::Ready:
        setStatusText(QString());
        break;
    case TerminalState::Disconnected:
        // The channel ended. The remote tmux session is untouched, so a Reattach
        // (or the next time this page becomes current) resumes the same shell —
        // hence m_attached is cleared while m_tmuxTarget is kept.
        m_attached = false;
        setStatusText(tr("Disconnected from this terminal. Tap Reattach to resume it."));
        break;
    case TerminalState::Error:
        m_attached = false;
        if (m_statusText.isEmpty())
            setStatusText(tr("This terminal could not be opened."));
        break;
    case TerminalState::Unloaded:
    case TerminalState::OpeningChannel:
    case TerminalState::AttachingTmux:
        break;
    }
    emit stateChanged();
}

void MobileTerminalSession::sendKey(int key, int modifiers, const QString &text)
{
    if (!m_controller)
        return;
    // The cursor-key mode is read from the SCREEN, not remembered here: it is
    // the remote program that switches it (DECCKM), the screen's parser is what
    // sees that happen, and an arrow encoded in the wrong mode types a literal
    // "OA"/"[A" into whatever is reading.
    const QByteArray bytes = vt::encodeKey(key, Qt::KeyboardModifiers(modifiers), text,
                                           m_screen && m_screen->applicationCursorKeys());
    if (!bytes.isEmpty())
        m_controller->sendInput(bytes);
}

void MobileTerminalSession::sendText(const QString &text)
{
    if (!m_controller || text.isEmpty())
        return;
    m_controller->sendInput(text.toUtf8());
}

void MobileTerminalSession::paste(const QString &text)
{
    if (!m_controller || text.isEmpty())
        return;
    // Guarded exactly as sendKey() guards its mode read: both are the same
    // "encode against the screen's live mode" pattern, and one of the two
    // spelling it defensively while the other dereferences was an invitation to
    // "fix" the wrong one.
    m_controller->sendInput(vt::encodePaste(text, m_screen && m_screen->bracketedPaste()));
}

bool MobileTerminalSession::sendMouseWheel(int notches, int column, int row)
{
    if (!m_controller || !m_screen || notches == 0)
        return false;

    // The screen decides, not a "mouse is on" boolean: 1006 selects an encoding
    // and is independent of the modes that ask for events, so this must read the
    // resolved form. None means the program wants no mouse events, and the
    // caller keeps its own scrollback behaviour.
    const VtMouseEncoding encoding = m_screen->mouseEncoding();
    if (encoding == VtMouseEncoding::None)
        return false;

    // One report per notch. A wheel has no "distance" field: three notches is
    // three events, which is exactly how tmux advances its copy-mode scroll.
    const bool up = notches > 0;
    const int count = qMin(qAbs(notches), kMaxWheelNotchesPerGesture);
    QByteArray payload;
    for (int sent = 0; sent < count; ++sent)
        payload += vt::encodeMouseWheel(up, column, row, encoding);
    if (payload.isEmpty())
        return false;

    // One write, not one per notch: these go through the same flow-controlled
    // channel as typing, and a flick can produce a dozen notches at once.
    m_controller->sendInput(payload);
    return true;
}

void MobileTerminalSession::resize(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return;
    const int columns = qMin(cols, kMaxColumns);
    const int lines = qMin(rows, kMaxRows);
    if (columns == m_columns && lines == m_rows)
        return;
    m_columns = columns;
    m_rows = lines;
    // Both halves, always in this order: reflow the local grid first so the next
    // batch is parsed against the size the remote is about to be told about.
    m_screen->resize(columns, lines);
    if (m_controller)
        m_controller->resize(columns, lines);
}

void MobileTerminalSession::setVisible(bool visible)
{
    if (m_controller)
        m_controller->setViewVisible(visible);
}

void MobileTerminalSession::close()
{
    if (!m_controller)
        return;
    // Stop flushing first: detaching walks the controller's state machine, and
    // there is no reason to hand a final batch to a screen whose page is being
    // popped.
    m_controller->setViewVisible(false);
    if (m_factory)
        m_factory->detach(m_controller);
    m_attached = false;
    // An in-flight resolution is ABANDONED, not left to land. Its answer would
    // otherwise reach attachNow() and open a fresh PTY channel behind a pane the
    // user has just left, undoing the detach above with nothing on screen to say
    // so. Nothing is lost by dropping it: a later open() re-asks, and the factory
    // joins the flight already under way (or answers from its own cache).
    m_resolvingIdentity.clear();
}

void MobileTerminalSession::copyToClipboard(const QString &text)
{
    if (text.isEmpty())
        return;
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

void MobileTerminalSession::pasteFromClipboard()
{
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        paste(clipboard->text());
}

void MobileTerminalSession::setStatusText(const QString &text)
{
    if (m_statusText == text)
        return;
    m_statusText = text;
    emit statusTextChanged();
}

QString MobileTerminalSession::identityOf(const QString &devSessionId, const QString &paneId,
                                          const QString &terminalPaneId)
{
    return devSessionId + QChar(u'\x1f') + paneId + QChar(u'\x1f') + terminalPaneId;
}

} // namespace ch
