#pragma once

#include "CodeharbordClient.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantMap>

namespace ch {

// The client end of the viewer control channel (SPEC 4.3): the seam an AI coding
// agent running in a terminal pane reaches this application's viewer panes
// through.
//
// The whole path, because no single file shows it:
//
//   agent tool (MCP server / codeharbor-view) on the SERVER
//     -> codeharbord's control Unix socket (remote/src/control.ts)
//     -> an id-less `viewer.command` JSON-RPC notification on the RPC channel
//     -> THIS class, which validates it and emits commandRequested()
//     -> Main.qml, which drives the real ViewerRegion and calls respond()
//     -> a `viewer.commandResult` REQUEST back to the daemon
//     -> the daemon writes the answer on the agent's still-open socket.
//
// Two decisions worth stating here rather than rediscovering:
//
// * The command arrives as a NOTIFICATION, not a server-initiated request.
//   CodeharbordClient deliberately treats a message carrying both `method` and
//   `id` as malformed (there is no server-request dispatcher), and the answer
//   travels back as an ordinary client request, which the transport already
//   carries in that direction.
//
// * This class performs NO layout work. It is the validating adapter between one
//   wire shape and one QML entry point; every pane mutation goes through the
//   same Main.qml/ViewerRegion/SessionLayouts path a user's click takes, so an
//   agent cannot reach a state the UI itself cannot produce.
class ViewerCommandService : public QObject {
    Q_OBJECT
public:
    // `client` supplies the notification stream and carries the answers back.
    // Not owned; a null or destroyed client makes every operation a no-op.
    explicit ViewerCommandService(CodeharbordClient* client = nullptr,
                                  QObject* parent = nullptr);

    // Answer a command previously announced by commandRequested().
    //
    // `errorCode` is one of the control channel's tokens — "bad_request",
    // "busy", "timeout", "not_active_session", "unknown_pane", "failed" — and is
    // ignored when `ok` is true. `data` carries the operation's result (a pane
    // id, a pane inventory) and is omitted from the wire when empty.
    //
    // Ignored for an id that is not in flight. That is not a defensive nicety:
    // the daemon may already have timed the command out, and a second answer
    // would settle a DIFFERENT agent's command if the daemon had meanwhile
    // reused nothing but the client had double-reported.
    Q_INVOKABLE void respond(const QString& commandId, bool ok,
                             const QString& errorCode, const QString& message,
                             const QVariantMap& data);

    // Commands announced and not yet answered. Past this bound a new command is
    // refused with "busy" instead of being emitted, which both bounds the set
    // and gives an agent firing faster than the UI can settle real
    // back-pressure rather than an unbounded queue of layout edits.
    static constexpr int kMaxInFlight = 32;

    // In-flight count, for tests asserting the bound holds.
    int inFlightCount() const { return m_inFlight.size(); }

signals:
    // One validated viewer command. `op` is one of "list", "open", "close",
    // "split", "focus", "reload"; `args` is that op's argument object, decoded
    // to a QVariantMap for QML. Exactly one respond() must follow, and the
    // handler must not assume anything about the pane ids in `args`: they come
    // from a remote process and may name panes that no longer exist.
    void commandRequested(const QString& commandId, const QString& devSessionId,
                          const QString& op, const QVariantMap& args);

private:
    void onNotification(const QString& method, const QJsonValue& params);

    QPointer<CodeharbordClient> m_client;
    QSet<QString> m_inFlight;
};

} // namespace ch
