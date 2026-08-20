#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QQmlEngine>

#include "SessionState.h"
// VtScreen must be COMPLETE, not forward-declared: it is the type of a
// Q_PROPERTY, and moc's generated metatype registration static_asserts that a
// pointer property points at a fully defined type (or is declared opaque). The
// same rule bites QIODevice in ch::TerminalController — see the note there.
#include "VtScreen.h"

namespace ch {

class TerminalController;
class TerminalFactory;

// One terminal pane on a mobile client (SPEC 5.1-5.6), and the mobile
// replacement for ch::TerminalBridge.
//
// The desktop pane renders a terminal with xterm.js inside a WebEngineView, so
// ch::TerminalBridge exists to translate the C++ controller into the JS
// TerminalBridge/TerminalHost contract that page speaks. Neither Qt WebEngine
// nor Qt WebChannel exists on Android or iOS, so there is no page, no channel
// and no bridge here: this class owns the controller AND the ch::VtScreen that
// interprets its bytes, in-process, and QML draws that screen with
// ch::MobileTerminalView. TerminalBridge is deliberately never constructed.
//
// WHAT IS PRESERVED FROM TerminalBridge, because it is not decoration:
//
//   * FLOW CONTROL, byte for byte. ch::TerminalController runs at most
//     kMaxUnacknowledgedBytes ahead of its renderer and retains the rest, so
//     the renderer must report what it consumed. The page could not count that
//     in the controller's unit (it holds decoded text, not PTY bytes), which is
//     why the bridge ships a byte weight with every batch and the page echoes
//     it back. Here the accounting is exact by construction: ch::VtScreen
//     consumes the batch SYNCHRONOUSLY inside the flushReady handler, so the
//     acknowledgement is the batch's own size() and is issued immediately.
//     Acknowledging decoded characters instead would leak credit on every
//     multi-byte glyph and on any output that is not valid UTF-8, and the pane
//     would slowly stop receiving bytes. See onFlushReady().
//
//   * VISIBILITY, as the gate on flushing rather than on drawing. A hidden pane
//     accumulates into the controller's bounded rolling buffer and replays it on
//     becoming visible (SPEC 5.4), which is what keeps a backgrounded phone app
//     from being handed megabytes it cannot draw.
//
// WHAT IS DROPPED: the mount handshake. The bridge had to start its controller
// HIDDEN because the page's write() handler did not exist until the bundle
// loaded, and bytes emitted before that were gone for good. The screen here is
// constructed with this object and consumes from the first flush, so there is
// no window to protect and no resetOutputAcknowledgements() equivalent — the
// renderer is never replaced without this object being destroyed with it.
//
// IDENTITY: a terminal's identity lives on the server (SPEC 5.2), so open() is
// the same two-phase sequence src/qml/TerminalPaneView.qml performs — resolve
// the pane's `terminal_panes` row to a server-minted tmux target, then attach a
// PTY channel to it. The layout pane id is only a recyclable slot LABEL and is
// never used to build a tmux target.
class MobileTerminalSession : public QObject {
    Q_OBJECT
    // MUST be registered: MobileAppController::createTerminalSession() returns
    // one of these to QML, and an unregistered return type makes that call fail
    // with "Unknown method return type" — leaving TerminalPage loaded, visible,
    // and permanently unable to attach.
    QML_ELEMENT
    QML_UNCREATABLE("A terminal session is minted by "
                    "MobileAppController.createTerminalSession().")
    // The grid this pane's bytes are interpreted into. CONSTANT: it is created
    // with the session and lives exactly as long, so a QML binding on it never
    // has to handle the screen being swapped underneath it.
    Q_PROPERTY(ch::VtScreen *screen READ screen CONSTANT)
    Q_PROPERTY(ch::TerminalState state READ state NOTIFY stateChanged)
    // Whole-sentence reason the pane is not live, because on a phone this is the
    // only thing on screen when a terminal fails to come up. Empty once the pane
    // is attached and there is nothing to explain.
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
public:
    // Upper bound on a dimension this class will push at a remote PTY. The
    // value crosses the wire as an SSH window-change and sizes a grid on the
    // remote host, so it is bounded here as well as in
    // ch::TerminalFactory::attach() — a QML layout mid-transition can report an
    // absurd cell count from a not-yet-settled item size.
    //
    // Expressed against ch::VtScreen's OWN grid bounds rather than as a second
    // independent number, because the local grid and the remote PTY must be told
    // the same size. A session that clamped at 2048 while ch::VtScreen::resize()
    // clamped at VtScreen::kMaxColumns would tell the remote to draw 2048 columns
    // and then parse that redraw against a narrower grid, wrapping every line in
    // the wrong place until the next resize happened to agree.
    static constexpr int kMaxColumns =
        VtScreen::kMaxColumns < 2048 ? VtScreen::kMaxColumns : 2048;
    static constexpr int kMaxRows = VtScreen::kMaxRows < 2048 ? VtScreen::kMaxRows : 2048;

    // `factory` is the application's ch::TerminalFactory (the `terminalFactory`
    // QML context property); not owned, and a null one leaves this session inert
    // chrome that explains itself instead of crashing, exactly like the desktop
    // pane's guarded context property.
    explicit MobileTerminalSession(TerminalFactory *factory, QObject *parent = nullptr);

    VtScreen *screen() const;
    TerminalState state() const;
    QString statusText() const;

    // TEST SEAM, and only that: the controller this session drives. Deliberately
    // not a Q_PROPERTY and not invokable, so QML — which on mobile draws
    // untrusted remote content — cannot reach the transport, the geometry or the
    // state machine except through the narrow surface below. A unit test needs it
    // to stand a QIODevice in for the SSH PTY channel, the same seam
    // ch::TerminalController::setTransport() exists for.
    TerminalController *controller() const;

    // Resolve this pane on the server and attach its PTY, i.e. the sequence
    // src/qml/TerminalPaneView.qml calls attachNow(). Idempotent while attached
    // or while a resolution is in flight; safe to call again whenever the
    // connection or the selected pane changes.
    //
    // `paneId` is the layout slot label and `terminalPaneId` the server row id
    // the layout leaf carries. Both are passed through to
    // ch::TerminalFactory::resolveTarget(), which decides the addressing mode:
    // an empty `terminalPaneId` is the legacy label lookup, and the mobile pane
    // picker fills the row id in for every leaf that has one.
    //
    // `cols`/`rows` at or below 0 mean "the view has not reported a size yet",
    // which lets the factory open at the conventional 80x24 rather than snapping
    // an already-laid-out pane back to it.
    Q_INVOKABLE void open(const QString &devSessionId,
                          const QString &paneId,
                          const QString &terminalPaneId,
                          const QString &workingDir,
                          int cols,
                          int rows);

    // One keystroke from the soft keyboard or the key bar. Encoded by
    // ch::vt::encodeKey() against the screen's CURRENT cursor-key mode, because
    // an arrow key is CSI A in normal mode and SS3 A once the remote program has
    // switched the keypad into application mode — sending the wrong one types
    // garbage into vi and readline.
    Q_INVOKABLE void sendKey(int key, int modifiers, const QString &text);
    // Literal text straight to the PTY: what an IME commits, and what the key
    // bar's punctuation keys produce. No key encoding is involved.
    Q_INVOKABLE void sendText(const QString &text);
    // Clipboard content, wrapped in the bracketed-paste guards when the remote
    // program has asked for them, so a shell can tell pasted text from typing.
    Q_INVOKABLE void paste(const QString &text);

    // The view's cell grid changed. Resizes the screen AND pushes an SSH
    // window-change, and is remembered so a later attach opens at this size.
    Q_INVOKABLE void resize(int cols, int rows);

    // Whether a renderer can currently show this pane (SPEC 5.4): the QML page
    // is on top of the stack AND the application is active. False makes the
    // controller retain output in its bounded rolling buffer instead of emitting
    // it; true replays what accumulated.
    Q_INVOKABLE void setVisible(bool visible);

    // Release this pane's channel. The remote tmux session is left RUNNING on
    // purpose — that is the whole reattach mechanism (SPEC 2.2): leaving the
    // page, backgrounding the app or losing the network all end up here, and the
    // user's build is still going when they come back.
    //
    // ch::TerminalFactory::kill(), which destroys the remote session and every
    // process in it, is deliberately NOT exposed on this class at all. A phone
    // is exactly where an accidental long-press must not be able to end a
    // running job, and there is no confirmation affordance in a single-pane UI
    // worth trusting with it. Destroying a terminal stays a desktop operation.
    Q_INVOKABLE void close();

    // Clipboard access for the pane's long-press menu. It lives HERE, on a class
    // the pane already has, because QML's own clipboard affordances come from
    // TextEdit/TextInput internals and the alternative — QApplication::clipboard()
    // — would drag QtWidgets into a mobile build that must not link it. QtGui's
    // QClipboard is all this needs.
    //
    // copyToClipboard() takes the text the caller already extracted with
    // ch::VtScreen::textRange(); this class never decides what a selection is.
    Q_INVOKABLE void copyToClipboard(const QString &text);
    // Read the clipboard and paste it into the PTY, bracketing included.
    Q_INVOKABLE void pasteFromClipboard();

signals:
    void stateChanged();
    void statusTextChanged();

private:
    void onFlushReady(const QByteArray &batch);
    void onTargetResolved(TerminalController *controller, const QString &target);
    void onFactoryError(TerminalController *controller, const QString &message);
    void onStateChanged(TerminalState state);
    // Second phase of open(): the target is known, open the PTY channel on it.
    void attachNow();
    void setStatusText(const QString &text);
    // The three ids that together name ONE terminal pane, as a single comparable
    // value. Joined with U+001F, which no id can contain, so two different panes
    // can never produce the same string.
    static QString identityOf(const QString &devSessionId, const QString &paneId,
                              const QString &terminalPaneId);

    // Not owned. QPointer because the factory is the application's and may be
    // destroyed while a page is still being torn down.
    QPointer<TerminalFactory> m_factory;
    // Owned through the QObject parent chain (this), so leaving the page
    // destroys the controller, its buffers, its timers and its channel with it.
    TerminalController *m_controller = nullptr;
    VtScreen *m_screen = nullptr;

    QString m_devSessionId;
    QString m_paneId;
    QString m_terminalPaneId;
    QString m_workingDir;
    // Server-minted tmux target once resolved; empty until then. Kept so a
    // reconnect or a retry goes straight to the attach, like the desktop pane.
    QString m_tmuxTarget;
    QString m_statusText;

    // Last geometry the view reported; 0 means "not reported yet", which the
    // factory reads as "keep whatever size this pane already had".
    int m_columns = 0;
    int m_rows = 0;

    // Identity of the in-flight ch::TerminalFactory::resolveTarget(), and EMPTY
    // when this session is waiting for no answer at all. Not a bare bool.
    //
    // The factory tags an answer with the CONTROLLER and nothing else, so an
    // answer that lands when nothing is being waited for is indistinguishable
    // from a wanted one, and adopting it re-opens an SSH PTY channel behind a
    // pane the user has already left — silently, because a signal handler did it
    // and not the user. close() therefore ABANDONS the flight by clearing this,
    // and an answer with nothing to match is dropped.
    //
    // WHAT THIS DOES NOT DO, so nobody expects more of it than it gives: it
    // cannot pick the right answer out of TWO outstanding requests, because the
    // signal does not say which request it answers. Re-opening this session on a
    // DIFFERENT pane while a lookup is in flight therefore lets the first answer
    // through, and it is ch::TerminalFactory::attach() that refuses it — the
    // factory only honours a target it authorised for this controller's LATEST
    // request. The pane lands in its ordinary "could not be attached" state with
    // the Reattach affordance already on screen, never on another pane's shell.
    // In this shell that path is unreachable anyway: one session is minted per
    // page and a page shows one pane for its whole life.
    QString m_resolvingIdentity;
    // Re-entrancy guard: ch::TerminalFactory::attach() walks the controller's
    // state machine, which reaches this object's own handlers, which are free to
    // call open() again.
    bool m_attaching = false;
    bool m_attached = false;
};

} // namespace ch
