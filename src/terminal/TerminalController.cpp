#include "TerminalController.h"

#include "SshChannelDevice.h"

#include <QIODevice>

#include <cstring>
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
enum class EscapeState {
    Normal,
    Escape,
    EscapeIntermediate,
    Csi,
    String,
    StringEscape,
};

struct EscapeScanner {
    EscapeState state = EscapeState::Normal;
    qsizetype start = -1;

    bool consume(unsigned char byte, qsizetype index)
    {
        switch (state) {
        case EscapeState::Normal:
            if (byte == 0x1b) {
                state = EscapeState::Escape;
                start = index;
            }
            return false;
        case EscapeState::Escape:
            if (byte == '[') {
                state = EscapeState::Csi;
            } else if (byte == ']' || byte == 'P' || byte == 'X' || byte == '^'
                       || byte == '_') {
                state = EscapeState::String;
            } else if (byte == 0x1b) {
                start = index;
                state = EscapeState::Escape;
            } else if (byte >= 0x20 && byte <= 0x2f) {
                // ESC Fe controls may carry one or more intermediate bytes
                // before their final byte, such as ESC ( 0.
                state = EscapeState::EscapeIntermediate;
            } else {
                state = EscapeState::Normal;
                start = -1;
                return true;
            }
            return false;
        case EscapeState::EscapeIntermediate:
            if (byte == 0x1b) {
                start = index;
                state = EscapeState::Escape;
                return false;
            }
            if (byte >= 0x20 && byte <= 0x2f)
                return false;
            state = EscapeState::Normal;
            start = -1;
            return true;
        case EscapeState::Csi:
            if (byte >= 0x40 && byte <= 0x7e) {
                state = EscapeState::Normal;
                start = -1;
                return true;
            }
            if (byte == 0x1b) {
                state = EscapeState::Escape;
                start = index;
            }
            return false;
        case EscapeState::String:
            if (byte == 0x07) {
                state = EscapeState::Normal;
                start = -1;
                return true;
            }
            if (byte == 0x1b)
                state = EscapeState::StringEscape;
            return false;
        case EscapeState::StringEscape:
            if (byte == '\\') {
                state = EscapeState::Normal;
                start = -1;
                return true;
            }
            state = byte == 0x1b ? EscapeState::StringEscape : EscapeState::String;
            return false;
        }
        return false;
    }
};

bool isUtf8Continuation(unsigned char byte)
{
    return (byte & 0xc0) == 0x80;
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
    // Whatever the outgoing renderer was handed and never acknowledged is gone
    // with it, and the incoming one never saw any of it. Starting the account
    // at zero is what lets a reloaded page resynchronise: it gets the retained
    // buffer replayed and a full window of credit, instead of inheriting a
    // debt run up by the page it replaced. (releaseRetained() is a no-op on the
    // way to hidden, which is what makes this one call cover both directions.)
    resetOutputAcknowledgements();
}

void TerminalController::acknowledgeOutput(qint64 bytes)
{
    if (bytes <= 0)
        return;
    m_unacknowledged = qMax<qint64>(0, m_unacknowledged - bytes);
    releaseRetained();
}

qint64 TerminalController::unacknowledgedBytes() const
{
    return m_unacknowledged;
}

void TerminalController::resetOutputAcknowledgements()
{
    m_unacknowledged = 0;
    releaseRetained();
}

void TerminalController::ingestOutput(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return; // empty output: nothing to buffer or flush
    // Before the coalescing below, and unconditionally: this is the liveness
    // signal, and a pane whose output is being buffered or held back is still
    // producing (see the doc comment on outputReceived).
    emit outputReceived();
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

qsizetype TerminalController::incompleteTrailingUtf8(const QByteArray &data)
{
    const qsizetype size = data.size();
    // A UTF-8 character is at most four bytes, so at most the last three can be
    // the incomplete start of one.
    const qsizetype scan = qMin<qsizetype>(3, size);
    for (qsizetype back = 1; back <= scan; ++back) {
        const auto byte = static_cast<unsigned char>(data[size - back]);
        if ((byte & 0xC0) == 0x80)
            continue; // continuation byte; the lead is further back
        qsizetype needed = 0;
        if ((byte & 0xE0) == 0xC0)
            needed = 2;
        else if ((byte & 0xF0) == 0xE0)
            needed = 3;
        else if ((byte & 0xF8) == 0xF0)
            needed = 4;
        // needed == 0 covers ASCII and every byte that is not a legal lead
        // (0xF8..0xFF). Those are not a truncated character, they are simply
        // not UTF-8, and holding them back would stall a binary stream forever.
        return back < needed ? back : 0;
    }
    // Three continuation bytes with no lead among them: either a complete
    // four-byte character or garbage. Either way nothing is pending.
    return 0;
}
qsizetype TerminalController::incompleteTrailingEscape(const QByteArray &data)
{
    EscapeScanner scanner;
    for (qsizetype index = 0; index < data.size(); ++index)
        scanner.consume(static_cast<unsigned char>(data.at(index)), index);
    if (scanner.state == EscapeState::Normal || scanner.start < 0)
        return 0;
    return data.size() - scanner.start;
}

qsizetype TerminalController::safePrefixBoundary(const QByteArray &buffer,
                                                 qsizetype maxBytes)
{
    if (maxBytes <= 0 || buffer.isEmpty())
        return 0;
    if (maxBytes >= buffer.size())
        return buffer.size();

    EscapeScanner scanner;
    qsizetype boundary = 0;
    const qsizetype limit = qMin(maxBytes, buffer.size());
    for (qsizetype index = 0; index < buffer.size(); ++index) {
        const auto byte = static_cast<unsigned char>(buffer.at(index));
        const bool completed = scanner.consume(byte, index);
        const qsizetype candidate = index + 1;
        if (candidate > limit)
            break;
        if (completed) {
            boundary = candidate;
            continue;
        }
        if (scanner.state == EscapeState::Normal
            && (candidate == buffer.size()
                || !isUtf8Continuation(
                    static_cast<unsigned char>(buffer.at(candidate)))))
            boundary = candidate;
    }
    return boundary;
}


void TerminalController::flush()
{
    m_flushTimer.stop();
    if (m_pending.isEmpty())
        return;

    // A batch must not END in the middle of a multi-byte UTF-8 character or an
    // ANSI escape sequence. The bridge can decode a split UTF-8 character
    // statefully, but an escape sequence has parser state that xterm.js cannot
    // recover if its tail is retained or evicted separately. Holding the
    // incomplete suffix here makes every emitted batch independently safe.
    const qsizetype utf8Partial = incompleteTrailingUtf8(m_pending);
    const qsizetype escapePartial = incompleteTrailingEscape(m_pending);
    const qsizetype partial = qMax(utf8Partial, escapePartial);
    if (partial == m_pending.size())
        return; // nothing but the head of a character: wait for the rest

    QByteArray batch = std::move(m_pending);
    m_pending = batch.right(partial); // empty when partial == 0
    batch.chop(partial);

    // Three reasons to retain instead of emit, and they share one buffer:
    //   * no renderer is listening (SPEC 5.4);
    //   * the renderer is too far behind on acknowledgements — emitting more
    //     would queue it in the WebChannel transport and in Chromium, which is
    //     memory nobody bounds;
    //   * something is ALREADY retained. This one is about ORDER, not volume:
    //     the retained bytes are older, so a batch emitted past them would
    //     reach the terminal out of sequence.
    if (!m_viewVisible || m_unacknowledged >= kMaxUnacknowledgedBytes
        || !m_hidden.isEmpty()) {
        appendHidden(batch);
        return;
    }

    m_unacknowledged += batch.size();
    emit flushReady(batch);
}

void TerminalController::releaseRetained()
{
    if (!m_viewVisible || m_hidden.isEmpty()
        || m_unacknowledged >= kMaxUnacknowledgedBytes)
        return;

    // Release only what the credit window still has room for. The whole buffer
    // used to go in one batch, which overshot kMaxUnacknowledgedBytes by up to
    // kHiddenBufferMaxBytes — a 2 MiB batch handed to a renderer that had
    // credit for 8 KiB, i.e. the flow control's own bound broken by the
    // mechanism that exists to enforce it.
    //
    // Nothing is DROPPED by cutting: the remainder stays retained, in order,
    // and the next acknowledgement releases the next slice. The cut must stay
    // at or before the available credit, so use safePrefixBoundary() rather
    // than the eviction helper, whose deliberate forward resynchronisation can
    // exceed its input offset to find a line feed.
    const qint64 allowance = kMaxUnacknowledgedBytes - m_unacknowledged; // > 0
    QByteArray replay;
    qsizetype cut = m_hidden.size() <= allowance
                              ? m_hidden.size()
                              : safePrefixBoundary(m_hidden, static_cast<qsizetype>(allowance));
    if (cut <= 0) {
        // A single complete escape sequence can itself be larger than the
        // remaining window. It cannot be split without corrupting the
        // renderer's parser, so release that indivisible sequence as a last
        // resort rather than deadlocking with no acknowledgement possible.
        const qsizetype indivisible =
            resyncBoundary(m_hidden, static_cast<qsizetype>(allowance));
        if (indivisible <= 0)
            return;
        cut = indivisible;
    }
    if (cut <= 0)
        return;
    if (cut >= m_hidden.size()) {
        replay = std::move(m_hidden);
        m_hidden.clear();
    } else {
        replay = m_hidden.left(cut);
        m_hidden.remove(0, cut);
    }
    m_unacknowledged += replay.size();
    emit flushReady(replay);
}
qsizetype TerminalController::resyncBoundary(const QByteArray &buffer, qsizetype from)
{
    const qsizetype limit = qMin(buffer.size(), from + kHiddenResyncWindowBytes);
    if (from >= limit)
        return from;

    EscapeScanner scanner;
    qsizetype safe = -1;
    qsizetype lineFeed = -1;

    for (qsizetype index = 0; index < buffer.size(); ++index) {
        const auto byte = static_cast<unsigned char>(buffer.at(index));
        const EscapeState before = scanner.state;
        if (index == from && before == EscapeState::Normal
            && !isUtf8Continuation(byte)) {
            safe = from;
        }
        const qsizetype sequenceStart = scanner.start;
        const bool completed = scanner.consume(byte, index);
        const qsizetype candidate = index + 1;

        if (completed && candidate >= from) {
            // A sequence that crosses the nominal resync window is still
            // emitted as a whole. The window bounds ordinary scrolling data;
            // it must not turn a long OSC title into literal terminal text.
            if (candidate <= limit || sequenceStart < from) {
                if (safe < 0)
                    safe = candidate;
                if (byte == '\n' && before == EscapeState::Normal)
                    lineFeed = candidate;
                if (candidate > limit)
                    break;
            }
            continue;
        }

        if (candidate < from || candidate > limit)
            continue;
        if (scanner.state == EscapeState::Normal
            && (candidate == buffer.size()
                || !isUtf8Continuation(
                    static_cast<unsigned char>(buffer.at(candidate))))) {
            if (safe < 0)
                safe = candidate;
            if (byte == '\n')
                lineFeed = candidate;
        }
    }

    if (lineFeed >= from)
        return lineFeed;
    if (safe >= from)
        return safe;

    // No line feed or complete escape boundary within the window. Settle for
    // the first UTF-8 boundary, retaining the old bounded eviction behavior
    // for a stream made entirely of continuation bytes.
    qsizetype fallback = from;
    while (fallback < limit
           && isUtf8Continuation(static_cast<unsigned char>(buffer.at(fallback)))) {
        ++fallback;
    }
    return fallback;
}

void TerminalController::appendHidden(const QByteArray &batch)
{
    m_hidden.append(batch);
    const qsizetype drop = m_hidden.size() - kHiddenBufferMaxBytes;
    if (drop <= 0)
        return;

    // WHERE the cut lands matters, not just how much it removes. The retained
    // buffer is replayed verbatim into the renderer when the pane becomes
    // visible again, so the first byte after the cut is the first byte
    // xterm.js's parser sees. Cutting at the raw overflow offset lands in the
    // middle of whatever byte sequence happened to straddle it:
    //
    //   * a multi-byte UTF-8 character loses its lead byte, and
    //     ch::TerminalBridge's decoder turns the orphaned continuation bytes
    //     into U+FFFD replacement glyphs at the top of the replay, and
    //   * an ANSI escape sequence loses its ESC, so its remaining bytes
    //     ("[1;31m") are printed as literal text instead of setting a colour.
    //
    // Both are visible corruption in a pane the user only hid for a while.
    // Move the cut forward to a point that cannot be inside either sequence,
    // dropping a few more bytes of the oldest scrollback than strictly needed
    // (tmux's own history covers anything older anyway, SPEC 5.4).
    m_hidden.remove(0, resyncBoundary(m_hidden, drop));
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
    // are terminal for this purpose (SPEC 5.6).
    switch (state) {
    case TerminalState::OpeningChannel:
    case TerminalState::AttachingTmux:
    case TerminalState::Ready:
        return true;
    case TerminalState::Unloaded:
    case TerminalState::Disconnected:
    case TerminalState::Error:
        return false;
    }
    return false;
}

QString TerminalController::tmuxNewSessionCommand(const QString &target,
                                                  const QString &workingDir)
{
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

} // namespace ch
