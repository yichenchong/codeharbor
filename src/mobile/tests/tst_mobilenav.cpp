#include <QtTest/QtTest>

#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringList>
#include <QVector>

// The fake codeharbord and the object graph under test. Shared with
// tst_mobileshell, which drives the same walk through the real QML shell.
#include "MobileNavFixture.h"
#include "MobileKeyStore.h"
#include "ServerProfiles.h"

#include <QTemporaryDir>

using namespace ch;
using namespace chtest;

// ch::MobileAppController is the whole of the mobile client's navigation: the
// two-step selection (which Dev Session, then which pane), the single-live-pane
// invariant, and the persistence that makes reopening a session resume where the
// user left it. Every case here drives the REAL AppController and the REAL
// SessionLayouts over a fake server, because the things worth asserting are
// precisely the interactions with them - a stale layout reply, a session the
// authoritative tree no longer holds, a remembered pane that has since been
// closed.
class TstMobileNav : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();

    void walksServersSessionsPanesThenOnePane();
    void backUnwindsOneStageAtATimeAndStopsAtTheRoot();
    void aRapidSessionSwitchDiscardsTheStaleLayoutReply();
    void leavingThePaneStageAlwaysReleasesTheLivePane();
    void theSelectedPaneIsRememberedPerDevSessionAndRestored();
    void aRememberedPaneThatIsGoneLeavesTheUserOnThePicker();
    void anUnknownPaneKeyIsANoOpThatSaysWhy();
    void aSessionTheWorkspaceNoLongerHoldsIsRefused();
    void theActiveSessionIsRecordedUnderTheExistingUiStateKey();
    void disconnectingDropsEveryCredentialTheStoreHolds();
    void aBackGestureDuringALayoutLoadCancelsIt();
    void aFailedLayoutLoadKeepsItsExplanation();
    void returningToThePickerRepublishesWhatThePaneOpened();
    void theShellFollowsASessionActivatedByAnybodyElse();
    void aSessionThatVanishesFromTheWorkspaceReleasesThePane();
    void reselectingTheSameSessionReloadsItRatherThanHanging();
    void aSettledConnectionStopsClaimingToBeBusy();
    void aRefusedProfileEditIsReportedRatherThanDiallingTheOldOne();
    void anOwnerlessTerminalSessionIsRefused();
};

// AppController's UiStateStore is the real per-user QSettings (constructed with
// an empty ini path, exactly as main.cpp leaves it), and these cases drive it for
// real. Redirect QSettings at the process level rather than writing into the
// developer's actual config - the same measure src/app/tests takes.
void TstMobileNav::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

// The whole walk, once, in order. Nothing may advance early: the pane picker is
// reachable only after BOTH region trees have resolved, because a picker built on
// a half-arrived layout would offer panes that do not exist.
void TstMobileNav::walksServersSessionsPanesThenOnePane()
{
    Fixture f;
    QSignalSpy stageSpy(&f.mobile, &MobileAppController::navStageChanged);
    QSignalSpy readySpy(&f.mobile, &MobileAppController::sessionReady);

    QCOMPARE(f.mobile.navStage(), MobileAppController::Servers);
    QVERIFY(f.mobile.panes() != nullptr);
    QVERIFY(f.mobile.capabilities() != nullptr);
    QCOMPARE(f.mobile.app(), &f.controller);

    f.listSessions({QStringLiteral("s1"), QStringLiteral("s2")});

    f.mobile.selectSession(QStringLiteral("s1"));
    // Armed, not arrived: the stage is the session list and the busy flag is up
    // until both regions answer.
    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);
    QVERIFY(f.mobile.layoutPending());
    QCOMPARE(readySpy.count(), 0);

    const QVector<QJsonObject> requests = f.takeLayoutRequests();
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.at(0).value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("region")).toString(),
             QStringLiteral("viewer"));
    QCOMPARE(requests.at(1).value(QStringLiteral("params")).toObject()
                 .value(QStringLiteral("region")).toString(),
             QStringLiteral("terminal"));

    // Only the viewer region so far. Still not enough.
    f.transport.deliver(
        layoutFrame(requests.at(0).value(QStringLiteral("id")).toInt(),
                    viewerTreeFor(QStringLiteral("viewer-1"),
                                  QStringLiteral("file:///srv/s1/notes.md"))));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);
    QVERIFY(f.mobile.layoutPending());
    QCOMPARE(readySpy.count(), 0);

    f.transport.deliver(
        layoutFrame(requests.at(1).value(QStringLiteral("id")).toInt(),
                    terminalTreeFor(QStringLiteral("terminal-1"),
                                    QStringLiteral("row-1"))));

    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    QVERIFY(!f.mobile.layoutPending());
    QCOMPARE(readySpy.count(), 1);
    QCOMPARE(readySpy.at(0).at(0).toString(), QStringLiteral("s1"));
    QCOMPARE(paneKeys(f.mobile.panes()),
             QStringList({QStringLiteral("viewer:viewer-1"),
                          QStringLiteral("terminal:terminal-1")}));

    f.mobile.selectPane(QStringLiteral("terminal:terminal-1"));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Pane);
    QCOMPARE(f.mobile.selectedPaneKey(), QStringLiteral("terminal:terminal-1"));
    QCOMPARE(f.mobile.selectedPane().value(QStringLiteral("kind")).toString(),
             QStringLiteral("terminal"));
    // The identity a terminal page attaches by is the server's row id, not the
    // recyclable slot label.
    QCOMPARE(f.mobile.selectedPane()
                 .value(QStringLiteral("terminalPaneId")).toString(),
             QStringLiteral("row-1"));

    // Servers -> Sessions (on select), Sessions -> Panes (on load), Panes ->
    // Pane. Three transitions, no churn in between.
    QCOMPARE(stageSpy.count(), 3);
}

void TstMobileNav::backUnwindsOneStageAtATimeAndStopsAtTheRoot()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});
    f.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                  QStringLiteral("terminal-1"));
    f.mobile.selectPane(QStringLiteral("viewer:viewer-1"));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Pane);

    f.mobile.back();
    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    // The pane list survives the step back to the picker: it is the picker's
    // content.
    QCOMPARE(f.mobile.panes()->rowCount(), 2);

    f.mobile.back();
    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);
    // Leaving the picker DOES drop the list: it belongs to a Dev Session that is
    // no longer chosen, and a stale row would still validate in selectPane().
    QCOMPARE(f.mobile.panes()->rowCount(), 0);

    f.mobile.back();
    QCOMPARE(f.mobile.navStage(), MobileAppController::Servers);

    // The root. A no-op, so QML can bind the platform back gesture to this
    // unconditionally without knowing where it is.
    f.mobile.back();
    QCOMPARE(f.mobile.navStage(), MobileAppController::Servers);
}

// A layout load is a server round trip and the user can tap a second Dev Session
// while the first is still on the wire. SessionLayouts drops a superseded load's
// replies, and this controller's half of that contract is to key on the NEWEST
// load it asked for - so a `loaded` for anything else may not republish the list
// under the user.
void TstMobileNav::aRapidSessionSwitchDiscardsTheStaleLayoutReply()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1"), QStringLiteral("s2")});
    QSignalSpy readySpy(&f.mobile, &MobileAppController::sessionReady);

    f.mobile.selectSession(QStringLiteral("s1"));
    const QVector<QJsonObject> first = f.takeLayoutRequests();
    QCOMPARE(first.size(), 2);

    // The user moves on before either region answers.
    f.mobile.selectSession(QStringLiteral("s2"));
    const QVector<QJsonObject> second = f.takeLayoutRequests();
    QCOMPARE(second.size(), 2);

    // s1's replies arrive late. They are superseded, so SessionLayouts drops them
    // and never reports: no pane list, no stage change.
    f.answerLayouts(first,
                    viewerTreeFor(QStringLiteral("viewer-1"),
                                  QStringLiteral("file:///srv/s1/a.md")),
                    terminalTreeFor(QStringLiteral("terminal-1"),
                                    QStringLiteral("row-1")));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);
    QCOMPARE(readySpy.count(), 0);
    QCOMPARE(f.mobile.panes()->rowCount(), 0);

    // And even a `loaded` for the abandoned session - which another consumer of
    // the same layouts object could legitimately cause - is discarded here rather
    // than trusted, because the pending id is the only thing that decides.
    QVERIFY(QMetaObject::invokeMethod(&f.layouts, "loaded",
                                      Q_ARG(QString, QStringLiteral("s1"))));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);
    QCOMPARE(readySpy.count(), 0);
    QVERIFY(f.mobile.layoutPending());

    // s2's replies are the ones that count.
    f.answerLayouts(second,
                    viewerTreeFor(QStringLiteral("viewer-9"),
                                  QStringLiteral("file:///srv/s2/b.txt")),
                    terminalTreeFor(QStringLiteral("terminal-9"),
                                    QStringLiteral("row-9")));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    QCOMPARE(readySpy.count(), 1);
    QCOMPARE(readySpy.at(0).at(0).toString(), QStringLiteral("s2"));
    QCOMPARE(paneKeys(f.mobile.panes()),
             QStringList({QStringLiteral("viewer:viewer-9"),
                          QStringLiteral("terminal:terminal-9")}));
}

// THE SINGLE-LIVE-PANE INVARIANT. A pane holds an SSH PTY channel or an editor
// buffer, so exactly one may exist at a time - and every exit from the pane stage
// has to release it, not just the back button. Three exits are asserted here:
// back, a session switch, and the connection going away.
void TstMobileNav::leavingThePaneStageAlwaysReleasesTheLivePane()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1"), QStringLiteral("s2")});
    f.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                  QStringLiteral("terminal-1"));

    f.mobile.selectPane(QStringLiteral("terminal:terminal-1"));
    QVERIFY(!f.mobile.selectedPane().isEmpty());
    f.mobile.back();
    QVERIFY(f.mobile.selectedPaneKey().isEmpty());
    QVERIFY(f.mobile.selectedPane().isEmpty());

    // A session switch made from inside a pane.
    f.mobile.selectPane(QStringLiteral("terminal:terminal-1"));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Pane);
    f.mobile.selectSession(QStringLiteral("s2"));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);
    QVERIFY(f.mobile.selectedPaneKey().isEmpty());
    QVERIFY(f.mobile.selectedPane().isEmpty());

    // Answer the switch's own load so the case continues from a settled state,
    // then open a pane on the session it switched to.
    f.answerLayouts(f.takeLayoutRequests(),
                    viewerTreeFor(QStringLiteral("viewer-9"),
                                  QStringLiteral("file:///srv/s2/b.txt")),
                    terminalTreeFor(QStringLiteral("terminal-9"),
                                    QStringLiteral("row-9")));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    f.mobile.selectPane(QStringLiteral("viewer:viewer-9"));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Pane);

    // And the connection going away: the pane's channel is gone with the
    // session, so holding the selection would describe something that no longer
    // exists. This exit skips the picker entirely, which is exactly the case a
    // release rule written as "coming from Panes" would have missed.
    f.mobile.disconnect();
    QCOMPARE(f.mobile.navStage(), MobileAppController::Servers);
    QVERIFY(f.mobile.selectedPaneKey().isEmpty());
    QVERIFY(f.mobile.selectedPane().isEmpty());
    QCOMPARE(f.mobile.panes()->rowCount(), 0);
}

// Resuming where the user left off, through the EXISTING client-local key
// (selectedPane/<devSessionId>) that the desktop shell already writes - not a
// second, mobile-only one, or the two shells would remember different things.
void TstMobileNav::theSelectedPaneIsRememberedPerDevSessionAndRestored()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1"), QStringLiteral("s2")});
    f.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                  QStringLiteral("terminal-1"));

    f.mobile.selectPane(QStringLiteral("terminal:terminal-1"));
    QCOMPARE(f.controller.uiState()->selectedPane(QStringLiteral("s1")),
             QStringLiteral("terminal:terminal-1"));

    // Per Dev Session: s2's choice is its own and does not disturb s1's.
    f.mobile.selectSession(QStringLiteral("s2"));
    f.answerLayouts(f.takeLayoutRequests(),
                    viewerTreeFor(QStringLiteral("viewer-9"),
                                  QStringLiteral("file:///srv/s2/b.txt")),
                    terminalTreeFor(QStringLiteral("terminal-9"),
                                    QStringLiteral("row-9")));
    f.mobile.selectPane(QStringLiteral("viewer:viewer-9"));
    QCOMPARE(f.controller.uiState()->selectedPane(QStringLiteral("s2")),
             QStringLiteral("viewer:viewer-9"));
    QCOMPARE(f.controller.uiState()->selectedPane(QStringLiteral("s1")),
             QStringLiteral("terminal:terminal-1"));

    // Reopening s1 goes straight back to the pane it was left in, which is the
    // only thing that makes the persistence observable. The restore is VALIDATED
    // against the freshly published list, not trusted.
    f.mobile.selectSession(QStringLiteral("s1"));
    f.answerLayouts(f.takeLayoutRequests(),
                    viewerTreeFor(QStringLiteral("viewer-1"),
                                  QStringLiteral("file:///srv/s1/a.md")),
                    terminalTreeFor(QStringLiteral("terminal-1"),
                                    QStringLiteral("row-1")));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Pane);
    QCOMPARE(f.mobile.selectedPaneKey(), QStringLiteral("terminal:terminal-1"));
}

// The pane the user last used can be closed from another client while they are
// away. The remembered key then names nothing, and the shell must land on the
// picker rather than on an empty pane - and must not report an error for a
// perfectly ordinary state.
void TstMobileNav::aRememberedPaneThatIsGoneLeavesTheUserOnThePicker()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});
    f.controller.uiState()->setSelectedPane(QStringLiteral("s1"),
                                            QStringLiteral("terminal:terminal-7"));

    f.mobile.selectSession(QStringLiteral("s1"));
    f.answerLayouts(f.takeLayoutRequests(),
                    viewerTreeFor(QStringLiteral("viewer-1"),
                                  QStringLiteral("file:///srv/s1/a.md")),
                    terminalTreeFor(QStringLiteral("terminal-1"),
                                    QStringLiteral("row-1")));

    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    QVERIFY(f.mobile.selectedPaneKey().isEmpty());
    QVERIFY(f.mobile.statusText().isEmpty());
}

// Selecting a pane the current list does not hold is a QUESTION, not a fault: the
// key may be a restored one, or a row a republish has already replaced. It must
// not move the user, and it must say something.
void TstMobileNav::anUnknownPaneKeyIsANoOpThatSaysWhy()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});
    f.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                  QStringLiteral("terminal-1"));

    QSignalSpy paneSpy(&f.mobile, &MobileAppController::selectedPaneChanged);
    f.mobile.selectPane(QStringLiteral("terminal:terminal-42"));

    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    QVERIFY(f.mobile.selectedPaneKey().isEmpty());
    QCOMPARE(paneSpy.count(), 0);
    QVERIFY(!f.mobile.statusText().isEmpty());
    // Nothing was persisted for a pane that was never opened.
    QCOMPARE(f.controller.uiState()->selectedPane(QStringLiteral("s1")),
             QString());
}

// A row can be tapped from a tree that has since been replaced - another client
// deleted or archived the session. AppController refuses the id (it validates
// against the last authoritative tree), so no layout is fetched at all, and the
// shell must say so instead of waiting for a load that was never issued.
void TstMobileNav::aSessionTheWorkspaceNoLongerHoldsIsRefused()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});
    f.transport.takeSent();

    f.mobile.selectSession(QStringLiteral("ghost"));

    QCOMPARE(f.mobile.navStage(), MobileAppController::Servers);
    QVERIFY(!f.mobile.layoutPending());
    QVERIFY(!f.mobile.statusText().isEmpty());
    QVERIFY(f.takeLayoutRequests().isEmpty());

    // An empty id is not a selection and is silently ignored: QML can bind a
    // delegate's itemId straight through without a guard of its own.
    const QString before = f.mobile.statusText();
    f.mobile.selectSession(QString());
    QCOMPARE(f.mobile.statusText(), before);
    QVERIFY(f.takeLayoutRequests().isEmpty());
}

// The active Dev Session is remembered under session/<serverId>/active - the key
// AppController already owns and the desktop already writes. This controller adds
// no key of its own, which is what lets AppController::restoreActiveSession()
// reopen the session on the next connect and the mobile shell simply follow it.
void TstMobileNav::theActiveSessionIsRecordedUnderTheExistingUiStateKey()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1"), QStringLiteral("s2")});

    f.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                  QStringLiteral("terminal-1"));
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s1"));
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));
    QCOMPARE(f.controller.activeSessionRepoRoot(), QStringLiteral("/srv/s1"));

    f.openSession(QStringLiteral("s2"), QStringLiteral("viewer-9"),
                  QStringLiteral("terminal-9"));
    QCOMPARE(f.controller.uiState()->activeSession(QStringLiteral("srv")),
             QStringLiteral("s2"));
}

// Guiless: everything here is QtCore/QtNetwork-free state over an in-process
// QIODevice, so the target runs identically on a headless CI machine.

// SPEC 12.1 keeps a secret for ONE authentication attempt. ch::MobileKeyStore's
// forgetSession() is the only thing that wipes imported key bytes, an armed
// passphrase and the identity installed on the SSH pool — and its contract says
// it runs on explicit disconnect. That wire is the thing under test here:
// asserting forgetSession() in isolation says nothing about whether anything
// ever calls it, which for a while nothing did.
void TstMobileNav::disconnectingDropsEveryCredentialTheStoreHolds()
{
    Fixture f;
    QTemporaryDir storeRoot;
    QVERIFY(storeRoot.isValid());
    MobileKeyStore keys(storeRoot.path());
    keys.setConnectionPool(&f.pool);
    f.mobile.setKeyStore(&keys);

    // ssh-keygen -t ed25519 -N '' — a throwaway pair that authenticates nothing.
    static const char *kKey =
        "-----BEGIN OPENSSH PRIVATE KEY-----\n"
        "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW\n"
        "QyNTUxOQAAACCFPLr0I6E8+7CRLJi8cjh4SciVWMEXlHzF/IIDXFBLIwAAAJDD6ACHw+gA\n"
        "hwAAAAtzc2gtZWQyNTUxOQAAACCFPLr0I6E8+7CRLJi8cjh4SciVWMEXlHzF/IIDXFBLIw\n"
        "AAAEB/HxJkLwbaZm48TCAx96WJuyrDUfAUyv9wZzQGfttws4U8uvQjoTz7sJEsmLxyOHhJ\n"
        "yJVYwReUfMX8ggNcUEsjAAAAB2NoLXRlc3QBAgMEBQY=\n"
        "-----END OPENSSH PRIVATE KEY-----\n";

    QVERIFY(keys.importKeyFromText(QStringLiteral("pasted"),
                                   QString::fromLatin1(kKey)));
    keys.armPassphrase(QStringLiteral("pasted"), QStringLiteral("hunter2"));
    QVERIFY(keys.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(f.pool.hasInMemoryIdentity());
    QVERIFY(keys.hasArmedPassphrase(QStringLiteral("pasted")));

    f.mobile.disconnect();

    // The credential is gone from the store AND from the pool, so nothing can
    // re-authenticate with it after the user asked to disconnect.
    QVERIFY(keys.keyNames().isEmpty());
    QVERIFY(!keys.hasArmedPassphrase(QStringLiteral("pasted")));
    QVERIFY(!f.pool.hasInMemoryIdentity());
    // And the navigation is back at the pre-connection stage.
    QCOMPARE(f.mobile.navStage(), MobileAppController::Servers);
}

// A layout load is a round trip, and the platform back gesture is bound to
// back() unconditionally - the busy veil covers the pages, not the back key. So
// the user can leave the session list while a load is on the wire, and the reply
// must not walk the shell forward from the connect page into a pane picker
// nobody is looking at any more.
void TstMobileNav::aBackGestureDuringALayoutLoadCancelsIt()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});
    QSignalSpy readySpy(&f.mobile, &MobileAppController::sessionReady);

    f.mobile.selectSession(QStringLiteral("s1"));
    const QVector<QJsonObject> requests = f.takeLayoutRequests();
    QCOMPARE(requests.size(), 2);
    QVERIFY(f.mobile.layoutPending());

    f.mobile.back();
    QCOMPARE(f.mobile.navStage(), MobileAppController::Servers);
    // The busy veil comes down with the stage: there is nothing left to wait for.
    QVERIFY(!f.mobile.layoutPending());

    // The reply lands anyway - it was already on the wire.
    f.answerLayouts(requests,
                    viewerTreeFor(QStringLiteral("viewer-1"),
                                  QStringLiteral("file:///srv/s1/a.md")),
                    terminalTreeFor(QStringLiteral("terminal-1"),
                                    QStringLiteral("row-1")));

    QCOMPARE(f.mobile.navStage(), MobileAppController::Servers);
    QCOMPARE(f.mobile.panes()->rowCount(), 0);
    QCOMPARE(readySpy.count(), 0);
}

// A region whose getLayout FAILS is reported through SessionLayouts::error() and
// the load then still reports itself finished. The explanation is the only thing
// standing between the user and a picker that says "This Dev Session has no
// panes yet." over a layout that could not be read, so finishing the load must
// not erase it.
void TstMobileNav::aFailedLayoutLoadKeepsItsExplanation()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});

    f.mobile.selectSession(QStringLiteral("s1"));
    const QVector<QJsonObject> requests = f.takeLayoutRequests();
    QCOMPARE(requests.size(), 2);
    for (const QJsonObject& request : requests) {
        f.transport.deliver(
            errorFrame(request.value(QStringLiteral("id")).toInt(),
                       QStringLiteral("layout unreadable")));
    }

    // The load is over - the busy veil must come down - and the picker is empty,
    // but the reason is still on screen.
    QVERIFY(!f.mobile.layoutPending());
    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    QCOMPARE(f.mobile.panes()->rowCount(), 0);
    QCOMPARE(f.mobile.statusText(), QStringLiteral("layout unreadable"));

    // And the NEXT load owns the line again: a stale failure must not outlive
    // the session it was about.
    f.mobile.selectSession(QStringLiteral("s1"));
    QCOMPARE(f.mobile.statusText(), QStringLiteral("Opening session…"));
    f.answerLayouts(f.takeLayoutRequests(),
                    viewerTreeFor(QStringLiteral("viewer-1"),
                                  QStringLiteral("file:///srv/s1/a.md")),
                    terminalTreeFor(QStringLiteral("terminal-1"),
                                    QStringLiteral("row-1")));
    QVERIFY(f.mobile.statusText().isEmpty());
}

// A pane records what it opens back into the layout tree, and SessionLayouts
// updates that tree QUIETLY (republishing it would destroy the very pane that
// just opened the file). The pane list was published when the session loaded, so
// coming back to the picker has to re-read the trees - otherwise reopening the
// pane reopens the file it started on and undoes the navigation the layout has
// already recorded.
void TstMobileNav::returningToThePickerRepublishesWhatThePaneOpened()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});
    f.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                  QStringLiteral("terminal-1"));
    f.mobile.selectPane(QStringLiteral("viewer:viewer-1"));
    QCOMPARE(f.mobile.selectedPane().value(QStringLiteral("url")).toString(),
             QStringLiteral("file:///srv/s1/notes.md"));

    // The pane browses to another file. Deliberately NOT republished by
    // SessionLayouts, so the list still names the old one.
    f.layouts.setPaneUrl(QStringLiteral("viewer"), QStringLiteral("viewer-1"),
                         QStringLiteral("file:///srv/s1/main.cpp"));
    f.transport.takeSent();
    QCOMPARE(f.mobile.panes()
                 ->data(f.mobile.panes()->index(0, 0),
                        PaneListModel::UrlRole).toString(),
             QStringLiteral("file:///srv/s1/notes.md"));

    f.mobile.back();
    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    // Same rows, in the same order, now describing what the pane really has open
    // - including the KIND, which is what decides the page that opens.
    QCOMPARE(paneKeys(f.mobile.panes()),
             QStringList({QStringLiteral("viewer:viewer-1"),
                          QStringLiteral("terminal:terminal-1")}));
    const QVariantMap pane =
        f.mobile.panes()->paneByKey(QStringLiteral("viewer:viewer-1"));
    QCOMPARE(pane.value(QStringLiteral("url")).toString(),
             QStringLiteral("file:///srv/s1/main.cpp"));
    QCOMPARE(pane.value(QStringLiteral("title")).toString(),
             QStringLiteral("main.cpp"));
    QCOMPARE(pane.value(QStringLiteral("kind")).toString(),
             QStringLiteral("text"));
}

// selectSession() is not the only way a Dev Session becomes active: the shared
// restore path (AppController::restoreActiveSession) activates one out of its own
// refresh handler, and this shell has to follow that without a second copy of the
// remembering. adoptActiveSession() is what does it, and this is the case that
// drives it from the OUTSIDE.
void TstMobileNav::theShellFollowsASessionActivatedByAnybodyElse()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});

    f.controller.activateSession(QStringLiteral("s1"));

    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);
    QVERIFY(f.mobile.layoutPending());
    f.answerLayouts(f.takeLayoutRequests(),
                    viewerTreeFor(QStringLiteral("viewer-1"),
                                  QStringLiteral("file:///srv/s1/a.md")),
                    terminalTreeFor(QStringLiteral("terminal-1"),
                                    QStringLiteral("row-1")));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    QCOMPARE(paneKeys(f.mobile.panes()),
             QStringList({QStringLiteral("viewer:viewer-1"),
                          QStringLiteral("terminal:terminal-1")}));
}

// Another client can delete or archive the Dev Session the user is sitting in.
// The next refresh retires it, and the pane the user is looking at describes a
// channel that no longer exists - so the shell must fall back to the session
// list and let go of it.
void TstMobileNav::aSessionThatVanishesFromTheWorkspaceReleasesThePane()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});
    f.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                  QStringLiteral("terminal-1"));
    f.mobile.selectPane(QStringLiteral("terminal:terminal-1"));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Pane);

    // The authoritative tree no longer holds it.
    f.listSessions({});

    QCOMPARE(f.controller.activeSessionId(), QString());
    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);
    QVERIFY(f.mobile.selectedPaneKey().isEmpty());
    QVERIFY(f.mobile.selectedPane().isEmpty());
    QCOMPARE(f.mobile.panes()->rowCount(), 0);
    QVERIFY(!f.mobile.layoutPending());
}

// Re-picking the session already on screen is the user's natural retry, and
// AppController deliberately does NOT short-circuit it. The shell must therefore
// treat it as a genuine reload - arm, wait, republish - and not as a no-op that
// leaves the busy veil up for a reply it decided to ignore.
void TstMobileNav::reselectingTheSameSessionReloadsItRatherThanHanging()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});
    f.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                  QStringLiteral("terminal-1"));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);

    f.mobile.selectSession(QStringLiteral("s1"));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);
    QVERIFY(f.mobile.layoutPending());
    QCOMPARE(f.mobile.panes()->rowCount(), 0);

    f.answerLayouts(f.takeLayoutRequests(),
                    viewerTreeFor(QStringLiteral("viewer-2"),
                                  QStringLiteral("file:///srv/s1/b.md")),
                    terminalTreeFor(QStringLiteral("terminal-2"),
                                    QStringLiteral("row-2")));
    QCOMPARE(f.mobile.navStage(), MobileAppController::Panes);
    QVERIFY(!f.mobile.layoutPending());
    QCOMPARE(paneKeys(f.mobile.panes()),
             QStringList({QStringLiteral("viewer:viewer-2"),
                          QStringLiteral("terminal:terminal-2")}));
}

// Every sentence written while an attempt is in flight ("Connecting…",
// "Installing the CodeHarbor remote service…", a refusal) is about something that
// is now over once the connection settles. Leaving one standing told a connected
// user, on the session list, that the shell was still connecting.
void TstMobileNav::aSettledConnectionStopsClaimingToBeBusy()
{
    Fixture f;
    f.listSessions({QStringLiteral("s1")});

    // Any in-flight sentence will do; a refused session is the one this fixture
    // can produce without a server.
    f.mobile.selectSession(QStringLiteral("ghost"));
    QVERIFY(!f.mobile.statusText().isEmpty());

    // The state has come to rest (this fixture never connects, so rest is
    // "disconnected"), which is what AppController announces with this signal.
    QVERIFY(QMetaObject::invokeMethod(&f.controller, "connectionStateChanged"));

    QCOMPARE(f.mobile.connectionState(), QStringLiteral("disconnected"));
    QVERIFY(f.mobile.statusText().isEmpty());
}

// ServerProfiles refuses an edit whose merged result could never connect, and
// says so by leaving the stored profile alone. Dialling anyway would connect to
// the PREVIOUSLY stored endpoint while the user looks at the form they just
// changed - a success against the wrong server, which is worse than a refusal.
void TstMobileNav::aRefusedProfileEditIsReportedRatherThanDiallingTheOldOne()
{
    Fixture f;
    QTemporaryDir storeRoot;
    QVERIFY(storeRoot.isValid());
    ServerProfiles profiles(storeRoot.filePath(QStringLiteral("servers.ini")));
    f.controller.setConnection(nullptr, nullptr, &profiles, &f.layouts);

    QVariantMap profile{{QStringLiteral("name"), QStringLiteral("box")},
                        {QStringLiteral("host"), QStringLiteral("box.example")},
                        {QStringLiteral("port"), 22},
                        {QStringLiteral("user"), QStringLiteral("dev")}};
    f.mobile.connectToServer(profile);
    const QString id = profiles.activeId();
    QVERIFY(!id.isEmpty());
    QCOMPARE(profiles.profile(id).value(QStringLiteral("host")).toString(),
             QStringLiteral("box.example"));

    // The same profile, edited into something unconnectable.
    profile.insert(QStringLiteral("id"), id);
    profile.insert(QStringLiteral("user"), QString());
    f.mobile.connectToServer(profile);

    QCOMPARE(f.mobile.statusText(),
             QStringLiteral("That server needs at least a host name and a user "
                            "name, and a port between 1 and 65535."));
    // The stored profile is intact - a bad edit may not corrupt a working
    // profile - which is exactly why the refusal has to be reported.
    QCOMPARE(profiles.profile(id).value(QStringLiteral("user")).toString(),
             QStringLiteral("dev"));

    // And a valid edit of the same profile still goes through, in place.
    profile.insert(QStringLiteral("user"), QStringLiteral("ops"));
    f.mobile.connectToServer(profile);
    QCOMPARE(profiles.profiles().size(), 1);
    QCOMPARE(profiles.profile(id).value(QStringLiteral("user")).toString(),
             QStringLiteral("ops"));

    // Unwired before the store leaves scope: it is declared after the fixture,
    // so it dies FIRST, and a controller still holding it would be pointing at
    // freed memory for the rest of the teardown.
    f.controller.setConnection(nullptr, nullptr, nullptr, &f.layouts);
}

// A terminal session's whole lifetime rule is "it dies with the page that asked
// for it". A session with no owner has nothing to die with: parented to the
// controller it would hold a PTY channel open for the rest of the run, behind
// whatever the user looks at next. So it is refused.
void TstMobileNav::anOwnerlessTerminalSessionIsRefused()
{
    Fixture f;
    QObject owner;
    QVERIFY(f.mobile.createTerminalSession(&owner) != nullptr);
    QVERIFY(f.mobile.createTerminalSession(nullptr) == nullptr);
    // And nothing was parented onto the controller as a consolation prize.
    QVERIFY(f.mobile.findChildren<MobileTerminalSession*>().isEmpty());
}

QTEST_GUILESS_MAIN(TstMobileNav)
#include "tst_mobilenav.moc"
