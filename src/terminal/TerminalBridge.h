#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringConverter>

namespace ch {

class TerminalController;

// The Qt WebChannel face of one terminal pane (SPEC 5.1): the object the
// trusted xterm.js page (src/web/terminal) talks to, registered under the
// WebChannel object name "terminal" by src/qml/TerminalPaneView.qml.
//
// Why this exists instead of exposing TerminalController directly: WebChannel
// can only publish SLOTS, INVOKABLES, PROPERTIES and SIGNALS, and it cannot
// call a JS function at all. TerminalController's API is a plain C++ API
// (sendInput takes bytes, visibility is setViewVisible, output arrives as
// flushReady) — none of it is reachable from, or shaped like, the frozen
// TerminalBridge/TerminalHost contract in src/web/terminal/src/index.ts. This
// class is the translation, and nothing more:
//
//   JS -> C++ (the frozen TerminalBridge interface) ... the slots below
//   C++ -> JS (the frozen TerminalHost callbacks) .... the signals below
//     write(data)               -> host.write(data)
//     connectionStateChanged(s) -> host.setConnectionState(s)
//     clearRequested()          -> host.clear()
//
// Output crosses as text, so the raw PTY bytes are decoded with a STATEFUL
// UTF-8 decoder: TerminalController flushes on a size/time threshold (SPEC 5.5)
// and will happily cut a multi-byte sequence in half, which a per-batch
// QString::fromUtf8() would turn into replacement characters mid-glyph.
//
// The renderer only exists once the page has mounted, so the controller is put
// in the SPEC 5.4 hidden state at construction: everything the pane emits
// meanwhile accumulates in the controller's rolling buffer and is replayed as a
// single batch by ready(), the page's mount handshake.
//
// SECURITY (the bridge is the page's ONLY reach into C++): every slot below is
// callable by whatever is running in that WebEngineView, so the surface is
// deliberately tiny and target-free. There is no attach, no kill, no
// tmux-target or working-directory argument anywhere on it — a pane's remote
// target is chosen by the QML host through ch::TerminalFactory, which the page
// cannot see. What is left is the renderer's own view state (input bytes into
// THIS pane's PTY, its geometry, its visibility, its mount handshake), so the
// worst a compromised page can do is drive the terminal its user is already
// looking at. The one value that leaves the process is the geometry, and it is
// bounded below.

class TerminalBridge : public QObject {
    Q_OBJECT
    // Current ch::TerminalState as a string, so the page can render the status
    // strip immediately instead of waiting for the next transition.
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)
    // Geometry the renderer last reported, so the QML pane can attach the PTY
    // at the real size instead of the 80x24 fallback (0 until it reports).
    Q_PROPERTY(int columns READ columns NOTIFY geometryChanged)
    Q_PROPERTY(int rows READ rows NOTIFY geometryChanged)
public:
    // Upper bound on a renderer-reported dimension. The value crosses the wire
    // as an SSH window-change and sizes a grid on the remote host, so it may
    // not be whatever a page felt like sending. 2048 is far past any real
    // display (an 8K panel at a 5 px cell is ~1536 columns) and still bounds
    // the remote allocation.
    static constexpr int kMaxDimension = 2048;

    explicit TerminalBridge(TerminalController* controller, QObject* parent = nullptr);

    TerminalController* controller() const;
    QString connectionState() const;
    // True once the page reported its renderer mounted (ready()).
    bool rendererReady() const;
    int columns() const;
    int rows() const;

    // App side (menu/command palette): drop the renderer's visible screen
    // buffer. Purely a view operation — the remote pane is untouched.
    Q_INVOKABLE void requestClear();

public slots:
    // ---- the frozen TerminalBridge contract (called by the page) ----
    void sendInput(const QString& data);
    // Bounded: see kMaxDimension. Anything at or below it is passed through
    // untouched, including the non-positive values an unmounted renderer
    // reports (the controller rejects those).
    void resize(int cols, int rows);
    // Reports what the RENDERER (or the QML pane) can currently show. It is
    // only half of the controller's visibility: the other half is whether a
    // renderer exists at all — see applyVisibility().
    void notifyViewVisible(bool visible);
    // Mount handshake; optional on the JS side (`ready?()`).
    void ready();

signals:
    // ---- the frozen TerminalHost callbacks (consumed by the page) ----
    void write(const QString& data);
    void connectionStateChanged(const QString& state);
    void clearRequested();

    // The RENDERER reported a new cols x rows. Not part of the frozen page
    // contract; the QML pane uses it to attach at the right size.
    //
    // FOOTGUN: src/qml/TerminalPaneView.qml answers this signal by calling
    // attachNow(). It must therefore stay a report of what the PAGE asked for
    // and nothing else. In particular it must NOT be re-broadcast from geometry
    // the C++ side records itself: TerminalFactory::attach() sizes the
    // controller as part of attaching, so a bridge that relayed every
    // controller-side size change would re-enter attach() from inside attach().
    void geometryChanged();

private:
    void onFlushReady(const QByteArray& batch);
    // Push `m_viewVisible && m_rendererReady` at the controller.
    //
    // BOTH conjuncts are required, and the second one is the load-bearing part:
    // "visible" on the controller means "there is a renderer listening to
    // write(), so stop retaining output and emit it" (SPEC 5.4). A page that has
    // not finished mounting has connected NO handler to write(), so anything
    // emitted at it is gone for good — it is not in the controller's rolling
    // buffer either, because the controller already handed it over.
    //
    // notifyViewVisible() alone cannot decide this: it has a caller that speaks
    // before the page exists. src/qml/TerminalPaneView.qml reports the QML
    // item's own visibility (onVisibleChanged), and a pane that is hidden and
    // shown again while Chromium is still loading the bundle would otherwise
    // flip the controller visible and throw away the whole first screenful tmux
    // drew — leaving a blank terminal until the user presses a key.
    void applyVisibility();

    // Weak: the controller is owned by the pane and may outlive or predecease
    // the bridge depending on teardown order.
    QPointer<TerminalController> m_controller;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    bool m_rendererReady = false;
    // Last visibility reported by the page or the QML pane. True by default:
    // a pane is shown unless something says otherwise, and QML only reports on
    // a CHANGE, so the initial "visible" report never arrives.
    bool m_viewVisible = true;
};

} // namespace ch
