#pragma once

#include "CodeharbordClient.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

class QJsonObject;

namespace ch {

// Polls the daemon's `tmux.paneActivity` RPC and reports, per terminal pane,
// how long ago the SERVER last saw that pane produce output.
//
// It exists because ch::AgentStatusMonitor's activity detection for the
// adapterless "generic" harness is fed by bytes this client receives on a
// pane's PTY, and a pane the user is not looking at has no PTY: switching Dev
// Session destroys the pane and detaches its channel. The pane is still running
// on the host — its tmux session is untouched — so the only place the evidence
// still exists is the host, and this is the call that goes and asks for it.
//
// The reading is tmux's `#{window_activity}` for the pane's own tmux session
// (each CodeHarbor terminal pane IS a tmux session, named by its
// `terminal_panes.tmux_target`). That field updates on output with NO client
// attached, which is the entire property this subsystem is built on;
// `#{pane_activity}` does not exist on tmux 3.6 and renders as an empty string,
// which is why the daemon reports a pane it could not date as a null and why
// this class reports NOTHING for such a pane rather than a fabricated age.
//
// Ages, never timestamps: the daemon stamps every listing with its own `nowMs`
// and the subtraction happens against that, so the client's clock never enters
// the arithmetic and a host whose clock is minutes off is still measured
// correctly.
class TmuxActivityPoller : public QObject {
    Q_OBJECT
public:
    explicit TmuxActivityPoller(QObject* parent = nullptr);

    // Bind the RPC peer to ask. Ownership stays with the caller. A null client
    // stops the polling; swapping clients abandons any answer still owed by the
    // old one (see the in-flight guard in the implementation).
    void setRpcClient(CodeharbordClient* client);
    CodeharbordClient* rpcClient() const { return m_client; }

    // The Dev Sessions whose panes are worth asking about — in production the
    // live sidebar tree, pushed by ch::AppController on every refresh. An empty
    // set stops the polling entirely: with no Dev Session on screen there is no
    // sidebar row for an answer to change, and a client sitting on a connection
    // it is not displaying anything from must not keep waking the daemon.
    void setDevSessionIds(const QSet<QString>& devSessionIds);
    QSet<QString> devSessionIds() const { return m_devSessionIds; }

    // How often to ask. Five seconds is chosen against what the answer is worth:
    // the reading it carries has one-second granularity, the state it feeds is a
    // coarse busy/idle badge in a sidebar, and the request is a single small
    // frame that costs the daemon one `tmux list-sessions`. Polling faster would
    // buy resolution the underlying field does not have.
    static constexpr int kPollIntervalMs = 5000;
    // Policy, not physics; a test compresses it instead of spending real
    // seconds of suite time. Values at or below 0 are refused, since a zero or
    // negative interval would spin the event loop.
    void setPollIntervalMs(int ms);
    int pollIntervalMs() const { return m_pollIntervalMs; }

    // Issue one request now, outside the timer's cadence. Used on arming, so a
    // Dev Session switch does not wait a whole interval for its first answer,
    // and by the tests. Obeys the in-flight guard like any tick.
    void pollNow();

signals:
    // The server dated one pane's last output. `ageMs` is (the listing's server
    // `nowMs` - that pane's activity time), so it is measured entirely on the
    // server's clock; `alive` is whether tmux still has a session for the pane's
    // target at all.
    //
    // Emitted ONLY for a pane the server could date. A pane whose activity time
    // came back null produces no signal, because "the daemon does not know" is
    // not the same claim as "the pane has been quiet", and the consumer must not
    // have to tell them apart from an age.
    void activityObserved(const QString& devSessionId, const QString& terminalId,
                          qint64 ageMs, bool alive);

private:
    // Start or stop the timer to match "a client and at least one Dev Session",
    // and issue an immediate poll on the transition into polling.
    void rearm();
    // Turn one `tmux.paneActivity` result into activityObserved signals.
    void publishActivity(const QJsonObject& result);

    // QPointer so a caller-owned client destroyed while this poller outlives it
    // auto-nulls instead of dangling.
    QPointer<CodeharbordClient> m_client;
    QSet<QString> m_devSessionIds;
    QTimer m_timer;
    int m_pollIntervalMs = kPollIntervalMs;
    // Whether a request issued to m_client is still unanswered. A tick that
    // finds this set is SKIPPED rather than queued: a daemon that has gone slow
    // or wedged would otherwise accumulate one identical outstanding request per
    // five seconds, forever, and every one of them would be answered at once
    // when it recovered. Nothing is lost by skipping — the next tick asks the
    // same question and gets a fresher answer than the one that was dropped.
    bool m_inFlight = false;
};

} // namespace ch
