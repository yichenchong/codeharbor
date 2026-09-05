#include "MobileAppController.h"

#include "MobileKeyStore.h"
#include "MobileTerminalSession.h"
#include "ServerProfiles.h"
#include "TerminalFactory.h"
#include "UiStateStore.h"

#include <QQmlEngine>
#include <QStringList>
#include <utility>

namespace ch {
namespace {

// The profile fields a connect needs. ServerProfiles applies this very whitelist
// again at its own write boundary (no secret may ever reach QSettings), so this
// copy is not the security boundary: it is here so a QML page that passes an
// extra key gets a predictable stored profile rather than relying on the store
// to quietly drop it.
const QStringList& profileFields()
{
    static const QStringList fields = {
        QStringLiteral("name"),         QStringLiteral("host"),
        QStringLiteral("port"),         QStringLiteral("user"),
        QStringLiteral("identityFile"), QStringLiteral("nodePath"),
        QStringLiteral("repoRoot"),
    };
    return fields;
}

// Did ServerProfiles actually TAKE this edit? updateProfile() refuses a merged
// result that could never connect and reports that by leaving the stored profile
// alone, so the only way to tell an accepted edit from a refused one is to read
// the profile back. Compared field by field rather than re-validated, so the
// rule that decides stays in exactly one place - and compared the way the store
// KEEPS the values, which is why:
//   * every string is compared trimmed: sanitize() trims what it stores;
//   * `port` is compared as a number, because it is stored as an int and may be
//     submitted as a string by a QML form. An empty port is not a value at all,
//     it means "use the default", so there is nothing to compare;
//   * an empty `name` is filled in by the store (with the host), so it likewise
//     asserts nothing.
// Fields the caller did not submit keep their stored values by definition and
// are not looked at.
bool storedProfileTook(const QVariantMap& stored, const QVariantMap& submitted)
{
    for (auto it = submitted.cbegin(); it != submitted.cend(); ++it) {
        const QString wanted = it.value().toString().trimmed();
        if (it.key() == QLatin1String("port")) {
            if (wanted.isEmpty())
                continue;
            bool ok = false;
            const int port = wanted.toInt(&ok);
            if (!ok || stored.value(it.key()).toInt() != port)
                return false;
            continue;
        }
        if (it.key() == QLatin1String("name") && wanted.isEmpty())
            continue;
        if (stored.value(it.key()).toString() != wanted)
            return false;
    }
    return true;
}

} // namespace

MobileAppController::MobileAppController(AppController* app,
                                        SessionLayouts* layouts,
                                        SshConnectionPool* pool,
                                        QObject* parent)
    : QObject(parent)
    , m_app(app)
    , m_layouts(layouts)
    , m_pool(pool)
    , m_panes(new PaneListModel(this))
    , m_capabilities(new MobileCapabilities(this))
{
    if (m_app) {
        connect(m_app, &AppController::connectionStateChanged, this,
                &MobileAppController::syncConnectionState);
        connect(m_app, &AppController::activeSessionChanged, this,
                &MobileAppController::adoptActiveSession);
        // The server-forwarded message, verbatim (SPEC 10.3). It is the only
        // explanation the user gets for a refused workspace mutation, and on a
        // phone there is no log pane to fall back on.
        connect(m_app, &AppController::error, this,
                [this](const QString& message) { setStatusText(message); });
        // Argument ORDER differs from AppController's own signal on purpose: the
        // mobile surface puts the fingerprint - the thing the user is being
        // asked to compare - second, so a page never has to remember which of
        // two adjacent strings is the key type.
        connect(m_app, &AppController::hostKeyPrompt, this,
                [this](const QString& host, const QString& keyType,
                       const QString& fingerprint) {
                    setStatusText(tr("%1 presented an unknown host key.")
                                      .arg(host));
                    emit hostKeyPrompt(host, fingerprint, keyType);
                });
        connect(m_app, &AppController::credentialPrompt, this,
                [this](const QString& user, const QString& host,
                       const QString& prompt, const QString& kind) {
                    setStatusText(prompt);
                    emit credentialPrompt(user, host, prompt, kind);
                });
        // Take the connection words as they stand right now: this controller can
        // legitimately be built after a session is already up (a test, or a
        // future host that re-creates the mobile shell over a live spine), and
        // waiting for the next transition would show "disconnected" over a
        // working server.
        syncConnectionState();
        if (!m_app->activeSessionId().isEmpty())
            adoptActiveSession();
    }

    if (m_layouts) {
        connect(m_layouts, &SessionLayouts::loaded, this,
                &MobileAppController::onLayoutsLoaded);
        // A region that could not be read reports here and NOT through `loaded`
        // with a null tree, so without this a failed getLayout would leave the
        // busy indicator up with nothing said.
        connect(m_layouts, &SessionLayouts::error, this,
                [this](const QString& message) {
                    // Remembered as well as shown: the load this belongs to will
                    // still report itself finished, and onLayoutsLoaded() must
                    // not wipe the explanation on its way out.
                    m_layoutErrorReported = true;
                    setStatusText(message);
                });
    }

    if (m_pool) {
        // A CHANGED host key is refused by the pool outright and never reaches
        // the decision callback (SPEC 12.1), so it can only be surfaced from
        // here. It is deliberately NOT turned into a prompt: there is no
        // "accept" for this case.
        connect(m_pool, &SshConnectionPool::hostKeyMismatch, this,
                [this](const QString& host) {
                    setStatusText(
                        tr("The host key for %1 has CHANGED. The connection was "
                           "refused. Verify the new key out of band and remove "
                           "the old entry from known_hosts before retrying.")
                            .arg(host));
                });
    }
}

void MobileAppController::setNavStage(NavStage stage)
{
    if (m_navStage == stage)
        return;

    // THE SINGLE-LIVE-PANE INVARIANT, enforced. Any departure from Pane releases
    // the selection, which is what makes PaneHostPage's Loader drop its item and
    // therefore closes the PTY channel or editor buffer the pane held. See the
    // class comment: this is the C++ half, and it covers every exit, not just
    // back().
    if (m_navStage == Pane && stage != Pane)
        setSelectedPane(QString(), {});
    // The pane LIST belongs to one Dev Session's layout. Retreating past the
    // picker means no session is chosen any more, so the list is dropped rather
    // than left standing: a stale row would otherwise still validate in
    // selectPane() and open a pane of the session the user just left. Written
    // against the DESTINATION rather than "coming from Panes", because Pane ->
    // Servers (a disconnect made from inside a pane) skips the picker entirely
    // and must still release it.
    if (m_navStage >= Panes && stage < Panes) {
        m_panes->setTrees({}, {});
        // The list and the id of the session it came from are one fact; letting
        // them disagree is what would make adoptActiveSession() skip a load it
        // owes the user.
        m_shownSessionId.clear();
    }
    // A layout load still on the wire belongs to a Dev Session the user has now
    // left ENTIRELY: at Servers nothing is chosen, not even a session. Dropping
    // the stamp here is what keeps that reply from republishing the pane list
    // and walking the shell forward out of the connect page into a picker
    // nobody asked for - which is exactly what a back gesture during a load
    // used to do, because the busy veil covers the pages but not the platform
    // back key.
    //
    // Only Servers. Entering SESSIONS must not cancel anything:
    // adoptActiveSession() arms the stamp and then moves to precisely that
    // stage, so cancelling there would park the shell on a load whose reply it
    // had just decided to ignore.
    if (stage == Servers) {
        m_pendingSessionId.clear();
        setLayoutPending(false);
    }
    // Coming BACK to the picker from a live pane, re-read the trees. A pane
    // records what it opened through SessionLayouts::setPaneUrlForSession(),
    // which updates the region tree QUIETLY (republishing it would destroy the
    // very pane that just opened the file), so the row this model published at
    // load time still names the file the pane started on. Without this re-read,
    // reopening the pane from the picker would reopen that stale url and undo
    // the navigation the layout has already recorded.
    //
    // Guarded on the layouts object still holding the session this list came
    // from: while a load for another session is in flight both trees are null,
    // and publishing those would empty the picker for no reason.
    if (m_navStage == Pane && stage == Panes && m_layouts
        && !m_shownSessionId.isEmpty()
        && m_layouts->devSessionId() == m_shownSessionId) {
        m_panes->setTrees(m_layouts->viewerTree(), m_layouts->terminalTree());
    }

    m_navStage = stage;
    emit navStageChanged();
}

void MobileAppController::setStatusText(const QString& text)
{
    if (m_statusText == text)
        return;
    m_statusText = text;
    emit statusTextChanged();
}

void MobileAppController::setLayoutPending(bool pending)
{
    if (m_layoutPending == pending)
        return;
    m_layoutPending = pending;
    emit layoutPendingChanged();
}

void MobileAppController::setSelectedPane(const QString& paneKey,
                                          const QVariantMap& pane)
{
    if (m_selectedPaneKey == paneKey && m_selectedPane == pane)
        return;
    m_selectedPaneKey = paneKey;
    m_selectedPane = pane;
    emit selectedPaneChanged();
}

void MobileAppController::syncConnectionState()
{
    if (!m_app)
        return;

    const QString state = m_app->connectionState();
    // AppController distinguishes eight states because the desktop connect sheet
    // shows all eight. The mobile surface has four, and each fold is deliberate:
    // "provisioning", "hostkey" and "credential" are all still an attempt in
    // progress from the user's point of view (a sheet is up, or the server is
    // being prepared), and "reconnecting" is the session coming back rather than
    // a new failure. What is lost by folding - WHICH kind of waiting this is -
    // is exactly what statusText and the two prompt signals carry, so nothing
    // becomes unsayable.
    QString mapped;
    QString status;
    // Whether the attempt has come to REST, either way. A settled connection
    // clears the status line, because every sentence written on the way there
    // ("Connecting…", "Installing the CodeHarbor remote service…") is a progress
    // note about a round trip that is now over: leaving one standing told a
    // connected user, on the session list, that the shell was still connecting.
    bool settled = false;
    if (state == QLatin1String("connected")) {
        mapped = QStringLiteral("connected");
        settled = true;
    } else if (state == QLatin1String("connecting")) {
        mapped = QStringLiteral("connecting");
        status = tr("Connecting…");
    } else if (state == QLatin1String("provisioning")) {
        mapped = QStringLiteral("connecting");
        status = tr("Installing the CodeHarbor remote service…");
    } else if (state == QLatin1String("reconnecting")) {
        mapped = QStringLiteral("connecting");
        status = tr("Reconnecting…");
    } else if (state == QLatin1String("hostkey")
               || state == QLatin1String("credential")) {
        // The prompt handlers above have already written the sentence that
        // matters; overwriting it here with a generic one would replace the
        // fingerprint or the credential label with nothing.
        mapped = QStringLiteral("connecting");
    } else if (state == QLatin1String("failed")) {
        mapped = QStringLiteral("error");
        status = m_app->connectionError();
    } else {
        mapped = QStringLiteral("disconnected");
        settled = true;
    }

    if (m_connectionState != mapped) {
        m_connectionState = mapped;
        emit connectionStateChanged();
    }
    if (!status.isEmpty())
        setStatusText(status);
    else if (settled)
        setStatusText(QString());

    // A connection that is gone takes the whole selection with it: the panes
    // listed belong to a Dev Session on a server this client can no longer
    // reach, and the pane itself holds a channel that no longer exists.
    if (mapped == QLatin1String("disconnected")
        || mapped == QLatin1String("error")) {
        m_pendingSessionId.clear();
        m_shownSessionId.clear();
        setLayoutPending(false);
        setNavStage(Servers);
    } else if (mapped == QLatin1String("connected") && m_navStage == Servers) {
        // The session list is the first thing there is to choose once a server
        // answers. AppController::restoreActiveSession() may take us straight
        // past it on the very next refresh; until then this is the stage.
        setNavStage(Sessions);
    }
}

void MobileAppController::adoptActiveSession()
{
    if (!m_app)
        return;

    const QString devSessionId = m_app->activeSessionId();
    if (devSessionId.isEmpty()) {
        // The session was dropped: it disappeared from the authoritative tree
        // (another client deleted or archived it), or the server changed. Back
        // out to the session list, which releases the pane.
        m_pendingSessionId.clear();
        m_shownSessionId.clear();
        setLayoutPending(false);
        if (m_navStage > Sessions)
            setNavStage(Sessions);
        return;
    }

    // The session already on screen, re-announced. Nothing to wait for and
    // nothing to clear: its panes ARE the published list. See m_shownSessionId
    // for why this case is reachable at all - a load can complete synchronously,
    // and activateSession() drives it before it announces the switch, so the
    // reply can genuinely arrive before this handler does. Re-arming here would
    // then park the shell on a load that had already finished.
    if (devSessionId == m_shownSessionId && m_pendingSessionId.isEmpty())
        return;

    // Whatever we were waiting for is history: this is the newest load, and it
    // is the only one whose reply may touch the pane list.
    m_pendingSessionId = devSessionId;
    m_shownSessionId.clear();
    m_layoutErrorReported = false;
    setLayoutPending(true);
    // Both are the previous session's, and neither survives the switch. Clearing
    // BEFORE the reply lands is what keeps a tap during the round trip from
    // opening a pane of the session being left.
    setNavStage(Sessions);
    m_panes->setTrees({}, {});
    setStatusText(tr("Opening session…"));
}

void MobileAppController::onLayoutsLoaded(const QString& devSessionId)
{
    // The stale-load discard. A superseded load never reports at all (see
    // SessionLayouts::loaded), so the only reply that can arrive for a session
    // that is not the pending one is a load somebody else started - the
    // desktop-shared restore path, or a deliberate reload issued while the user
    // was already moving on. Either way it is not the layout the user is waiting
    // for and must not republish the list under them.
    if (devSessionId.isEmpty() || devSessionId != m_pendingSessionId)
        return;
    if (!m_layouts)
        return;

    m_pendingSessionId.clear();
    m_shownSessionId = devSessionId;
    setLayoutPending(false);
    // setTrees BEFORE the stage moves: setNavStage(Panes) is what makes the
    // picker visible, and it must never be visible over the previous session's
    // rows.
    m_panes->setTrees(m_layouts->viewerTree(), m_layouts->terminalTree());
    setNavStage(Panes);
    // The PROGRESS note goes; an EXPLANATION stays. SessionLayouts reports a
    // region it could not read through error() and then still reports the load
    // as finished, so clearing unconditionally erased the only sentence saying
    // why the picker is empty - leaving the picker's own "This Dev Session has
    // no panes yet." standing over a layout that failed to load.
    if (!m_layoutErrorReported)
        setStatusText(QString());
    emit sessionReady(devSessionId);

    // Resume where the user left this Dev Session. The key is the EXISTING
    // client-local one (selectedPane/<devSessionId>), so the two shells remember
    // the same thing, and it is validated against the freshly published list
    // rather than trusted: the pane may have been closed from another client
    // since it was stored, in which case the user simply lands on the picker.
    if (!m_app || !m_app->uiState())
        return;
    const QString remembered = m_app->uiState()->selectedPane(devSessionId);
    if (remembered.isEmpty())
        return;
    if (m_panes->paneByKey(remembered).isEmpty())
        return;
    selectPane(remembered);
}

QString MobileAppController::storeProfile(const QVariantMap& profile)
{
    if (!m_app) {
        setStatusText(tr("This shell has no connection spine to dial with."));
        return {};
    }
    ServerProfiles* profiles = m_app->serverProfiles();
    if (!profiles) {
        setStatusText(tr("This shell has no connection spine to dial with."));
        return {};
    }

    QVariantMap fields;
    for (const QString& field : profileFields()) {
        const auto value = profile.constFind(field);
        if (value != profile.constEnd())
            fields.insert(field, value.value());
    }

    // An `id` the store knows is an EDIT of that profile, not a new one: a
    // reconnect from the same page - or a second tap on Save - must not leave a
    // duplicate entry behind on every attempt.
    QString id = profile.value(QStringLiteral("id")).toString();
    if (!id.isEmpty() && !profiles->profile(id).isEmpty()) {
        profiles->updateProfile(id, fields);
        // updateProfile() refuses an edit whose merged result could never
        // connect, and says so by doing NOTHING. Left unchecked, that silently
        // dialled the PREVIOUSLY stored endpoint while the user was looking at
        // the form they had just changed - the worst kind of surprise, because
        // the connect succeeds against the wrong server. Whether the store took
        // the edit is read back rather than re-validated, so the rule that
        // decides stays in ServerProfiles.
        if (!storedProfileTook(profiles->profile(id), fields)) {
            setStatusText(tr("That server needs at least a host name and a user "
                             "name, and a port between 1 and 65535."));
            return {};
        }
        return id;
    }

    id = profiles->addProfile(fields);
    if (id.isEmpty()) {
        // ServerProfiles refuses a profile that could never connect rather than
        // storing it, so this is the one place the user learns why.
        setStatusText(tr("That server needs at least a host name and a user "
                         "name, and a port between 1 and 65535."));
    }
    return id;
}

void MobileAppController::connectToServer(QVariantMap profile)
{
    const QString id = storeProfile(profile);
    if (id.isEmpty())
        return;

    m_app->serverProfiles()->setActiveId(id);
    m_app->connectToProfile(id);
}

QString MobileAppController::saveServer(QVariantMap profile)
{
    const QString id = storeProfile(profile);
    if (id.isEmpty())
        return {};

    // The name the STORE kept, not the one that was submitted: a blank name is
    // filled in with the host on the way in, so reading it back is the only way
    // to name the entry the user will actually see in the list.
    const QString name =
        m_app->serverProfiles()->profile(id).value(QStringLiteral("name")).toString();
    // The one confirmation this page can give. A save that says nothing is
    // indistinguishable from the refusal above, which is exactly the complaint
    // that produced this entry point.
    setStatusText(tr("Saved \u201c%1\u201d.").arg(name));
    return id;
}

void MobileAppController::setTerminalFactory(TerminalFactory* factory)
{
    m_terminalFactory = factory;
}

MobileTerminalSession* MobileAppController::createTerminalSession(QObject* owner)
{
    if (!m_terminalFactory)
        return nullptr;
    // Parented to the CALLER - the QML page - and never to this controller. That
    // is the whole point: the session's lifetime is the pane's lifetime, so
    // navigating away destroys the page, the session, its controller and its
    // PTY channel together. An ownerless session is therefore REFUSED rather
    // than parented here: it would hold a shell open for the rest of the run
    // behind whatever the user looks at next, which is precisely the
    // single-live-pane invariant's failure mode.
    if (!owner)
        return nullptr;
    return new MobileTerminalSession(m_terminalFactory, owner);
}

void MobileAppController::setKeyStore(MobileKeyStore* store)
{
    m_keyStore = store;
}

void MobileAppController::disconnect()
{
    m_pendingSessionId.clear();
    m_shownSessionId.clear();
    setLayoutPending(false);
    setNavStage(Servers);
    if (m_app)
        m_app->disconnectServer();
    // Every secret the store holds goes with the connection: imported key bytes,
    // a resolved reference's bytes, an armed passphrase, and the identity
    // installed on the pool. SPEC 12.1 keeps a secret for ONE authentication
    // attempt, so a client sitting disconnected must not still be holding the
    // credential it used. Durable references survive - they name a file, they are
    // not secrets - so reconnecting needs no fresh pick.
    //
    // Ordering matters: the pool identity is cleared AFTER the session is torn
    // down, so nothing can re-authenticate with it on the way out.
    if (m_keyStore)
        m_keyStore->forgetSession();
}

void MobileAppController::acceptHostKey(bool trust)
{
    if (m_app)
        m_app->resolveHostKey(trust);
}

void MobileAppController::submitCredential(QString secret, QString kind)
{
    if (m_app)
        m_app->submitCredential(std::move(secret), std::move(kind));
}

void MobileAppController::selectSession(QString devSessionId)
{
    if (devSessionId.isEmpty())
        return;
    if (!m_app) {
        setStatusText(tr("This shell has no workspace to open a session in."));
        return;
    }

    // The load stamp is armed BEFORE the call, not from the activeSessionChanged
    // that follows it. activateSession() drives SessionLayouts::load() and only
    // THEN announces the switch, and a load can complete synchronously - with no
    // transport bound, ch::CodeharbordClient fails a call by invoking its
    // callback inline - so the reply can arrive before this function returns.
    // Armed afterwards, that reply would be discarded as stale and the shell
    // would sit on a busy indicator for a load that had already finished.
    m_pendingSessionId = devSessionId;
    // A fresh load owns the status line again: whatever the previous one could
    // not read is no longer the reason anything is on screen.
    m_layoutErrorReported = false;
    setLayoutPending(true);

    // activateSession() is the shared entry point and does three things this
    // controller must not duplicate: it refuses an id the authoritative tree
    // does not hold (or holds as archived), it records the id under the existing
    // session/<serverId>/active key, and it drives SessionLayouts::load(). Its
    // activeSessionChanged is what clears the previous session's panes, through
    // adoptActiveSession().
    m_app->activateSession(devSessionId);

    if (m_app->activeSessionId() != devSessionId) {
        // Refused. The row the user tapped is from a tree that has since been
        // replaced - the session was deleted or archived by another client - so
        // say so instead of waiting for a load that was never issued. Nothing
        // was disturbed: no activeSessionChanged was emitted, so the pane the
        // user is looking at (if any) is untouched.
        m_pendingSessionId.clear();
        setLayoutPending(false);
        setStatusText(tr("That Dev Session is no longer in the workspace."));
        return;
    }

    // The load above only happened if AppController is wired to the SAME
    // SessionLayouts this controller watches. It is in production (main.cpp
    // passes one instance to both) and in the tests, and then issuing a second
    // load here would cost a full extra pair of getLayout round trips on every
    // tap for a tree we are already fetching. When they are NOT the same object,
    // nothing has been asked for yet and this is the request.
    if (m_layouts && m_app->layouts() != m_layouts)
        m_layouts->load(devSessionId);
}

void MobileAppController::selectPane(QString paneKey)
{
    const QVariantMap pane = m_panes->paneByKey(paneKey);
    if (pane.isEmpty()) {
        // Not an error to report to the server, and not a programming fault: the
        // key may have come out of UiStateStore and name a pane another client
        // closed, or out of a picker row that a republish has already replaced.
        setStatusText(tr("That pane is no longer part of this session's layout."));
        return;
    }

    setSelectedPane(paneKey, pane);
    setNavStage(Pane);
    setStatusText(QString());

    // Remembered per Dev Session under the EXISTING client-local key, so
    // reopening the session resumes this pane. Written after the stage moves, so
    // a store that refuses the write (an empty Dev Session id) cannot stop the
    // pane from opening.
    //
    // Keyed on the session whose layout this list was PUBLISHED from, not on
    // whatever AppController calls active. The two agree whenever one
    // SessionLayouts is shared (production and the tests), and where they do not
    // it is this list the user just picked from, so writing the other id would
    // remember the pane under a session it does not belong to - and the restore
    // in onLayoutsLoaded(), which reads by the loaded id, would never find it.
    if (m_app && m_app->uiState())
        m_app->uiState()->setSelectedPane(m_shownSessionId, paneKey);
}

void MobileAppController::back()
{
    switch (m_navStage) {
    case Pane:
        // setNavStage releases the pane; see the invariant in the header.
        setNavStage(Panes);
        break;
    case Panes:
        setNavStage(Sessions);
        break;
    case Sessions:
        setNavStage(Servers);
        break;
    case Servers:
        // The root. A no-op rather than an error, so QML can bind the platform
        // back gesture to this unconditionally.
        break;
    }
}

} // namespace ch
