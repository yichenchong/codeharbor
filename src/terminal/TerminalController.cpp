#include "TerminalController.h"

#include "SshChannelDevice.h"

#include <QIODevice>

#include <utility>

namespace {
// POSIX single-quote a value for safe interpolation into a shell command: wrap
// it in single quotes and rewrite every embedded quote as the '\'' sequence, so
// no quote, space, or metacharacter in a working directory or id can break out
// of the quoting (SPEC 5.2). Without this a workingDir like /a'; rm -rf ~; ' or
// an id carrying a quote would inject shell.
QString shellSingleQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}
} // namespace

namespace ch {

TerminalController::TerminalController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<TerminalState>("ch::TerminalState");
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(kFlushIntervalMs);
    connect(&m_flushTimer, &QTimer::timeout, this, &TerminalController::flush);
    m_attachTimer.setSingleShot(true);
    connect(&m_attachTimer, &QTimer::timeout, this,
            &TerminalController::onAttachTimeout);
}

TerminalState TerminalController::state() const
{
    return m_state;
}

void TerminalController::setAttachTimeoutMs(int ms)
{
    m_attachTimeoutMs = qMax(0, ms);
    // A pane that is attaching right now is measured against the NEW window,
    // restarted from this moment: leaving the old interval running would make
    // the setter silently ineffective for the one pane it was called about.
    if (m_attachTimer.isActive() || m_state == TerminalState::OpeningChannel
        || m_state == TerminalState::AttachingTmux) {
        m_attachTimer.stop();
        if (m_attachTimeoutMs > 0)
            m_attachTimer.start(m_attachTimeoutMs);
    }
}

int TerminalController::attachTimeoutMs() const
{
    return m_attachTimeoutMs;
}

bool TerminalController::viewVisible() const
{
    return m_viewVisible;
}

void TerminalController::setViewVisible(bool visible)
{
    if (m_viewVisible == visible)
        return;
    m_viewVisible = visible;
    if (visible && !m_hidden.isEmpty()) {
        // Replay everything retained while hidden as one batch, then reset the
        // rolling buffer so tmux history covers anything older (SPEC 5.4/5.5).
        const QByteArray replay = std::move(m_hidden);
        m_hidden.clear();
        emit flushReady(replay);
    }
}

void TerminalController::ingestOutput(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return; // empty output: nothing to buffer or flush
    m_pending.append(bytes);
    if (m_pending.size() >= kFlushSizeBytes) {
        flush(); // size threshold reached first
    } else if (!m_flushTimer.isActive()) {
        m_flushTimer.start(); // arm the time threshold from the first buffered byte
    }
}

void TerminalController::setTransport(QIODevice *transport)
{
    if (m_transport == transport)
        return;

    if (m_transport)
        m_transport->disconnect(this);

    m_transport = transport;
    // m_pending/m_hidden survive on purpose: a reconnect swaps the channel
    // underneath the same pane and must not drop buffered scrollback.

    if (!m_transport)
        return;

    connect(m_transport, &QIODevice::readyRead, this,
            &TerminalController::onTransportReadyRead);
    connect(m_transport, &QIODevice::readChannelFinished, this,
            &TerminalController::onTransportFinished);

    // A reconnect opens a fresh PTY at the channel's default size; re-assert the
    // geometry the renderer last reported so the pane does not snap back.
    if (m_columns > 0 && m_rows > 0)
        applyPtySize(m_columns, m_rows);

    // Drain whatever the transport buffered before we subscribed.
    if (m_transport->bytesAvailable() > 0)
        onTransportReadyRead();
}

QIODevice *TerminalController::transport() const
{
    return m_transport.data();
}

void TerminalController::onTransportReadyRead()
{
    // isReadable(), not just non-null: closeChannel() fires readChannelFinished()
    // on an already-closed device, and QIODevice::readAll() on one is a warning
    // plus a guaranteed empty result.
    if (!m_transport || !m_transport->isReadable())
        return;
    ingestOutput(m_transport->readAll());
}

void TerminalController::onTransportFinished()
{
    // The final payload precedes readChannelFinished(); claim it before
    // reporting the drop so the last remote bytes still reach the pane.
    onTransportReadyRead();

    // A channel that ends under a live pane is a dropped connection (SPEC 5.6).
    // Only live states transition: an already Disconnected/Error pane must not
    // be walked backwards, and a pane that never came up keeps its state.
    if (isLiveState(m_state))
        setState(TerminalState::Disconnected);
}

bool TerminalController::sendInput(const QByteArray &bytes)
{
    if (!m_transport || !m_transport->isOpen() || !m_transport->isWritable())
        return false;
    if (bytes.isEmpty())
        return true; // nothing to send, but the pane is writable
    return m_transport->write(bytes) == bytes.size();
}

bool TerminalController::resize(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return false;
    m_columns = cols;
    m_rows = rows;
    return applyPtySize(cols, rows);
}

int TerminalController::columns() const
{
    return m_columns;
}

int TerminalController::rows() const
{
    return m_rows;
}

bool TerminalController::applyPtySize(int cols, int rows)
{
    // The seam is a plain QIODevice, but a window-change is not a byte stream:
    // it is an out-of-band SSH channel request. Narrow to the one transport
    // that can carry it; every other transport records the size and no more.
    if (auto *pty = qobject_cast<SshChannelDevice *>(m_transport.data()))
        return pty->resizePty(cols, rows);
    return false;
}

const QByteArray &TerminalController::hiddenBuffer() const
{
    return m_hidden;
}

void TerminalController::flush()
{
    m_flushTimer.stop();
    if (m_pending.isEmpty())
        return;
    const QByteArray batch = std::move(m_pending);
    m_pending.clear();
    if (m_viewVisible)
        emit flushReady(batch);
    else
        appendHidden(batch);
}

void TerminalController::appendHidden(const QByteArray &batch)
{
    m_hidden.append(batch);
    const qsizetype overflow = m_hidden.size() - kHiddenBufferMaxBytes;
    if (overflow > 0)
        m_hidden.remove(0, overflow); // evict oldest bytes past the cap
}

void TerminalController::setState(TerminalState next)
{
    if (m_state == next)
        return;
    m_state = next;
    // Arm the attach watchdog on the way INTO an attaching state and disarm it
    // on the way out. Both attaching states restart the window, because each of
    // them is progress: a slow channel open must not eat the budget tmux gets to
    // draw its first screenful. Every other state is an outcome — Ready means the
    // pane came up, Disconnected/Error mean it will not — so the clock stops.
    if (next == TerminalState::OpeningChannel || next == TerminalState::AttachingTmux) {
        if (m_attachTimeoutMs > 0)
            m_attachTimer.start(m_attachTimeoutMs);
    } else {
        m_attachTimer.stop();
    }
    emit stateChanged(m_state);
}

void TerminalController::onAttachTimeout()
{
    // Re-check the state rather than trusting the timer: a timeout already
    // queued in the event loop can still be delivered after setState() stopped
    // the timer, and turning a pane that just came up into an Error would be
    // strictly worse than the stall this guards against.
    if (m_state != TerminalState::OpeningChannel
        && m_state != TerminalState::AttachingTmux)
        return;

    // Error, not Disconnected: nothing was ever established, so there is nothing
    // for an automatic reconnect ladder to resume — the attach itself has to be
    // retried, which is what the pane's Retry action does. setState() emits the
    // transition and stops the timer; the signal only carries the reason out to
    // whoever writes the pane's message (TerminalFactory).
    setState(TerminalState::Error);
    emit attachTimedOut();
}

bool TerminalController::isLiveState(TerminalState state)
{
    // A pane only has a channel to lose from the moment the channel is being
    // opened until it ends. Unloaded (never attached), Disconnected and Error
    // are terminal for this purpose; Connecting/Authenticating/Reconnecting
    // describe the session, not this pane's channel, so a channel end must not
    // overwrite them either (SPEC 5.6).
    switch (state) {
    case TerminalState::OpeningChannel:
    case TerminalState::AttachingTmux:
    case TerminalState::Ready:
        return true;
    case TerminalState::Unloaded:
    case TerminalState::Connecting:
    case TerminalState::Authenticating:
    case TerminalState::Disconnected:
    case TerminalState::Reconnecting:
    case TerminalState::Error:
        return false;
    }
    return false;
}

QString TerminalController::tmuxTarget(const DevSessionId &devSession,
                                       const TerminalId &terminal)
{
    return QStringLiteral("ch_%1_%2").arg(devSession.value, terminal.value);
}

QString TerminalController::tmuxNewSessionCommand(const DevSessionId &devSession,
                                                  const TerminalId &terminal,
                                                  const QString &workingDir)
{
    const QString target = tmuxTarget(devSession, terminal);

    // TWO tmux commands in ONE invocation, separated by an escaped semicolon:
    // the shell unescapes `\;` into a literal `;` argument, which is tmux's own
    // command separator (a bare `;` would end the shell command instead).
    //
    // The second command exists to fix the mouse wheel. tmux runs on the
    // terminal's ALTERNATE screen, and an alternate screen has no scrollback of
    // its own, so xterm.js falls back to "alternate scroll": with no application
    // asking for mouse events it translates each wheel notch into a cursor-up /
    // cursor-down key press (verified in @xterm/xterm's wheel handler, which
    // sends ESC [ A / ESC [ B when `buffer.hasScrollback` is false). Those keys
    // reach whatever runs inside tmux, so a wheel turn walked back through shell
    // history instead of scrolling. tmux DOES own a scrollback; it just never
    // sees the wheel until mouse reporting is on. Switching it on makes the
    // wheel a mouse event, which tmux turns into its own copy-mode scroll.
    //
    // The option is set on THIS SESSION ONLY. `set -g mouse on` would be wrong:
    // this command carries no `-L`/`-S`, so the session lives on the user's
    // default tmux server, and a global option would silently change the
    // behaviour of every tmux session that user started by hand.
    //
    // set-option's `-t` is a target-PANE expression, so the session is named as
    // the window target `<session>:`; and, exactly as in
    // TerminalFactory::tmuxKillSessionCommand(), tmux's exact-match `=` sigil
    // goes INSIDE the quotes because it is tmux syntax, not shell syntax —
    // without it a target such as `ch_*_t1:` resolves by fnmatch and would
    // reconfigure somebody else's session. Verified against tmux 3.6: with
    // `ch_victim_t1` live, `set-option -t 'ch_*_t1:' mouse on` set the option on
    // it, while `-t '=ch_*_t1:'` refused with "no such session" (SPEC 5.2).
    return QStringLiteral("tmux new-session -A -s %1 -c %2 \\; set-option -t %3 mouse on")
        .arg(shellSingleQuote(target), shellSingleQuote(workingDir),
             shellSingleQuote(QLatin1Char('=') + target + QLatin1Char(':')));
}

int TerminalController::reconnectDelaySeconds(int attempt)
{
    static constexpr int schedule[] = {1, 2, 5, 10, 30};
    constexpr int count = static_cast<int>(sizeof(schedule) / sizeof(schedule[0]));
    if (attempt < 0)
        return schedule[0];
    if (attempt < count)
        return schedule[attempt];
    return 60;
}

} // namespace ch
