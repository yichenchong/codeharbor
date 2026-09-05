#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QJsonObject>
#include <QPointF>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRectF>
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

namespace {

// ---- finding an item in the real shell -------------------------------------
//
// The two cases at the end of this file drive the REAL QML pages, because the
// question they ask - "is there something on this screen the user can tap to get
// back to the server form" - is exactly the one a C++-only case cannot answer:
// ch::MobileAppController::back() has always done the right thing, and the
// session picker still had no affordance bound to it.
//
// Everything is found STRUCTURALLY, by walking the item tree, for the reason
// tst_mobileshell spells out: a compiled QML module's ids are local to the
// component and unreachable from outside it, and adding objectNames to a page
// purely so a test can find it puts scaffolding into production QML. These are
// deliberately private copies of that approach rather than a reach into the
// other gate's file.
QString qmlTypeName(QObject* object)
{
    if (!object)
        return {};
    QString name = QString::fromLatin1(object->metaObject()->className());
    const qsizetype marker = name.indexOf(QLatin1String("_QMLTYPE_"));
    if (marker >= 0)
        name.truncate(marker);
    return name;
}

template <typename Predicate>
QQuickItem* findItem(QQuickItem* item, Predicate matches)
{
    if (!item)
        return nullptr;
    if (matches(item))
        return item;
    const QList<QQuickItem*> children = item->childItems();
    for (QQuickItem* child : children) {
        if (QQuickItem* found = findItem(child, matches))
            return found;
    }
    return nullptr;
}

// Match either spelling of a Quick type: an unstyled type is C++
// ("QQuickStackView"), while a styled Controls type is QML-defined and arrives
// as "StackView_QMLTYPE_55_QML_63".
QQuickItem* findByType(QQuickItem* root, const QString& name)
{
    const QString cppName = QStringLiteral("QQuick") + name;
    return findItem(root, [&name, &cppName](QQuickItem* item) {
        const QString className =
            QString::fromLatin1(item->metaObject()->className());
        return className == cppName || className == name
               || qmlTypeName(item) == name;
    });
}

// The page the StackView is showing, through its public `currentItem` property
// rather than its id.
QQuickItem* currentPage(QQuickWindow* window)
{
    QQuickItem* stack =
        findByType(window->contentItem(), QStringLiteral("StackView"));
    return stack ? stack->property("currentItem").value<QQuickItem*>() : nullptr;
}

// The first item under `root` whose `text` property reads exactly `text`. It is
// how a case checks what the header is SHOWING rather than which property it
// happens to be bound to.
QQuickItem* findItemShowing(QQuickItem* root, const QString& text)
{
    return findItem(root, [&text](QQuickItem* candidate) {
        const QVariant value = candidate->property("text");
        return value.isValid() && value.toString() == text;
    });
}

// The button `item` is the content of, if any. A glyph in a header is a Text
// INSIDE an AbstractButton, and it is the button that has to be tapped and that
// carries `enabled` - a Text is not clickable and would report nothing.
QQuickItem* enclosingButton(QQuickItem* item)
{
    for (QQuickItem* candidate = item; candidate;
         candidate = candidate->parentItem()) {
        if (candidate->inherits("QQuickAbstractButton"))
            return candidate;
    }
    return nullptr;
}

}  // namespace

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

    // ---- the server form: reachable, and editable ------------------------
    void theSessionPickerOffersATapBackToTheServerForm();
    void steppingBackToTheServerFormKeepsTheConnectionAndItsCredential();
    void theConnectFormSavesTheServerItIsShowingOnATap();
    void steppingBackToTheFormEditsTheServerRatherThanDuplicatingIt();
    void anEditedProfileRoundTripsThroughTheSharedStore();
    void nothingSecretInTheFormReachesTheProfileStore();

private:
    // The real shell over the same fake codeharbord, for the cases that have
    // to tap a real page. One engine per case: the shell keeps per-session
    // state, so a case that inherited another's stack would be asserting on
    // somebody else's walk.
    //
    // No type registration and no hand-written pages: ch_mobile's own QML module
    // registers every C++ type QML names, so this host loads exactly the module
    // a device loads. terminalFactory, keyStore and viewerService are
    // deliberately NOT installed - every page guards its context properties, and
    // their absence exercises that degraded path rather than a configuration no
    // real host uses.
    struct Shell {
        Fixture fixture;
        QQmlApplicationEngine engine;

        Shell()
        {
            engine.rootContext()->setContextProperty(QStringLiteral("mobile"),
                                                     &fixture.mobile);
            engine.rootContext()->setContextProperty(QStringLiteral("app"),
                                                     &fixture.controller);
            engine.rootContext()->setContextProperty(QStringLiteral("layouts"),
                                                     &fixture.layouts);
            engine.loadFromModule(QStringLiteral("CodeHarbor.Mobile"),
                                  QStringLiteral("MobileMain"));
        }

        QQuickWindow* window() const
        {
            if (engine.rootObjects().isEmpty())
                return nullptr;
            return qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        }
    };
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

// HEADLESS, not guiless: every case here is state over an in-process QIODevice
// and needs no network and no server, but the last two load the real QML shell
// to tap a real button, so the target runs under QGuiApplication on the
// offscreen platform. Nothing about it needs a display or a device.

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

// ---- the server form: reachable, and editable ------------------------------

// The user's report was "there is no option anywhere to go back to the server
// settings". back() was never the broken half - this very file has asserted the
// Sessions -> Servers step since the beginning - so the case has to be driven
// through the real page and a real tap, which is the only thing that can tell
// "the controller can do it" from "the user can ask for it".
void TstMobileNav::theSessionPickerOffersATapBackToTheServerForm()
{
    Shell shell;
    QQuickWindow* window = shell.window();
    QVERIFY(window);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));

    shell.fixture.listSessions({QStringLiteral("s1")});
    // Opened and then stepped back to the list, rather than left mid-load: a
    // pending layout raises the busy veil over the whole window, and a tap on a
    // veiled page proves nothing about the page.
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));
    shell.fixture.mobile.back();
    QVERIFY(!shell.fixture.mobile.layoutPending());
    QQuickItem* page = currentPage(window);
    QCOMPARE(qmlTypeName(page), QStringLiteral("SessionPickerPage"));

    // The affordance the rest of the shell uses: the "\u2039" glyph in the
    // header, in the same place PanePickerPage and PaneHostPage put it.
    QQuickItem* glyph = findItemShowing(page, QStringLiteral("\u2039"));
    QVERIFY2(glyph != nullptr,
             "the session picker header shows no back glyph, so the connect "
             "form is reachable only by Android's hardware key");
    QQuickItem* button = enclosingButton(glyph);
    QVERIFY2(button != nullptr, "the back glyph is not inside a button");
    QVERIFY2(button->isEnabled(), "the back button is disabled");

    // Inside the window, or the click is dropped by the platform and the case
    // would pass on a button no thumb can reach.
    const QPointF centre = button->mapToScene(
        QPointF(button->width() / 2.0, button->height() / 2.0));
    QVERIFY2(QRectF(0, 0, window->width(), window->height()).contains(centre),
             "the back button is outside the window");

    // Wait out the stack transition first. StackView refuses pointer input while
    // it is animating a page in or out - measured: a click sent with `busy` true
    // never reaches the button and the stage does not move - so a case that
    // tapped immediately would report "no affordance" for a button that works.
    QQuickItem* stack =
        findByType(window->contentItem(), QStringLiteral("StackView"));
    QVERIFY(stack != nullptr);
    QTRY_VERIFY(!stack->property("busy").toBool());

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());

    QTRY_COMPARE(shell.fixture.mobile.navStage(), MobileAppController::Servers);
    // And the shell really shows the form, rather than only recording the
    // stage: the whole complaint was about what is on the screen.
    QTRY_COMPARE(qmlTypeName(currentPage(window)), QStringLiteral("ConnectPage"));
}

// The editable half, tapped rather than invoked: the store round trip below
// proves the WRITE is right, and this proves the form is wired to it. A page
// whose Save button called nothing would pass every store-level case in this
// file, which is precisely the shape of the defect being fixed.
void TstMobileNav::theConnectFormSavesTheServerItIsShowingOnATap()
{
    Shell shell;
    QTemporaryDir storeRoot;
    QVERIFY(storeRoot.isValid());
    ServerProfiles profiles(storeRoot.filePath(QStringLiteral("servers.ini")));
    shell.fixture.controller.setConnection(nullptr, nullptr, &profiles,
                                           &shell.fixture.layouts);

    QQuickWindow* window = shell.window();
    QVERIFY(window);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));

    QQuickItem* page = currentPage(window);
    QCOMPARE(qmlTypeName(page), QStringLiteral("ConnectPage"));

    // The form's own state properties, which are what its fields write on every
    // keystroke (onTextEdited). Typing character by character would be testing
    // TextField.
    page->setProperty("nameText", QStringLiteral("Prod box"));
    page->setProperty("hostText", QStringLiteral("box.example"));
    page->setProperty("userText", QStringLiteral("dev"));
    page->setProperty("portText", QStringLiteral("2222"));
    page->setProperty("nodePathText", QStringLiteral("/usr/bin/node"));
    page->setProperty("repoRootText", QStringLiteral("/srv/codeharbor"));
    QTRY_VERIFY(page->property("formValid").toBool());

    // Found by objectName, which is THIS page's own convention - every control
    // on it already carries one - rather than the structural search the
    // objectName-free pages need.
    QQuickItem* save = page->findChild<QQuickItem*>(QStringLiteral("saveServerButton"));
    QVERIFY2(save != nullptr, "the connect form has no save control at all");
    QTRY_VERIFY(save->isEnabled());

    // It sits at the bottom of a scrolling column, so it has to be scrolled to
    // before it can be tapped: a click outside the window is dropped, and one
    // sent while the stack is still animating never arrives.
    QQuickItem* flick = findByType(page, QStringLiteral("Flickable"));
    QVERIFY(flick != nullptr);
    flick->setProperty("contentY",
                       qMax(0.0, flick->property("contentHeight").toReal()
                                     - flick->height()));
    QQuickItem* stack =
        findByType(window->contentItem(), QStringLiteral("StackView"));
    QVERIFY(stack != nullptr);
    QTRY_VERIFY(!stack->property("busy").toBool());

    const QRectF inside(0, 0, window->width(), window->height());
    QTRY_VERIFY(inside.contains(save->mapToScene(
        QPointF(save->width() / 2.0, save->height() / 2.0))));
    const QPointF centre =
        save->mapToScene(QPointF(save->width() / 2.0, save->height() / 2.0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());

    // One profile, holding what the form was showing - and nothing was dialled.
    QTRY_COMPARE(profiles.profiles().size(), 1);
    const QString id = profiles.profiles().first().toMap()
                           .value(QStringLiteral("id")).toString();
    const QVariantMap stored = profiles.profile(id);
    QCOMPARE(stored.value(QStringLiteral("name")).toString(),
             QStringLiteral("Prod box"));
    QCOMPARE(stored.value(QStringLiteral("host")).toString(),
             QStringLiteral("box.example"));
    QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 2222);
    QCOMPARE(stored.value(QStringLiteral("user")).toString(),
             QStringLiteral("dev"));
    QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(),
             QStringLiteral("/usr/bin/node"));
    QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(),
             QStringLiteral("/srv/codeharbor"));
    QCOMPARE(shell.fixture.mobile.navStage(), MobileAppController::Servers);

    // The page adopted the new id, so the NEXT save edits this entry instead of
    // adding a second one - which is the difference between a save button and a
    // duplicate factory.
    QTRY_COMPARE(page->property("profileId").toString(), id);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    QTRY_COMPARE(profiles.profiles().size(), 1);

    // Unwired before the store leaves scope: it is declared after the shell, so
    // it dies FIRST.
    shell.fixture.controller.setConnection(nullptr, nullptr, nullptr,
                                           &shell.fixture.layouts);
}

// The whole reported journey in one case: get back to the server form, change
// something, connect. Each half was covered separately and the join was not,
// which is where the remaining defect lived - the way back reached a BLANK
// form, so a user who retyped their server and connected got a second copy of
// it rather than an edited one. A profile is only updated in place when the
// form carries its id, and nothing put the id there on the way back.
void TstMobileNav::steppingBackToTheFormEditsTheServerRatherThanDuplicatingIt()
{
    QTemporaryDir storeRoot;
    QVERIFY(storeRoot.isValid());
    const QString ini = storeRoot.filePath(QStringLiteral("servers.ini"));

    Shell shell;
    ServerProfiles profiles(ini);
    shell.fixture.controller.setConnection(nullptr, nullptr, &profiles,
                                           &shell.fixture.layouts);

    QQuickWindow* window = shell.window();
    QVERIFY(window);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));

    // A remembered server, stored the way connecting stores it. Setup, not the
    // assertion: connectToServer() persisting is covered on its own above.
    shell.fixture.mobile.connectToServer(
        {{QStringLiteral("name"), QStringLiteral("Prod")},
         {QStringLiteral("host"), QStringLiteral("box.example")},
         {QStringLiteral("port"), 22},
         {QStringLiteral("user"), QStringLiteral("dev")},
         {QStringLiteral("nodePath"), QStringLiteral("/usr/bin/node")}});
    const QString id = profiles.activeId();
    QVERIFY(!id.isEmpty());
    QCOMPARE(profiles.profiles().size(), 1);

    // Out to the Dev Session list and back through the header glyph, which is
    // the route the user has. Opened and then stepped back rather than left
    // mid-load, exactly as the reachability case does: a pending layout raises
    // the busy veil, and a tap on a veiled page proves nothing.
    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));
    shell.fixture.mobile.back();
    QVERIFY(!shell.fixture.mobile.layoutPending());
    QTRY_COMPARE(shell.fixture.mobile.navStage(), MobileAppController::Sessions);
    QQuickItem* picker = currentPage(window);
    QCOMPARE(qmlTypeName(picker), QStringLiteral("SessionPickerPage"));

    QQuickItem* glyph = findItemShowing(picker, QStringLiteral("\u2039"));
    QVERIFY(glyph != nullptr);
    QQuickItem* back = enclosingButton(glyph);
    QVERIFY(back != nullptr);
    QQuickItem* stack =
        findByType(window->contentItem(), QStringLiteral("StackView"));
    QVERIFY(stack != nullptr);
    QTRY_VERIFY(!stack->property("busy").toBool());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      back->mapToScene(QPointF(back->width() / 2.0,
                                               back->height() / 2.0)).toPoint());

    QTRY_COMPARE(qmlTypeName(currentPage(window)), QStringLiteral("ConnectPage"));
    QQuickItem* form = currentPage(window);

    // The form is an EDIT of the server just left, not an empty one.
    QTRY_COMPARE(form->property("profileId").toString(), id);
    QCOMPARE(form->property("hostText").toString(),
             QStringLiteral("box.example"));
    QCOMPARE(form->property("userText").toString(), QStringLiteral("dev"));

    // Change something and connect, which is what the user came back to do.
    form->setProperty("hostText", QStringLiteral("moved.example"));
    QTRY_VERIFY(form->property("formValid").toBool());

    QQuickItem* connect =
        form->findChild<QQuickItem*>(QStringLiteral("connectButton"));
    QVERIFY2(connect != nullptr, "the connect form has no connect button");
    QQuickItem* flick = findByType(form, QStringLiteral("Flickable"));
    QVERIFY(flick != nullptr);
    flick->setProperty("contentY",
                       qMax(0.0, flick->property("contentHeight").toReal()
                                     - flick->height()));
    QTRY_VERIFY(!stack->property("busy").toBool());
    const QRectF inside(0, 0, window->width(), window->height());
    QTRY_VERIFY(connect->isEnabled()
                && inside.contains(connect->mapToScene(
                    QPointF(connect->width() / 2.0, connect->height() / 2.0))));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      connect->mapToScene(
                          QPointF(connect->width() / 2.0,
                                  connect->height() / 2.0)).toPoint());

    // ONE profile, carrying the edit. Two would mean the id never made it back
    // to the form - the defect this case exists for.
    QTRY_COMPARE(profiles.profile(id).value(QStringLiteral("host")).toString(),
                 QStringLiteral("moved.example"));
    QCOMPARE(profiles.profiles().size(), 1);

    shell.fixture.controller.setConnection(nullptr, nullptr, nullptr,
                                           &shell.fixture.layouts);
}

// WHAT THAT TAP COSTS, pinned so the choice is recorded rather than remembered.
//
// The button calls back(), NOT disconnect(): the stage moves to Servers and
// nothing else happens, so the SSH session stays up, the credential the store
// holds stays live, and the active Dev Session is still active - going back to
// the list costs no second handshake. disconnect() is the other thing entirely,
// and this case drives both so the difference is not a matter of opinion.
void TstMobileNav::steppingBackToTheServerFormKeepsTheConnectionAndItsCredential()
{
    Fixture f;
    QTemporaryDir storeRoot;
    QVERIFY(storeRoot.isValid());
    MobileKeyStore keys(storeRoot.path());
    keys.setConnectionPool(&f.pool);
    f.mobile.setKeyStore(&keys);

    // ssh-keygen -t ed25519 -N '' — the same throwaway pair the disconnect case
    // uses; it authenticates nothing.
    static const char* kKey =
        "-----BEGIN OPENSSH PRIVATE KEY-----\n"
        "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW\n"
        "QyNTUxOQAAACCFPLr0I6E8+7CRLJi8cjh4SciVWMEXlHzF/IIDXFBLIwAAAJDD6ACHw+gA\n"
        "hwAAAAtzc2gtZWQyNTUxOQAAACCFPLr0I6E8+7CRLJi8cjh4SciVWMEXlHzF/IIDXFBLIw\n"
        "AAAEB/HxJkLwbaZm48TCAx96WJuyrDUfAUyv9wZzQGfttws4U8uvQjoTz7sJEsmLxyOHhJ\n"
        "yJVYwReUfMX8ggNcUEsjAAAAB2NoLXRlc3QBAgMEBQY=\n"
        "-----END OPENSSH PRIVATE KEY-----\n";
    QVERIFY(keys.importKeyFromText(QStringLiteral("pasted"),
                                   QString::fromLatin1(kKey)));
    QVERIFY(keys.applyIdentityForConnect(QStringLiteral("pasted")));

    f.listSessions({QStringLiteral("s1")});
    f.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                  QStringLiteral("terminal-1"));
    f.mobile.back();
    QCOMPARE(f.mobile.navStage(), MobileAppController::Sessions);

    f.mobile.back();

    QCOMPARE(f.mobile.navStage(), MobileAppController::Servers);
    // Nothing was torn down: the identity is still installed on the pool, the
    // key is still in the store, and the workspace still has a session active.
    QVERIFY(f.pool.hasInMemoryIdentity());
    QCOMPARE(keys.keyNames(), QStringList({QStringLiteral("pasted")}));
    QCOMPARE(f.controller.activeSessionId(), QStringLiteral("s1"));

    // The contrast, in the same case: disconnect() DOES take all of it.
    f.mobile.disconnect();
    QVERIFY(!f.pool.hasInMemoryIdentity());
    QVERIFY(keys.keyNames().isEmpty());
}

// The other half of the report: the mobile connect form could select a
// remembered server and edit its fields in memory, and nothing ever wrote them
// back. saveServer() is that write, through the SAME ch::ServerProfiles the
// connect path uses, so a saved profile is one the form can offer again - which
// is only true if every field the form needs survives a trip to disk.
void TstMobileNav::anEditedProfileRoundTripsThroughTheSharedStore()
{
    Fixture f;
    QTemporaryDir storeRoot;
    QVERIFY(storeRoot.isValid());
    const QString ini = storeRoot.filePath(QStringLiteral("servers.ini"));
    ServerProfiles profiles(ini);
    f.controller.setConnection(nullptr, nullptr, &profiles, &f.layouts);

    QVariantMap form{
        {QStringLiteral("name"), QStringLiteral("Prod box")},
        {QStringLiteral("host"), QStringLiteral("box.example")},
        {QStringLiteral("port"), 2222},
        {QStringLiteral("user"), QStringLiteral("dev")},
        {QStringLiteral("identityFile"),
         QStringLiteral("/home/dev/.ssh/id_ed25519")},
        // REQUIRED by the form's own validation, so a save that dropped it would
        // remember a server that cannot connect.
        {QStringLiteral("nodePath"), QStringLiteral("/usr/bin/node")},
        {QStringLiteral("repoRoot"), QStringLiteral("/srv/codeharbor")}};

    const QString id = f.mobile.saveServer(form);
    QVERIFY(!id.isEmpty());
    // Saved WITHOUT dialling: this is the entry point the phone lacked, and a
    // save that connected would be a second connect path.
    QVERIFY(f.takeLayoutRequests().isEmpty());
    QCOMPARE(f.mobile.connectionState(), QStringLiteral("disconnected"));
    // The one confirmation the page can give, naming the entry as stored.
    QCOMPARE(f.mobile.statusText(), QStringLiteral("Saved \u201cProd box\u201d."));

    // A rename and an edit of the same profile, which is what the form does on
    // a second Save: an EDIT in place, never a second entry.
    form.insert(QStringLiteral("id"), id);
    form.insert(QStringLiteral("name"), QStringLiteral("Prod"));
    form.insert(QStringLiteral("repoRoot"), QStringLiteral("/srv/other"));
    QCOMPARE(f.mobile.saveServer(form), id);
    QCOMPARE(profiles.profiles().size(), 1);

    // Re-read through a SECOND store on the same file: an in-memory list would
    // agree with itself even if nothing reached disk.
    {
        ServerProfiles reread(ini);
        QCOMPARE(reread.profiles().size(), 1);
        const QVariantMap stored = reread.profile(id);
        QCOMPARE(stored.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Prod"));
        QCOMPARE(stored.value(QStringLiteral("host")).toString(),
                 QStringLiteral("box.example"));
        QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 2222);
        QCOMPARE(stored.value(QStringLiteral("user")).toString(),
                 QStringLiteral("dev"));
        QCOMPARE(stored.value(QStringLiteral("identityFile")).toString(),
                 QStringLiteral("/home/dev/.ssh/id_ed25519"));
        QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(),
                 QStringLiteral("/usr/bin/node"));
        QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(),
                 QStringLiteral("/srv/other"));
    }

    // Unwired before the store leaves scope: it is declared after the fixture,
    // so it dies FIRST, and a controller still holding it would be pointing at
    // freed memory for the rest of the teardown.
    f.controller.setConnection(nullptr, nullptr, nullptr, &f.layouts);
}

// SPEC 11.2 permits a profile to record connection metadata and never a secret,
// and ServerProfiles' whitelist is what enforces it. The new save path must not
// be a way around that: the connect form holds a passphrase field, and the map a
// page hands over is the kind of place a secret ends up by accident.
void TstMobileNav::nothingSecretInTheFormReachesTheProfileStore()
{
    Fixture f;
    QTemporaryDir storeRoot;
    QVERIFY(storeRoot.isValid());
    const QString ini = storeRoot.filePath(QStringLiteral("servers.ini"));
    ServerProfiles profiles(ini);
    f.controller.setConnection(nullptr, nullptr, &profiles, &f.layouts);

    const QVariantMap form{
        {QStringLiteral("name"), QStringLiteral("box")},
        {QStringLiteral("host"), QStringLiteral("box.example")},
        {QStringLiteral("port"), 22},
        {QStringLiteral("user"), QStringLiteral("dev")},
        {QStringLiteral("identityFile"), QString()},
        {QStringLiteral("nodePath"), QStringLiteral("/usr/bin/node")},
        {QStringLiteral("repoRoot"), QStringLiteral("/srv/codeharbor")},
        // Three shapes of the same accident.
        {QStringLiteral("password"), QStringLiteral("hunter2")},
        {QStringLiteral("passphrase"), QStringLiteral("hunter2")},
        {QStringLiteral("privateKey"), QStringLiteral("hunter2")}};

    const QString id = f.mobile.saveServer(form);
    QVERIFY(!id.isEmpty());

    const QVariantMap stored = profiles.profile(id);
    QVERIFY(!stored.contains(QStringLiteral("password")));
    QVERIFY(!stored.contains(QStringLiteral("passphrase")));
    QVERIFY(!stored.contains(QStringLiteral("privateKey")));

    // And the file itself, because "not in the map we read back" is not the same
    // claim as "not on disk".
    QFile file(ini);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    QVERIFY(!contents.isEmpty());
    QVERIFY2(!contents.contains("hunter2"),
             qPrintable(QStringLiteral("a secret handed to saveServer() reached "
                                       "%1").arg(ini)));
    // The metadata SPEC 11.2 does allow is there, so the check above is not
    // passing on an empty file.
    QVERIFY(contents.contains("box.example"));

    f.controller.setConnection(nullptr, nullptr, nullptr, &f.layouts);
}

// The Material style and a QGuiApplication rather than QTEST_GUILESS_MAIN: the
// two shell cases load the real QML module, and Controls needs a style resolved
// before the first component is created. Everything else in this file is
// unaffected - a QGuiApplication on the offscreen platform is still headless.
int main(int argc, char* argv[])
{
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication app(argc, argv);
    TstMobileNav test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_mobilenav.moc"
