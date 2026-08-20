#pragma once

#include "AppController.h"
#include "MobileCapabilities.h"
// Not a forward declaration: createTerminalSession() returns one of these to QML,
// and a QML-visible return type must be a COMPLETE metatype or moc refuses to
// build the meta-object at all.
#include "MobileTerminalSession.h"
#include "PaneListModel.h"
#include "SessionLayouts.h"
#include "SshConnectionPool.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QQmlEngine>

namespace ch {

class MobileKeyStore;

// The mobile shell's navigation controller: the ONE object that owns "where am I
// and what am I looking at" for a client that can show exactly one pane at a
// time.
//
// It is deliberately thin. Everything it drives already exists and is shared
// verbatim with the desktop — ch::AppController for the connection and the
// workspace, ch::SessionLayouts for the server-authoritative region trees,
// ch::UiStateStore for client-local presentation state — and nothing here
// duplicates or shadows any of it. What is genuinely new is the two-step
// SELECTION the desktop does not have: the desktop shows every Dev Session in a
// sidebar beside every pane of the active one, whereas a phone must ask twice
// (which session, then which pane) and then commit the whole screen to one
// answer.
//
// THE SINGLE-LIVE-PANE INVARIANT. At most one pane object exists at any moment.
// That is not a memory optimisation: a terminal pane holds an SSH PTY channel
// and an editor pane holds unsaved buffer state, so a shell that quietly kept
// the previous pane alive behind the current one would hold a shell open the
// user believes they left, and would attach a second PTY to the same tmux target
// on the way back. It is enforced structurally in two halves that must agree:
//   * here, by clearing selectedPane/selectedPaneKey on every transition OUT of
//     NavStage::Pane (see setNavStage), so nothing downstream can be handed a
//     pane it should no longer have;
//   * in PaneHostPage.qml, whose single Loader is the only thing that ever
//     instantiates a pane and which has no source at all outside that stage.
// Neither half is sufficient alone, and neither may be relaxed without the
// other.
//
// Forward-declared rather than included: ch::MobileKeyStore, which is only ever
// held and called through a pointer here, and ch::TerminalFactory, which
// AppController.h (included above for AppController itself) already declares.
// ch::MobileTerminalSession is NOT among them - see the include at the top of
// this file for why a QML-visible return type has to be a complete type.

// STALE LOADS. A layout load is a server round trip and the user can tap a
// second Dev Session while the first is still on the wire. SessionLayouts
// already drops the replies of a superseded load, and its header spells out the
// consumer's half of that contract: key on the NEWEST load you asked for rather
// than counting `loaded` signals, because a superseded load never reports at
// all. m_pendingSessionId is exactly that key.
class MobileAppController : public QObject {
    Q_OBJECT
    // Named from QML (MobileAppController.Panes in MobileMain.qml) but never
    // constructed there: the one instance is the host's and arrives as the
    // `mobile` context property.
    QML_ELEMENT
    QML_UNCREATABLE("MobileAppController is provided to QML as the `mobile` "
                    "context property and cannot be created.")

public:
    // Where the two-step selection currently is. Servers is the pre-connection
    // stage (no server, hence no sessions to list); Panes is reached only once
    // BOTH region trees of the chosen Dev Session have resolved, so the picker
    // can never offer a pane from a layout that is still on the wire.
    enum NavStage {
        Servers,
        Sessions,
        Panes,
        Pane,
    };
    Q_ENUM(NavStage)


    // Declared after the enum so the NavStage property below names a type moc
    // has already seen.
    Q_PROPERTY(ch::AppController* app READ app CONSTANT)
    Q_PROPERTY(ch::PaneListModel* panes READ panes CONSTANT)
    Q_PROPERTY(ch::MobileCapabilities* capabilities READ capabilities CONSTANT)
    Q_PROPERTY(NavStage navStage READ navStage NOTIFY navStageChanged)
    Q_PROPERTY(QString selectedPaneKey READ selectedPaneKey
                   NOTIFY selectedPaneChanged)
    Q_PROPERTY(QVariantMap selectedPane READ selectedPane
                   NOTIFY selectedPaneChanged)
    Q_PROPERTY(QString connectionState READ connectionState
                   NOTIFY connectionStateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    // Whether a layout load this controller is waiting for is still on the wire.
    // MobileMain binds its busy indicator to it, and it is a property rather
    // than something QML derives from navStage because the stage does NOT move
    // while a load is pending: the whole point of waiting is that the pane
    // picker is not shown until both regions have resolved.
    Q_PROPERTY(bool layoutPending READ layoutPending NOTIFY layoutPendingChanged)

    // `app` and `layouts` are borrowed and must outlive this object; both are
    // owned by main.cpp, exactly as the desktop shell borrows them. `pool` is
    // optional (nullptr is safe) and is used for ONE thing this controller
    // cannot learn any other way: SshConnectionPool refuses a CHANGED host key
    // outright rather than routing it through the decision callback, so the
    // mismatch is only observable as a pool signal.
    explicit MobileAppController(AppController* app, SessionLayouts* layouts,
                                 SshConnectionPool* pool,
                                 QObject* parent = nullptr);

    AppController* app() const { return m_app; }
    PaneListModel* panes() const { return m_panes; }
    MobileCapabilities* capabilities() const { return m_capabilities; }
    NavStage navStage() const { return m_navStage; }
    QString selectedPaneKey() const { return m_selectedPaneKey; }
    QVariantMap selectedPane() const { return m_selectedPane; }
    QString connectionState() const { return m_connectionState; }
    QString statusText() const { return m_statusText; }
    bool layoutPending() const { return m_layoutPending; }

    // Connect to `profile`, a {name, host, port, user, identityFile, nodePath,
    // repoRoot} map with an optional `id` naming a stored profile to reuse.
    //
    // The profile is written to ch::ServerProfiles first and the connect then
    // goes through ch::AppController::connectToProfile, which is the REAL
    // ch::SessionBootstrap chain: the bounded connect pre-flight, remote-service
    // provisioning, the host-key decision callback, the credential chain, and
    // the wire-and-adopt tail. There is deliberately no second connect path
    // here — a mobile-only one would have to reimplement all of that, and the
    // half of it that was forgotten would be the half that matters.
    //
    // A profile that could never connect (no host, no user, an impossible port)
    // is refused by ServerProfiles' own write-boundary validation; this reports
    // that through statusText rather than dialling nothing.
    Q_INVOKABLE void connectToServer(QVariantMap profile);
    // Deliberately SHADOWS QObject::disconnect(), whose three-argument form is
    // also callable with none. That is why there is no `using
    // QObject::disconnect;` here: it would make an unqualified `disconnect()`
    // ambiguous. Nothing in this class disconnects a connection by name, and a
    // caller that needs to can still say QObject::disconnect(...) explicitly.
    Q_INVOKABLE void disconnect();

    // Answer a hostKeyPrompt. Accepting trusts the key and retries the connect;
    // rejecting ends the attempt. A call with no prompt outstanding is ignored
    // by AppController, so a sheet left open behind a live session cannot
    // redial or drop it.
    Q_INVOKABLE void acceptHostKey(bool trust);

    // Answer a credentialPrompt. `kind` is "keyPassphrase" or "password" and
    // keeps a private-key passphrase from ever being offered to a remote host's
    // password authentication; an empty secret cancels the attempt.
    //
    // Not in the mobile navigation vocabulary, but the connect chain it belongs
    // to is: without it an encrypted key or a password-only server is a dead
    // end that parks forever on a prompt nobody can answer. It is a straight
    // forward to the existing AppController seam and adds no policy of its own.
    Q_INVOKABLE void submitCredential(QString secret, QString kind);

    // Make `devSessionId` current and fetch its layout. Moves to Panes (and
    // reports sessionReady) only when BOTH region trees have resolved.
    Q_INVOKABLE void selectSession(QString devSessionId);

    // Open the pane named by "<region>:<paneId>". Validated against the current
    // pane list: an unknown key is a no-op that says why in statusText, because
    // the key may have been restored from UiStateStore and name a pane another
    // client has since closed.
    Q_INVOKABLE void selectPane(QString paneKey);

    // One step back: Pane -> Panes -> Sessions -> Servers. At Servers it is a
    // no-op, which is what lets QML bind the platform back gesture to it
    // unconditionally without having to know it is at the root.
    Q_INVOKABLE void back();

    // Wire the per-pane terminal factory. Ownership stays with main.cpp and
    // nullptr is safe, mirroring ch::AppController::setTerminalFactory - the
    // house injection style, chosen over a constructor argument for the same
    // reason it was there: existing construction sites (the tests, and any
    // future host that has no terminal at all) stay untouched.
    void setTerminalFactory(TerminalFactory* factory);

    // Wire the credential store, so a disconnect can drop every secret it holds.
    // Injected the same way and for the same reason as the factory above;
    // nullptr is safe and simply means there is nothing to forget.
    //
    // This is not a convenience: ch::MobileKeyStore::forgetSession() is the ONLY
    // thing that wipes imported key bytes, a resolved reference's bytes, an armed
    // passphrase and the pool's in-memory identity, and its contract says it runs
    // on explicit disconnect. Without this wire, "Disconnect" left the last
    // credential live in the process for the rest of the run.
    void setKeyStore(MobileKeyStore* store);

    // Mint a terminal session owned by `owner`, mirroring
    // ch::TerminalFactory::create(): the session dies with the QML page that
    // asked for it, taking its controller, its PTY channel and its VtScreen
    // with it. That is the single-live-pane invariant's teeth on the terminal
    // side - PaneHostPage destroys the page, and the shell goes with it.
    //
    // Returns nullptr when no factory was injected, which is what a page loaded
    // outside the shell gets, and nullptr for a null `owner`, because a session
    // with no owning page has nothing to die with: it would hold a PTY channel
    // open for the rest of the run behind whatever the user looks at next. QML
    // must null-check either way, exactly as it guards a missing context
    // property.
    Q_INVOKABLE MobileTerminalSession* createTerminalSession(QObject* owner);

signals:
    void navStageChanged();
    void selectedPaneChanged();
    void connectionStateChanged();
    void statusTextChanged();
    void layoutPendingChanged();
    // An UNKNOWN host key was presented; answer with acceptHostKey(). `host` is
    // the bare host as libssh reported it (NOT the "[host]:port" known_hosts
    // spelling), and `fingerprint` is OpenSSH's own display form,
    // "SHA256:<unpadded base64>", so it can be compared character for character
    // against `ssh-keygen -lf`.
    void hostKeyPrompt(QString host, QString fingerprint, QString keyType);
    // Default keys could not authenticate; answer with submitCredential().
    void credentialPrompt(QString user, QString host, QString prompt,
                          QString kind);
    // Both region trees of `devSessionId` have resolved and the pane list has
    // been republished from them.
    void sessionReady(QString devSessionId);

private:
    // The one writer of m_navStage. Clearing the pane selection on the way out
    // of Pane lives HERE rather than in back(), because back() is not the only
    // exit: disconnecting, switching Dev Session, and a session disappearing
    // from the authoritative tree all leave that stage too, and each one of them
    // must release the live pane.
    void setNavStage(NavStage stage);
    void setStatusText(const QString& text);
    void setLayoutPending(bool pending);
    void setSelectedPane(const QString& paneKey, const QVariantMap& pane);
    // Mirror AppController's connection vocabulary onto the four words the
    // mobile surface distinguishes, and turn the rest into statusText.
    void syncConnectionState();
    // AppController::activeSessionChanged: adopt whatever session is now active
    // as the load we are waiting for. This is also how the RESTORE path is
    // followed rather than reimplemented: AppController::restoreActiveSession()
    // reopens the remembered session out of its own `refreshed` handler, so the
    // mobile shell walks straight to that session's panes after a connect
    // without needing a second copy of the remembering.
    void adoptActiveSession();
    // SessionLayouts::loaded: publish the trees and advance, or drop the reply
    // when it belongs to a Dev Session the user has already left.
    void onLayoutsLoaded(const QString& devSessionId);

    QPointer<AppController> m_app;
    QPointer<SessionLayouts> m_layouts;
    QPointer<SshConnectionPool> m_pool;
    // Borrowed from main.cpp like every other injected collaborator, and held
    // through a QPointer for the same reason the others are: a host is allowed
    // to tear the spine down while this object is still alive.
    QPointer<TerminalFactory> m_terminalFactory;
    PaneListModel* m_panes = nullptr;
    MobileCapabilities* m_capabilities = nullptr;
    // Not owned. QPointer for the same reason the factory is one: the host owns
    // it, and a controller outliving it must not dereference a stale pointer.
    QPointer<MobileKeyStore> m_keyStore;

    NavStage m_navStage = Servers;
    QString m_selectedPaneKey;
    QVariantMap m_selectedPane;
    QString m_connectionState = QStringLiteral("disconnected");
    QString m_statusText;
    // The newest Dev Session a load was asked for. Empty when nothing is
    // pending. See STALE LOADS above.
    QString m_pendingSessionId;
    // The Dev Session whose panes are currently PUBLISHED. It is what lets
    // adoptActiveSession() tell "the controller just activated a session I have
    // not loaded" from "the controller re-announced the session already on
    // screen", and the second case has to be a no-op: a load can complete
    // SYNCHRONOUSLY (ch::CodeharbordClient fails a call with no transport bound
    // by invoking its callback inline), and AppController::activateSession()
    // drives the load BEFORE it emits activeSessionChanged - so the reply can
    // genuinely land first, and re-arming afterwards would leave the shell
    // waiting forever for a load that had already finished.
    // It is also the key the selected pane is remembered under (see
    // selectPane), so the list and the memory of what was picked from it can
    // never name different sessions.
    QString m_shownSessionId;
    bool m_layoutPending = false;
    // Whether the load currently being waited for has already had a region
    // reported unreadable. SessionLayouts reports such a region through error()
    // and STILL reports the load as finished, so without this the "load
    // finished" tail would erase the only sentence explaining why the pane
    // picker came up empty. Reset whenever a load is armed.
    bool m_layoutErrorReported = false;
};

} // namespace ch
