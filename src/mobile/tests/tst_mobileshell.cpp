// The mobile shell as the user meets it: the REAL MobileMain.qml, loaded from the
// real CodeHarbor.Mobile module, driven through the two-step walk over a fake
// codeharbord.
//
// WHY THIS EXISTS BESIDE tst_mobilenav. That gate proves the state machine:
// stages advance in order, a stale layout is discarded, exactly one pane is ever
// selected. None of that says a single page RENDERS, that the session picker's
// nested delegate model produces rows for a two-level QAbstractItemModel, or that
// PaneHostPage's kind -> page table actually resolves to a component that loads.
// Those are the failures that reach a user first, and they are invisible to a
// C++-only gate — a mistyped page name in the routing table costs nothing at
// compile time and shows a blank screen on a phone.
//
// Every case runs offscreen against an in-process transport: no SSH server, no
// display, no device. The frame grabs are written to the build tree (or to
// CH_SHELL_FRAME_DIR) so a human can look at what the assertions describe.

#include <QtTest/QtTest>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QVariant>

#include "EditorFactory.h"
#include "MobileNavFixture.h"
#include "MobileTerminalSession.h"
#include "MobileTerminalView.h"
#include "MobileViewerService.h"
#include "TerminalController.h"

using namespace ch;
using namespace chtest;

namespace {

// The QML type name of an object, without the engine's instance suffix:
// "PanePickerPage_QMLTYPE_57" -> "PanePickerPage". That suffix is an engine
// implementation detail and would make every assertion here brittle.
QString qmlTypeName(QObject *object)
{
    if (!object)
        return {};
    QString name = QString::fromLatin1(object->metaObject()->className());
    const qsizetype marker = name.indexOf(QLatin1String("_QMLTYPE_"));
    if (marker >= 0)
        name.truncate(marker);
    return name;
}

// EVERYTHING here is found STRUCTURALLY — by walking the item tree and matching
// types — rather than by `id` or `objectName`. Two reasons, both load bearing:
//
//   * A compiled QML module's ids are local to the component; they are not
//     reachable from an expression evaluated outside it, so an id-based probe
//     tests nothing and reports a ReferenceError.
//   * Adding objectNames to the pages purely so a test can find them puts test
//     scaffolding into production QML. The desktop gates refuse to do that for
//     the same reason (see src/qml/tests and the comment in tst_liveshell about
//     finding the pane's view structurally), and this module follows suit.
//
// The cost is that these helpers match on Quick type names. Those are stable
// across a Qt minor, and a rename fails loudly here rather than silently passing.
template <typename Predicate>
QQuickItem *findItem(QQuickItem *item, Predicate matches)
{
    if (!item)
        return nullptr;
    if (matches(item))
        return item;
    const QList<QQuickItem *> children = item->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = findItem(child, matches))
            return found;
    }
    return nullptr;
}

// Match either spelling of a Quick type, because which one an item has depends
// on how the active style defines it: an unstyled type is C++ ("QQuickLoader"),
// while a styled Controls type is QML-defined and arrives as
// "StackView_QMLTYPE_55_QML_63" -> "StackView". A probe that knew only one
// spelling would silently find nothing under the other style.
QQuickItem *findByType(QQuickItem *root, const QString &name)
{
    const QString cppName = QStringLiteral("QQuick") + name;
    return findItem(root, [&name, &cppName](QQuickItem *item) {
        const QString className =
            QString::fromLatin1(item->metaObject()->className());
        // "Loader" has to match QQuickLoader, and "StackView" has to match
        // StackView_QMLTYPE_55_QML_63, so both spellings are tried for one name.
        return className == cppName || className == name
               || qmlTypeName(item) == name;
    });
}

// The page the StackView is showing. Found by walking to the StackView and
// reading its currentItem property, which IS public API — unlike its id.
QQuickItem *currentPage(QQuickWindow *window)
{
    QQuickItem *stack = findByType(window->contentItem(), QStringLiteral("StackView"));
    if (!stack)
        return nullptr;
    return stack->property("currentItem").value<QQuickItem *>();
}

int stackDepth(QQuickWindow *window)
{
    QQuickItem *stack = findByType(window->contentItem(), QStringLiteral("StackView"));
    return stack ? stack->property("depth").toInt() : -1;
}

// How many rows a page's list is actually showing.
int listCount(QQuickItem *page)
{
    QQuickItem *view = findByType(page, QStringLiteral("ListView"));
    return view ? view->property("count").toInt() : -1;
}

// Captures what the pane WRITES to its PTY, and nothing else.
//
// tst_mobileterminal and tst_terminalcontroller each carry a full FakeChannel
// with a read side; this test needs no read side at all, because remote output
// is injected straight through TerminalController::ingestOutput(). So this is
// deliberately not a third copy of that class - it is the smaller thing the
// question here needs. NOT a QBuffer, for the same reason those two say: a
// QBuffer makes written bytes readable, so every report this test sends would
// come back as terminal output.
class CapturingPty : public QIODevice {
public:
    CapturingPty() { open(QIODevice::ReadWrite | QIODevice::Unbuffered); }

    bool isSequential() const override { return true; }
    const QByteArray &written() const { return m_written; }
    void clearWritten() { m_written.clear(); }

protected:
    qint64 readData(char *, qint64) override { return 0; }
    qint64 writeData(const char *data, qint64 maxSize) override
    {
        m_written.append(data, maxSize);
        return maxSize;
    }

private:
    QByteArray m_written;
};

// The pane host's single Loader, and what it has loaded.
QQuickItem *findByQmlTypeIn(QQuickItem *root, const QString &typeName)
{
    return findItem(root, [&typeName](QQuickItem *item) {
        return qmlTypeName(item) == typeName;
    });
}

QQuickItem *paneLoaderOf(QQuickItem *host)
{
    return findByType(host, QStringLiteral("Loader"));
}

// The first item anywhere under `root` whose `text` property reads exactly
// `text`. Structural, like everything else here: it is how a case checks what
// the header is SHOWING rather than which property it happens to be bound to.
QQuickItem *findItemShowing(QQuickItem *root, const QString &text)
{
    return findItem(root, [&text](QQuickItem *candidate) {
        const QVariant value = candidate->property("text");
        return value.isValid() && value.toString() == text;
    });
}

// Every key sequence a QML Shortcut is really bound to, expanded exactly as
// QQuickShortcut expands it: a string is parsed as a sequence, and an integer is
// a QKeySequence::StandardKey standing for EVERY platform binding of that
// standard key. The expansion is the point — StandardKey.Back silently includes
// Backspace on Windows, and a test that only compared the written spelling would
// never see it.
QList<QKeySequence> boundSequences(const QObject *shortcut)
{
    QList<QKeySequence> bound;
    const QVariantList values = shortcut->property("sequences").toList();
    for (const QVariant &value : values) {
        if (value.typeId() == QMetaType::Int) {
            bound += QKeySequence::keyBindings(
                static_cast<QKeySequence::StandardKey>(value.toInt()));
        } else {
            bound += QKeySequence::fromString(value.toString());
        }
    }
    return bound;
}

// Every live item of a QML type under `item`: the single-live-pane invariant is
// measured in objects that exist, not in a property that claims one.
int countItemsOfType(QQuickItem *item, const QString &typeName)
{
    if (!item)
        return 0;
    int found = qmlTypeName(item) == typeName ? 1 : 0;
    const QList<QQuickItem *> children = item->childItems();
    for (QQuickItem *child : children)
        found += countItemsOfType(child, typeName);
    return found;
}

// ---- driving the fake codeharbord for a single viewer page ------------------
//
// The page cases below answer RPCs in the middle of a gesture, which is the only
// way to test what a page does with a reply that lands late, or never. These are
// the four reply shapes those pages consume; each mirrors the one the daemon
// really sends (see remote/src/files.ts and src/mobile/tests/tst_mobileviewers.cpp,
// which pins the same shapes against the service itself).

// The subset of `requests` with one method. A single gesture issues more than one
// call — a listing and the SPEC 9 resolve go out together — so a case picks the
// one it means instead of assuming an order.
QVector<QJsonObject> requestsFor(const QVector<QJsonObject> &requests,
                                 const char *method)
{
    QVector<QJsonObject> matching;
    for (const QJsonObject &request : requests) {
        if (request.value(QStringLiteral("method")).toString()
            == QLatin1String(method)) {
            matching.push_back(request);
        }
    }
    return matching;
}

int requestId(const QJsonObject &request)
{
    return request.value(QStringLiteral("id")).toInt();
}

QString requestPath(const QJsonObject &request)
{
    return request.value(QStringLiteral("params"))
        .toObject()
        .value(QStringLiteral("path"))
        .toString();
}

QByteArray listingFrame(int id, const QStringList &directories,
                        const QStringList &files)
{
    QJsonArray entries;
    for (const QString &name : directories)
        entries.append(QJsonObject{{"name", name}, {"kind", "directory"}});
    for (const QString &name : files)
        entries.append(QJsonObject{{"name", name}, {"kind", "file"}});
    return resultFrame(id, QJsonObject{{"entries", entries}});
}

QByteArray textReadFrame(int id, const QString &text, const QString &revision)
{
    return resultFrame(id, QJsonObject{{"encoding", "utf-8"},
                                       {"content", text},
                                       {"revision", revision},
                                       {"truncated", false}});
}

// A base64 reply, which is what the daemon sends for bytes its strict UTF-8
// decoder refused — an image or a PDF, in these cases.
QByteArray byteReadFrame(int id, const QByteArray &bytes)
{
    return resultFrame(id,
                       QJsonObject{{"encoding", "base64"},
                                   {"content",
                                    QString::fromLatin1(bytes.toBase64())},
                                   {"revision", "r1"},
                                   {"truncated", false}});
}

QByteArray resolveFrame(int id, const QString &path, bool insideRepositoryRoot)
{
    return resultFrame(id, QJsonObject{{"path", path},
                                       {"insideRepositoryRoot",
                                        insideRepositoryRoot}});
}

// A real PNG, so the image handshake is exercised against bytes a decoder
// accepts rather than a placeholder.
QByteArray pngBytes()
{
    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return buffer.data();
}

// The first descendant QObject (not only Items — an ch::EditorController is
// parented to the page but is not one) whose C++ class is `className`.
QObject *findObjectOfClass(QObject *root, const QString &className)
{
    if (!root)
        return nullptr;
    const QList<QObject *> children = root->findChildren<QObject *>();
    for (QObject *child : children) {
        if (QString::fromLatin1(child->metaObject()->className()) == className)
            return child;
    }
    return nullptr;
}

// Call a function declared on a QML object. QML function parameters arrive as
// QVariant whatever their declared type, which is why every argument here is
// wrapped in one.
bool callQmlFunction(QObject *object, const char *name,
                     const QVariant &argument = QVariant())
{
    if (!object)
        return false;
    if (!argument.isValid())
        return QMetaObject::invokeMethod(object, name);
    return QMetaObject::invokeMethod(object, name, Q_ARG(QVariant, argument));
}

// Answer an editor page's first read, leaving a clean buffer holding `content`,
// and hand back the REAL ch::EditorController the page minted. The editor cases
// drive that object rather than a stand-in: the page's whole job is to react
// correctly to its contract, and a double could be made to agree with a wrong
// page.
QObject *openEditor(QQuickItem *page, const QString &content,
                    const QString &revision,
                    const QVector<QJsonObject> &requests,
                    FakeTransport &transport)
{
    const QVector<QJsonObject> reads =
        requestsFor(requests, ch::rpc::kMethodReadFile);
    if (reads.size() != 1)
        return nullptr;
    transport.deliver(textReadFrame(requestId(reads.at(0)), content, revision));
    return findObjectOfClass(page, QStringLiteral("ch::EditorController"));
}

}  // namespace

class TstMobileShell : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();

    void theFirstScreenIsTheConnectPage();
    void aBlankNodePathBlocksConnecting();
    void aDragOverTheTerminalSendsWheelReports();
    void theKeySheetFitsTheScreenAndScrolls();
    void theSessionPickerListsEverySessionTheServerReported();
    void thePanePickerListsBothRegionsOfTheSelectedSession();
    void aTerminalLeafLoadsTheTerminalPage();
    void aMarkdownLeafLoadsTheMarkdownPage();
    void anUnknownKindLandsOnTheUnsupportedPage();
    void onlyOnePaneIsEverLoaded();
    void backFromThePaneUnloadsIt();
    void theUnsupportedPageIsToldWhichKindItCouldNotShow();
    void thePaneHeaderFollowsTheLoadedPageAndResetsWithThePane();
    void disconnectingFromAPaneCollapsesTheStackInOneStep();
    void theBackGestureIsNotBoundToAnyTypingKey();
    void theErrorStripComesBackForTheNextMessage();

    // ---- one viewer page at a time (requested by the viewer-page slice) ----
    void tappingASecondFileRetargetsTheLoadedViewer();
    void theSessionRootListsEvenWithoutATrailingSlash();
    void tappingADirectoryListsItExactlyOnceAndRenamesTheHeader();
    void anEmptyDirectorySaysSoOnlyOnceItHasAnswered();
    void leavingADirtyBufferAsksFirstAndKeepsNamingTheOpenFile();
    void aLoadLandingOnADirtyBufferKeepsTheEditsAndFlagsAConflict();
    void anEditMadeWhileASaveIsInFlightStaysUnsaved();
    void theFirstKeystrokeIsReportedWithoutWaitingForTheDebounce();
    void aCrlfFileIsSplitWithoutCarriageReturns();
    void anOrderedListItemIsNotDrawnAsABullet();
    void retargetingTheImagePaneHandsBackTheBytes();
    // Declared unconditionally and SKIPPED inside when the optional Qt module is
    // absent, rather than compiled out: a slot behind a preprocessor conditional
    // has to be seen the same way by moc and by the compiler, and a build where
    // those two disagree fails at link time with nothing pointing at the cause.
    void thePdfSpoolFileIsDeletedWhenThePaneMovesOnAndWhenItDies();
    void aNavigationToANonHttpAddressIsRefusedAndSaidSo();

private:
    // One engine per case: the shell keeps per-session state, and a case that
    // inherited another's stack would be asserting on somebody else's walk.
    struct Shell {
        Fixture fixture;
        MobileViewerService viewers{&fixture.client, nullptr};
        QQmlApplicationEngine engine;

        Shell()
        {
            // No type registration here on purpose: ch_mobile's own QML module
            // (CodeHarbor.Mobile.Core) registers every C++ type QML names
            // statically, so this host loads exactly the module a device loads.
            // That is the point — a gate that hand-registered types would be
            // proving a configuration no real host uses.

            engine.rootContext()->setContextProperty(QStringLiteral("mobile"),
                                                     &fixture.mobile);
            engine.rootContext()->setContextProperty(QStringLiteral("app"),
                                                     &fixture.controller);
            engine.rootContext()->setContextProperty(QStringLiteral("layouts"),
                                                     &fixture.layouts);
            engine.rootContext()->setContextProperty(
                QStringLiteral("viewerService"), &viewers);
            // terminalFactory, keyStore and editorFactory are deliberately NOT
            // installed. Every page guards its context properties the way the
            // desktop module does, so their absence exercises the degraded path
            // — chrome without a live backend — which is what a page must do
            // rather than throwing a ReferenceError that aborts the binding pass
            // building the whole pane.
            engine.loadFromModule(QStringLiteral("CodeHarbor.Mobile"),
                                  QStringLiteral("MobileMain"));
        }

        QQuickWindow *window() const
        {
            if (engine.rootObjects().isEmpty())
                return nullptr;
            return qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        }
    };

    // ONE viewer page, alone in a phone-shaped window, over the same fake
    // codeharbord the shell walk uses.
    //
    // In production these pages are built by PaneHostPage's Loader, which hands
    // them exactly the properties `properties` spells out here; instantiating one
    // directly is that, minus the shell. It is what lets a case answer an RPC in
    // the middle of a gesture, and what lets the editor cases drive a REAL
    // ch::EditorController rather than a stand-in that could happily agree with a
    // wrong page.
    //
    // A real window, shown offscreen, because a ListView with no size creates no
    // delegates at all — and several of these cases assert on what a row shows.
    struct PageHost {
        Fixture fixture;
        MobileViewerService viewers{&fixture.client, nullptr};
        EditorFactory editors{&fixture.client};
        QQmlApplicationEngine engine;
        QScopedPointer<QObject> root;

        PageHost(const QString &type, const QString &properties)
        {
            engine.rootContext()->setContextProperty(QStringLiteral("mobile"),
                                                     &fixture.mobile);
            engine.rootContext()->setContextProperty(QStringLiteral("app"),
                                                     &fixture.controller);
            engine.rootContext()->setContextProperty(QStringLiteral("layouts"),
                                                     &fixture.layouts);
            engine.rootContext()->setContextProperty(
                QStringLiteral("viewerService"), &viewers);
            engine.rootContext()->setContextProperty(
                QStringLiteral("editorFactory"), &editors);

            const QString source =
                QStringLiteral("import QtQuick\n"
                               "import QtQuick.Window\n"
                               "import CodeHarbor.Mobile\n"
                               "Window {\n"
                               "    width: 412\n"
                               "    height: 892\n"
                               "    visible: true\n"
                               "    %1 {\n"
                               "        anchors.fill: parent\n"
                               "        %2\n"
                               "    }\n"
                               "}\n")
                    .arg(type, properties);
            QQmlComponent component(&engine);
            component.setData(source.toUtf8(),
                              QUrl(QStringLiteral("qrc:/tst_mobileshell/page.qml")));
            if (component.isError())
                qWarning("%s", qPrintable(component.errorString()));
            root.reset(component.create());
        }

        QQuickWindow *window() const
        {
            return qobject_cast<QQuickWindow *>(root.data());
        }

        // The page under test: the window's one declared child.
        QQuickItem *page() const
        {
            QQuickWindow *w = window();
            if (!w || w->contentItem()->childItems().isEmpty())
                return nullptr;
            return w->contentItem()->childItems().first();
        }

        // Everything the page has asked the server since the last call.
        QVector<QJsonObject> requests() { return takeRequests(fixture.transport); }

        void deliver(const QByteArray &frame) { fixture.transport.deliver(frame); }
    };

    // Bring a page up and wait for its window, so its views have a size.
    bool showPage(PageHost &host);

    // Save a frame of the shell as it stands. Visual evidence for the assertion
    // beside it, and the only way a reviewer can see that "the pane picker lists
    // two rows" means a legible list rather than two overlapping zero-height
    // delegates.
    void grabFrame(Shell &shell, const QString &name);

    QString m_frameDir;
};

void TstMobileShell::initTestCase()
{
    // The shell writes through the REAL UiStateStore (per-user QSettings), as
    // main.cpp leaves it. Redirect at the process level rather than into the
    // developer's own config, exactly as src/app/tests does.
    QStandardPaths::setTestModeEnabled(true);

    m_frameDir = qEnvironmentVariable("CH_SHELL_FRAME_DIR");
    if (m_frameDir.isEmpty())
        m_frameDir = QDir::current().filePath(QStringLiteral("shell-frames"));
    QDir().mkpath(m_frameDir);
    qInfo("shell frames: %s", qPrintable(m_frameDir));
}

bool TstMobileShell::showPage(PageHost &host)
{
    QQuickWindow *window = host.window();
    if (!window)
        return false;
    window->show();
    return QTest::qWaitForWindowExposed(window) && host.page() != nullptr;
}

void TstMobileShell::grabFrame(Shell &shell, const QString &name)
{
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    // A phone-shaped surface, so the frames show what the layout does at the
    // aspect ratio it was designed for rather than at a desktop default.
    window->resize(412, 892);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    // Wait out the StackView push transition before grabbing. currentItem
    // changes the instant the stage does, but the OUTGOING page is still on
    // screen until the transition finishes — so an immediate grab produces a
    // frame of the previous page beside an assertion about the current one,
    // which is worse than no evidence at all.
    QQuickItem *stack = findByType(window->contentItem(), QStringLiteral("StackView"));
    QVERIFY(stack);
    QTRY_VERIFY(!stack->property("busy").toBool());
    // One more turn of the loop so the settled scene graph is what gets grabbed.
    QTest::qWait(50);

    const QImage frame = window->grabWindow();
    QVERIFY(!frame.isNull());
    // A blank grab is the failure this catches: a shell that loaded, bound and
    // reported no errors while drawing nothing at all.
    QVERIFY(frame.width() > 0 && frame.height() > 0);
    const QString path = QDir(m_frameDir).filePath(name + QStringLiteral(".png"));
    QVERIFY2(frame.save(path), qPrintable(path));
}

void TstMobileShell::theFirstScreenIsTheConnectPage()
{
    // Nothing about this client works before a server is reachable, so the
    // Servers stage must land on the page that gets one — and it must load
    // without the connect slice's optional context properties present.
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY2(window, "MobileMain.qml did not produce a window");

    QCOMPARE(stackDepth(window), 1);
    QCOMPARE(qmlTypeName(currentPage(window)), QStringLiteral("ConnectPage"));
    grabFrame(shell, QStringLiteral("01-connect"));
}

// A blank remote Node path used to sail through this form and fail on the
// server, one SSH handshake later, with a prerequisite report about the remote
// instead of about the field the user left empty. README documents the value as
// an absolute path and states it is deliberately NOT looked up on `PATH`, so
// blank is not a sanctioned shortcut and the form is where it must be caught.
void TstMobileShell::aBlankNodePathBlocksConnecting()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY2(window, "MobileMain.qml did not produce a window");

    QQuickItem *page = currentPage(window);
    QCOMPARE(qmlTypeName(page), QStringLiteral("ConnectPage"));

    // A host and user alone are NOT enough any more.
    page->setProperty("hostText", QStringLiteral("box.local"));
    page->setProperty("userText", QStringLiteral("someone"));
    page->setProperty("nodePathText", QString());
    QVERIFY2(!page->property("formValid").toBool(),
             "a blank node path still counted as a valid form");

    // And the reason names the field AND the command that answers it, so
    // "required" does not just relocate the guesswork.
    const QString blankMessage = page->property("validationMessage").toString();
    QVERIFY2(blankMessage.contains(QStringLiteral("Node path")),
             qPrintable(blankMessage));
    QVERIFY2(blankMessage.contains(QStringLiteral("command -v node")),
             qPrintable(blankMessage));
    // The host the user already typed is spliced in, not a placeholder.
    QVERIFY2(blankMessage.contains(QStringLiteral("ssh box.local")),
             qPrintable(blankMessage));

    // Whitespace is not a value.
    page->setProperty("nodePathText", QStringLiteral("   "));
    QVERIFY2(!page->property("formValid").toBool(),
             "whitespace counted as a node path");

    // A real path completes the form.
    page->setProperty("nodePathText", QStringLiteral("/usr/bin/node"));
    QVERIFY2(page->property("formValid").toBool(),
             "a complete form was still rejected");
    QCOMPARE(page->property("validationMessage").toString(), QString());
}

void TstMobileShell::theSessionPickerListsEverySessionTheServerReported()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);

    shell.fixture.listSessions({QStringLiteral("s1"), QStringLiteral("s2")});
    // Step one of the walk. The stage is what drives the stack, so this is the
    // shell reacting to the controller rather than the test pushing a page.
    shell.fixture.mobile.selectSession(QStringLiteral("s1"));
    QCOMPARE(qmlTypeName(currentPage(window)),
             QStringLiteral("SessionPickerPage"));

    // The sidebar model is a TWO-LEVEL QAbstractItemModel: groups at the root,
    // Dev Sessions as their children. The page's outer ListView therefore counts
    // GROUPS, and the sessions come from a nested DelegateModel rooted at the
    // group's index — the same construction src/qml/SessionsSidebar.qml uses,
    // because a flattening proxy would be a second, mobile-only view of the
    // workspace tree that could disagree with the desktop about ordering.
    //
    // So: one group row, and the model really does hold the two sessions under
    // it. That the nested level RENDERS is what the frame grab below shows.
    QQuickItem *page = currentPage(window);
    QCOMPARE(listCount(page), 1);
    const QAbstractItemModel *sessions = shell.fixture.controller.sessionsModel();
    QCOMPARE(sessions->rowCount(), 1);
    QCOMPARE(sessions->rowCount(sessions->index(0, 0)), 2);
    grabFrame(shell, QStringLiteral("02-sessions"));
}

void TstMobileShell::thePanePickerListsBothRegionsOfTheSelectedSession()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);

    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));

    // Step two: both server-authoritative region trees, flattened into one list.
    QCOMPARE(qmlTypeName(currentPage(window)), QStringLiteral("PanePickerPage"));
    QQuickItem *page = currentPage(window);
    QCOMPARE(listCount(page), 2);
    grabFrame(shell, QStringLiteral("03-panes"));
}

void TstMobileShell::aTerminalLeafLoadsTheTerminalPage()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));

    shell.fixture.mobile.selectPane(QStringLiteral("terminal:terminal-1"));
    QQuickItem *page = currentPage(window);
    QCOMPARE(qmlTypeName(page), QStringLiteral("PaneHostPage"));

    // The routing table resolved to a component that actually loaded. The
    // Loader's STATUS is the assertion, not the source string alone: a
    // misspelled page name leaves the source set and the status in Error, so a
    // source-only check would pass on a pane that shows the user nothing.
    QQuickItem *loader = paneLoaderOf(page);
    QVERIFY(loader);
    QCOMPARE(loader->property("status").toInt(), 1);  // Loader.Ready
    QVERIFY(loader->property("source").toUrl().toString()
                .endsWith(QStringLiteral("TerminalPage.qml")));
    QCOMPARE(countItemsOfType(page, QStringLiteral("TerminalPage")), 1);

    // A page that LOADED is not a page that WORKS. The terminal only exists once
    // MobileAppController::createTerminalSession() has handed it a live
    // ch::MobileTerminalSession, and that call returns a C++ type: if the type is
    // not registered with QML the call fails at runtime ("Unknown method return
    // type"), the Loader still reports Ready, and the user gets a terminal pane
    // that can never attach to anything. So the session itself is the assertion.
    QQuickItem *terminal = findByQmlTypeIn(page, QStringLiteral("TerminalPage"));
    QVERIFY(terminal);
    QObject *session = terminal->property("session").value<QObject *>();
    QVERIFY2(session, "TerminalPage loaded without a MobileTerminalSession");
    QCOMPARE(QString::fromLatin1(session->metaObject()->className()),
             QStringLiteral("ch::MobileTerminalSession"));
    // And it is wired to a screen, which is what the renderer draws from.
    QVERIFY(session->property("screen").value<QObject *>());
    grabFrame(shell, QStringLiteral("04-terminal-pane"));
}

// The QML half of the tmux scroll fix, driven through the real MouseArea.
//
// tst_mobileterminal asserts the BYTES for a given notch count by calling
// sendMouseWheel() directly. That leaves the part the user actually touches
// unproven: the travel-to-notch arithmetic, the swipe direction, and the
// pixel-to-cell conversion could each be wrong and that test would still pass.
// So this one synthesises a drag on the page and reads what reached the PTY.
void TstMobileShell::aDragOverTheTerminalSendsWheelReports()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));
    shell.fixture.mobile.selectPane(QStringLiteral("terminal:terminal-1"));

    // Let the StackView push FINISH before touching anything. Mid-transition the
    // page is still sliding horizontally, so a synthesised press lands at a
    // different cell than the arithmetic here predicts, and the item moving
    // under a stationary finger generates extra positionChanged deliveries -
    // which is an extra notch. Both were observed while writing this.
    QQuickItem *stack = findByType(window->contentItem(), QStringLiteral("StackView"));
    QVERIFY(stack);
    QTRY_VERIFY(!stack->property("busy").toBool());

    QQuickItem *page = currentPage(window);
    QQuickItem *terminal = findByQmlTypeIn(page, QStringLiteral("TerminalPage"));
    QVERIFY(terminal);
    auto *session = qobject_cast<MobileTerminalSession *>(
        terminal->property("session").value<QObject *>());
    QVERIFY(session);

    CapturingPty pty;
    session->controller()->setTransport(&pty);

    // Put the pane where a real attached tmux puts it: alternate screen, mouse
    // reporting on, SGR encoding.
    session->controller()->ingestOutput(QByteArrayLiteral(
        "\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1006h"));
    QTRY_VERIFY(session->screen()->altScreenActive());
    QCOMPARE(session->screen()->mouseEncoding(), VtMouseEncoding::Sgr);

    // By C++ type, not by QML type name: qmlTypeName() reports the class name,
    // which for this one is namespace-qualified ("ch::MobileTerminalView").
    auto *view = qobject_cast<MobileTerminalView *>(
        findItem(page, [](QQuickItem *item) {
            return qobject_cast<MobileTerminalView *>(item) != nullptr;
        }));
    QVERIFY(view);
    const qreal lineHeight = view->lineHeight();
    const qreal cellWidth = view->cellWidth();
    QVERIFY2(lineHeight > 0.0 && cellWidth > 0.0,
             "the view reported no cell metric, so the drag maths cannot run");

    // Three lines per notch, as TerminalPage's gesture defines.
    const qreal step = lineHeight * 3.0;
    const QPointF origin = view->mapToScene(QPointF(0.0, 0.0));

    // Local coordinates inside the view, then the same point in the window.
    const QPointF startLocal(cellWidth * 6.0 + 1.0, lineHeight * 4.0 + 1.0);
    // Two notches DOWN the screen, plus a sliver that must stay a remainder.
    const QPointF endLocal(startLocal.x(), startLocal.y() + step * 2.0 + 2.0);
    const QPoint start = (origin + startLocal).toPoint();
    const QPoint end = (origin + endLocal).toPoint();

    pty.clearWritten();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(window, end);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);

    // The cell is the one UNDER THE FINGER at the moment the notch fires, which
    // is what decides which tmux pane scrolls in a split window.
    //
    // Derived from the point the event actually carried, mapped back into the
    // item: a synthesised press takes INTEGER window coordinates, and the item's
    // scene origin is fractional, so predicting from the ideal float lands on
    // the neighbouring row whenever the rounding goes the other way.
    const auto cellOf = [&](const QPoint &windowPoint) {
        const QPointF local = view->mapFromScene(QPointF(windowPoint));
        return QPoint(int(local.x() / cellWidth) + 1,
                      int(local.y() / lineHeight) + 1);
    };
    const QPoint cell = cellOf(end);
    const QByteArray up = "\x1b[<64;" + QByteArray::number(cell.x()) + ';'
                          + QByteArray::number(cell.y()) + 'M';
    // Dragging DOWN reveals OLDER output, so this is wheel UP (64) - and the
    // sliver past two full notches did NOT become a third.
    QCOMPARE(pty.written(), up + up);

    // The local offset is untouched: on the alternate screen it has nowhere to
    // go, and moving it was the original bug.
    QCOMPARE(view->scrollOffset(), 0);

    // The other direction is the other button.
    const QPointF backLocal(startLocal.x(), startLocal.y() - step - 1.0);
    const QPoint back = (origin + backLocal).toPoint();
    pty.clearWritten();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(window, back);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, back);
    const QPoint backCell = cellOf(back);
    const int backColumn = backCell.x();
    const int backRow = backCell.y();
    QCOMPARE(pty.written(),
             QByteArray("\x1b[<65;" + QByteArray::number(backColumn) + ';'
                        + QByteArray::number(backRow) + 'M'));

    // A TAP is not a scroll: it raises the keyboard and writes nothing.
    pty.clearWritten();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, start);
    QCOMPARE(pty.written(), QByteArray());

    // And when the program stops asking for mouse events, the same drag falls
    // back to the local offset instead of writing to the PTY. Leave the alt
    // screen too, so there is real scrollback to move through.
    session->controller()->ingestOutput(QByteArrayLiteral(
        "\x1b[?1000l\x1b[?1002l\x1b[?1006l\x1b[?1049l"));
    QTRY_COMPARE(session->screen()->mouseEncoding(), VtMouseEncoding::None);
    for (int line = 0; line < 200; ++line)
        session->controller()->ingestOutput(QByteArrayLiteral("filler\r\n"));
    QTRY_VERIFY(view->maxScrollOffset() > 0);

    pty.clearWritten();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(window, end);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
    QCOMPARE(pty.written(), QByteArray());
    QVERIFY2(view->scrollOffset() > 0,
             "with no mouse reporting the drag must move the local offset");
}

// The key sheet is the longest surface in the client - explanation, key list,
// file pick, name field, paste box, preview, fingerprint - so it is the first
// to outgrow a phone. It used to size itself to that content, and because the
// Close button lives in the Dialog's pinned FOOTER, the dialog growing past the
// window took the only way out with it: on Android the button landed under the
// navigation bar, unreachable.
void TstMobileShell::theKeySheetFitsTheScreenAndScrolls()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);

    // A short window, which is the whole point: 480 is shorter than this sheet's
    // natural height, so the cap and the Flickable both have to do their job.
    // Phone-width but deliberately SHORT, and shown, exactly as grabFrame()
    // does: an unexposed window lays nothing out, so its content item has zero
    // height and the sheet would fall back to its own minimum instead of being
    // constrained by the screen this test is meant to simulate.
    window->resize(412, 480);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QTRY_COMPARE(window->contentItem()->height(), 480.0);

    // The sheet the APP hosts, opened the way the app opens it. Instantiating
    // KeyImportSheet standalone would test a sheet nobody uses: the insets are
    // handed to it by ConnectPage, so a bare instance sees zero and the whole
    // point of this test evaporates.
    QQuickItem *connectPage = currentPage(window);
    QCOMPARE(qmlTypeName(connectPage), QStringLiteral("ConnectPage"));
    QObject *sheet = nullptr;
    for (QObject *child : connectPage->findChildren<QObject *>()) {
        if (child->inherits("QQuickPopup")
            && QString::fromLatin1(child->metaObject()->className())
                   .contains(QStringLiteral("KeyImportSheet"))) {
            sheet = child;
            break;
        }
    }
    QVERIFY2(sheet, "ConnectPage does not host a KeyImportSheet");
    QVERIFY(QMetaObject::invokeMethod(sheet, "open"));
    QTRY_VERIFY(sheet->property("visible").toBool());

    // A REAL non-zero bottom inset, injected on the WINDOW.
    //
    // SafeArea attaches to a Window as well as to an Item, and
    // additionalMargins is writable for exactly this purpose, so the shell can
    // be told it has a 96px navigation bar and every consumer must follow.
    // Measured along the way, and worth keeping: a Popup and a Popup's
    // contentItem report ZERO whatever the window says, which is why the sheet
    // is handed its insets by ConnectPage rather than asking SafeArea itself -
    // wiring `SafeArea.margins` onto the Dialog would have been dead code on a
    // device as much as here.
    {
        QQmlComponent inj(&shell.engine);
        inj.setData("import QtQuick\n"
                    "QtObject {\n"
                    "    property var target: null\n"
                    "    property real applied: -1\n"
                    "    function apply(px) {\n"
                    "        target.SafeArea.additionalMargins.bottom = px\n"
                    "        applied = target.SafeArea.additionalMargins.bottom\n"
                    "    }\n"
                    "}\n",
                    QUrl(QStringLiteral("qrc:/tst_mobileshell/inject.qml")));
        QVERIFY2(!inj.isError(), qPrintable(inj.errorString()));
        std::unique_ptr<QObject> injector(inj.create());
        QVERIFY(injector);
        injector->setProperty("target", QVariant::fromValue(window));
        QVERIFY(QMetaObject::invokeMethod(injector.get(), "apply",
                                          QVariant(96.0)));
        QCOMPARE(injector->property("applied").toReal(), 96.0);
    }

    // The window published it and ConnectPage handed it down. Both halves of the
    // chain are load bearing, so both are asserted.
    QTRY_COMPARE(window->property("safeAreaBottom").toReal(), 96.0);
    QTRY_COMPARE(sheet->property("safeBottomInset").toReal(), 96.0);

    const qreal safeTop = sheet->property("safeTop").toReal();
    const qreal safeBottom = sheet->property("safeBottom").toReal();
    QCOMPARE(safeBottom, 96.0);
    QCOMPARE(safeTop, window->property("safeAreaTop").toReal());
    QCOMPARE(sheet->property("topMargin").toReal(), safeTop);
    QCOMPARE(sheet->property("bottomMargin").toReal(), 96.0);

    // Everything below runs WITH the 96px inset in place - restoring it first
    // would leave these assertions describing a case they never exercised.
    //
    // It fits the window minus the navigation bar, minus its own breathing room.
    const qreal available = window->contentItem()->height();
    const qreal sheetHeight = sheet->property("height").toReal();
    QVERIFY2(sheetHeight <= available - safeTop - safeBottom - 32.0 + 0.5,
             qPrintable(QStringLiteral("sheet is %1 tall in a %2 window with "
                                       "insets %3/%4")
                            .arg(sheetHeight)
                            .arg(available)
                            .arg(safeTop)
                            .arg(safeBottom)));

    // The button must clear the NAVIGATION BAR, not merely the window edge, and
    // with a 96px inset injected above that is a real distinction rather than a
    // restatement. This is what the report was about: a Close button inside the
    // window but underneath the bar cannot be tapped.
    auto *footer = sheet->property("footer").value<QQuickItem *>();
    QVERIFY2(footer, "the sheet has no footer");
    QQuickItem *close = findItem(footer, [](QQuickItem *item) {
        return item->objectName() == QStringLiteral("importCloseButton");
    });
    QVERIFY2(close, "the sheet has no Close button");
    const QPointF closeBottom =
        close->mapToScene(QPointF(0.0, close->height()));
    QVERIFY2(closeBottom.y() <= available - safeBottom + 0.5,
             qPrintable(QStringLiteral("Close button bottom is at %1, but the "
                                       "reachable area ends at %2")
                            .arg(closeBottom.y())
                            .arg(available - safeBottom)));

    // And the body scrolls, so capping the height hid nothing. The content is
    // genuinely taller than the viewport, and flicking moves it.
    auto *body = sheet->property("contentItem").value<QQuickItem *>();
    QVERIFY2(body, "the sheet has no contentItem");
    QQuickItem *flick = qobject_cast<QQuickItem *>(body->inherits("QQuickFlickable")
                                                       ? body
                                                       : findByType(body, QStringLiteral("Flickable")));
    QVERIFY(flick);
    const qreal contentHeight = flick->property("contentHeight").toReal();
    QVERIFY2(contentHeight > flick->height(),
             qPrintable(QStringLiteral("content %1 fits viewport %2, so this "
                                       "test proves nothing")
                            .arg(contentHeight)
                            .arg(flick->height())));
    QCOMPARE(flick->property("contentY").toReal(), 0.0);
    flick->setProperty("contentY", 40.0);
    QCOMPARE(flick->property("contentY").toReal(), 40.0);

    // The fingerprint at the very bottom of the body is reachable by scrolling
    // to the end - it is the last thing the sheet shows about a pasted key.
    flick->setProperty("contentY", contentHeight - flick->height());
    QVERIFY(flick->property("contentY").toReal() > 0.0);
}

void TstMobileShell::aMarkdownLeafLoadsTheMarkdownPage()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    shell.fixture.listSessions({QStringLiteral("s1")});
    // The fixture's viewer leaf is notes.md, which ch::ViewerHandlerRegistry
    // classifies as markdown — so this also pins the classification the pane
    // list hands the host page.
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));

    shell.fixture.mobile.selectPane(QStringLiteral("viewer:viewer-1"));
    QQuickItem *page = currentPage(window);
    QCOMPARE(qmlTypeName(page), QStringLiteral("PaneHostPage"));
    QQuickItem *loader = paneLoaderOf(page);
    QVERIFY(loader);
    QCOMPARE(loader->property("status").toInt(), 1);  // Loader.Ready
    QVERIFY(loader->property("source").toUrl().toString()
                .endsWith(QStringLiteral("ViewerMarkdownPage.qml")));
    QCOMPARE(countItemsOfType(page, QStringLiteral("ViewerMarkdownPage")), 1);
    grabFrame(shell, QStringLiteral("05-markdown-pane"));
}

void TstMobileShell::anUnknownKindLandsOnTheUnsupportedPage()
{
    // componentFor() is total by contract. A kind nothing recognises must land on
    // a page that says so, never on an empty screen.
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.mobile.selectSession(QStringLiteral("s1"));
    shell.fixture.answerLayouts(
        shell.fixture.takeLayoutRequests(),
        viewerTreeFor(QStringLiteral("viewer-1"),
                      QStringLiteral("file:///srv/s1/archive.tar.zst")),
        terminalTreeFor(QStringLiteral("terminal-1"), QStringLiteral("row-1")));

    shell.fixture.mobile.selectPane(QStringLiteral("viewer:viewer-1"));
    QQuickItem *page = currentPage(window);
    QQuickItem *loader = paneLoaderOf(page);
    QVERIFY(loader);
    QCOMPARE(loader->property("source").toUrl().toString().section('/', -1),
             QStringLiteral("ViewerUnsupportedPage.qml"));
    QCOMPARE(countItemsOfType(page, QStringLiteral("ViewerUnsupportedPage")), 1);
    grabFrame(shell, QStringLiteral("06-unsupported-pane"));
}

void TstMobileShell::onlyOnePaneIsEverLoaded()
{
    // The single-pane contract, measured in live items rather than in intent:
    // switching panes must destroy the previous one, not stack a second beside
    // it. A terminal pane holds an SSH PTY channel, so two live at once is a
    // resource leak the user pays for.
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));

    shell.fixture.mobile.selectPane(QStringLiteral("terminal:terminal-1"));
    QQuickItem *host = currentPage(window);
    QVERIFY(host);
    QCOMPARE(countItemsOfType(host, QStringLiteral("TerminalPage")), 1);
    QCOMPARE(countItemsOfType(host, QStringLiteral("ViewerMarkdownPage")), 0);

    // Straight from one pane to another, without going back through the picker.
    shell.fixture.mobile.selectPane(QStringLiteral("viewer:viewer-1"));
    host = currentPage(window);
    QVERIFY(host);
    QCOMPARE(countItemsOfType(host, QStringLiteral("TerminalPage")), 0);
    QCOMPARE(countItemsOfType(host, QStringLiteral("ViewerMarkdownPage")), 1);
}

void TstMobileShell::backFromThePaneUnloadsIt()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));
    shell.fixture.mobile.selectPane(QStringLiteral("terminal:terminal-1"));

    shell.fixture.mobile.back();
    // Back to the picker, and the pane is GONE rather than parked out of sight
    // holding its channel open. Counted over the WHOLE window, not under the
    // picker: a popped page that was merely unparented from the stack is still a
    // live object holding its PTY channel, and counting from the page that
    // replaced it could never have seen one.
    QCOMPARE(qmlTypeName(currentPage(window)), QStringLiteral("PanePickerPage"));
    QCOMPARE(countItemsOfType(window->contentItem(),
                              QStringLiteral("TerminalPage")),
             0);
}

// The unsupported page EXPLAINS itself, and it can only do that if the host tells
// it what it was handed. It derives its whole message from a `kind` property that
// defaults to "binary", so a host that passed nothing told every user the same
// story — "this file is not UTF-8 text" — including for a PDF on a build with no
// PDF engine and for a resource whose scheme no handler claims.
void TstMobileShell::theUnsupportedPageIsToldWhichKindItCouldNotShow()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.mobile.selectSession(QStringLiteral("s1"));
    // A scheme ch::ViewerHandlerRegistry claims nothing for, which is what makes
    // ch::PaneListModel call the kind "unsupported" rather than "binary".
    shell.fixture.answerLayouts(
        shell.fixture.takeLayoutRequests(),
        viewerTreeFor(QStringLiteral("viewer-1"),
                      QStringLiteral("gopher://example.invalid/0/notes")),
        terminalTreeFor(QStringLiteral("terminal-1"), QStringLiteral("row-1")));

    shell.fixture.mobile.selectPane(QStringLiteral("viewer:viewer-1"));
    QQuickItem *page = currentPage(window);
    QQuickItem *unsupported =
        findByQmlTypeIn(page, QStringLiteral("ViewerUnsupportedPage"));
    QVERIFY2(unsupported, "an unclaimed scheme did not reach the unsupported page");
    QCOMPARE(unsupported->property("kind").toString(),
             QStringLiteral("unsupported"));
}

// The pane header shows the pane's own title until the loaded page reports a
// better one, and it goes back to the pane's title for the NEXT pane. The header
// used to be updated by assigning onto its `text`, which destroys the binding for
// good: the first page that ever reported a title pinned that title onto every
// pane opened afterwards.
void TstMobileShell::thePaneHeaderFollowsTheLoadedPageAndResetsWithThePane()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));

    shell.fixture.mobile.selectPane(QStringLiteral("terminal:terminal-1"));
    QQuickItem *host = currentPage(window);
    QVERIFY(host);
    // The layout's own label for the pane, before anything reports anything.
    QVERIFY(findItemShowing(host, QStringLiteral("terminal-1")));

    // What a page reporting a title does: TerminalPage emits titleRequested for
    // the remote window title, and the host records it here.
    host->setProperty("titleOverride", QStringLiteral("vim README.md"));
    QVERIFY2(findItemShowing(host, QStringLiteral("vim README.md")),
             "the header is not bound to the title the loaded page reported");

    // The next pane. Its own title is what must be shown — the previous pane's
    // reported title belonged to a page that no longer exists.
    shell.fixture.mobile.selectPane(QStringLiteral("viewer:viewer-1"));
    host = currentPage(window);
    QVERIFY(host);
    QCOMPARE(host->property("titleOverride").toString(), QString());
    QVERIFY(findItemShowing(host, QStringLiteral("notes.md")));
    QVERIFY(!findItemShowing(host, QStringLiteral("vim README.md")));
}

// A stage change that skips levels. The stack mirrors ch::MobileAppController's
// stage rather than being pushed by the pages, so a disconnect made from inside a
// pane has to unwind three pages at once — and take the live pane with it.
void TstMobileShell::disconnectingFromAPaneCollapsesTheStackInOneStep()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);
    shell.fixture.listSessions({QStringLiteral("s1")});
    shell.fixture.openSession(QStringLiteral("s1"), QStringLiteral("viewer-1"),
                              QStringLiteral("terminal-1"));
    shell.fixture.mobile.selectPane(QStringLiteral("terminal:terminal-1"));
    QCOMPARE(stackDepth(window), 4);

    shell.fixture.mobile.disconnect();
    QCOMPARE(stackDepth(window), 1);
    QCOMPARE(qmlTypeName(currentPage(window)), QStringLiteral("ConnectPage"));
    QCOMPARE(countItemsOfType(window->contentItem(),
                              QStringLiteral("TerminalPage")),
             0);
    QCOMPARE(countItemsOfType(window->contentItem(),
                              QStringLiteral("PaneHostPage")),
             0);
}

// The back gesture, and what it must NOT also be. A QML Shortcut is matched
// before the key reaches the focused item, so anything bound here is taken away
// from every page in the shell: Escape would never reach a terminal pane (which
// is how you leave insert mode in vi), and Backspace — which
// QKeySequence::Back includes on Windows — would navigate back instead of
// deleting a character on the desktop-hosted build.
void TstMobileShell::theBackGestureIsNotBoundToAnyTypingKey()
{
    Shell shell;
    QQuickWindow *window = shell.window();
    QVERIFY(window);

    QObject *shortcut = nullptr;
    const QList<QObject *> children = window->findChildren<QObject *>();
    for (QObject *child : children) {
        if (QString::fromLatin1(child->metaObject()->className())
            == QLatin1String("QQuickShortcut")) {
            shortcut = child;
            break;
        }
    }
    QVERIFY2(shortcut, "the shell declares no back shortcut at all");

    const QList<QKeySequence> bound = boundSequences(shortcut);
    QVERIFY(bound.contains(QKeySequence(QKeyCombination(Qt::Key_Back))));
    QVERIFY2(!bound.contains(QKeySequence(QKeyCombination(Qt::Key_Escape))),
             "Escape is bound to back navigation and can never reach a pane");
    QVERIFY2(!bound.contains(QKeySequence(QKeyCombination(Qt::Key_Backspace))),
             "Backspace is bound to back navigation and can never reach a field");
}

// The error strip is dismissed per MESSAGE, not per wording. Remembering the
// dismissed TEXT hid every later message that happened to read the same — two
// wrong passwords in a row, the same pane refused twice — which leaves the user
// tapping a button that appears to do nothing, with no explanation on screen.
void TstMobileShell::theErrorStripComesBackForTheNextMessage()
{
    QQmlApplicationEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\n"
                      "import CodeHarbor.Mobile\n"
                      "MobileErrorBar { width: 400 }\n",
                      QUrl(QStringLiteral("qrc:/tst_mobileshell/errorbar.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    const QScopedPointer<QObject> bar(component.create());
    QVERIFY(bar);

    const QString failure = QStringLiteral("Authentication failed.");
    bar->setProperty("message", failure);
    QVERIFY(bar->property("showing").toBool());

    // The user closes the strip.
    bar->setProperty("_dismissed", true);
    QVERIFY(!bar->property("showing").toBool());

    // The retry: a progress note, then the very same failure. Both are new
    // messages and both must be shown.
    bar->setProperty("message", QStringLiteral("Connecting"));
    QVERIFY(bar->property("showing").toBool());
    bar->setProperty("message", failure);
    QVERIFY2(bar->property("showing").toBool(),
             "a repeated failure stayed hidden behind an old dismissal");
}

// ---- one viewer page at a time ---------------------------------------------
//
// Every case below was requested by the slice that owns the viewer pages, and
// each one guards a defect that slice had just fixed. They live here because
// this is the target that can build a real page out of the real module and
// answer its RPCs.

// The pane retargeted at another file of the SAME kind. Both files resolve to
// ViewerTextPage, so the inner Loader's `source` string does not change between
// them — and a Loader whose source does not change does not rebuild. The target
// therefore has to be pushed onto the item that is already there; without that
// push the first file's content stayed on screen under the second file's
// address, with nothing to make it re-read.
void TstMobileShell::tappingASecondFileRetargetsTheLoadedViewer()
{
    PageHost host(QStringLiteral("ViewerDirectoryPage"),
                  QStringLiteral(
                      "remotePath: \"/repo/\"\n"
                      "        repoRoot: \"/repo\"\n"
                      // The host's half of the contract, verbatim from
                      // PaneHostPage.navigateTo(): the pane says where it went
                      // and the host assigns that back onto this very item.
                      "        onOpenRequested: function(path) { remotePath = path }"));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();

    QVector<QJsonObject> listings =
        requestsFor(host.requests(), rpc::kMethodListDirectory);
    QCOMPARE(listings.size(), 1);
    QCOMPARE(requestPath(listings.at(0)), QStringLiteral("/repo"));
    host.deliver(listingFrame(requestId(listings.at(0)), {},
                              {QStringLiteral("a.txt"), QStringLiteral("b.txt")}));

    // Tap the first file. This is what the row delegate's onClicked does, with
    // the row the model gave it.
    QVERIFY(callQmlFunction(page, "activate",
                            QVariantMap{{QStringLiteral("name"),
                                         QStringLiteral("a.txt")},
                                        {QStringLiteral("kind"),
                                         QStringLiteral("file")}}));
    QQuickItem *text = findByQmlTypeIn(page, QStringLiteral("ViewerTextPage"));
    QVERIFY2(text, "a text file did not open the text page inside the pane");
    QCOMPARE(text->property("remotePath").toString(),
             QStringLiteral("/repo/a.txt"));
    QVector<QJsonObject> reads =
        requestsFor(host.requests(), rpc::kMethodReadFile);
    QCOMPARE(reads.size(), 1);
    host.deliver(textReadFrame(requestId(reads.at(0)),
                               QStringLiteral("first file"),
                               QStringLiteral("r1")));

    // The host moves the pane to another file — the leaf's url changed, or the
    // pane was restored somewhere else. The listing is not passed through, so
    // the Loader stays active and its source stays the same.
    page->setProperty("remotePath", QStringLiteral("/repo/b.txt"));

    QCOMPARE(findByQmlTypeIn(page, QStringLiteral("ViewerTextPage")), text);
    QCOMPARE(text->property("remotePath").toString(),
             QStringLiteral("/repo/b.txt"));
    reads = requestsFor(host.requests(), rpc::kMethodReadFile);
    QCOMPARE(reads.size(), 1);
    QCOMPARE(requestPath(reads.at(0)), QStringLiteral("/repo/b.txt"));
}

// The default viewer pane. A viewer leaf with NO url is "the session root, i.e.
// a directory listing" (ch::PaneListModel), and PaneHostPage resolves that to
// the repository root — which has no trailing slash. The trailing slash is the
// only thing that spells "directory" in this vocabulary, so without the
// repoRoot clause the pane classified its own starting point as a file and drew
// "cannot show this Binary" over a directory it could have listed.
void TstMobileShell::theSessionRootListsEvenWithoutATrailingSlash()
{
    PageHost host(QStringLiteral("ViewerDirectoryPage"),
                  QStringLiteral("repoRoot: \"/repo\"\n"
                                 "        remotePath: \"/repo\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();

    const QVector<QJsonObject> listings =
        requestsFor(host.requests(), rpc::kMethodListDirectory);
    QCOMPARE(listings.size(), 1);
    QCOMPARE(requestPath(listings.at(0)), QStringLiteral("/repo"));
    QCOMPARE(page->property("contentKind").toString(),
             QStringLiteral("directory"));
    QCOMPARE(countItemsOfType(page, QStringLiteral("ViewerUnsupportedPage")), 0);
}

// One tap, one listing. The pane navigates FIRST and tells the host afterwards,
// so the host's answering assignment names the directory the pane is already
// showing and is ignored; the other order listed the directory twice, once for
// the tap and once for the echo. The tap also renames the pane header, which is
// the only way a single-pane client can say where the user has walked to.
void TstMobileShell::tappingADirectoryListsItExactlyOnceAndRenamesTheHeader()
{
    PageHost host(QStringLiteral("ViewerDirectoryPage"),
                  QStringLiteral(
                      "remotePath: \"/repo/\"\n"
                      "        repoRoot: \"/repo\"\n"
                      "        onOpenRequested: function(path) { remotePath = path }"));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();

    const QVector<QJsonObject> listings =
        requestsFor(host.requests(), rpc::kMethodListDirectory);
    QCOMPARE(listings.size(), 1);
    host.deliver(listingFrame(requestId(listings.at(0)),
                              {QStringLiteral("src")}, {}));

    QSignalSpy titles(page, SIGNAL(titleRequested(QString)));
    QVERIFY(callQmlFunction(page, "activate",
                            QVariantMap{{QStringLiteral("name"),
                                         QStringLiteral("src")},
                                        {QStringLiteral("kind"),
                                         QStringLiteral("directory")}}));

    const QVector<QJsonObject> afterTap =
        requestsFor(host.requests(), rpc::kMethodListDirectory);
    QCOMPARE(afterTap.size(), 1);
    QCOMPARE(requestPath(afterTap.at(0)), QStringLiteral("/repo/src"));
    QCOMPARE(titles.size(), 1);
    QCOMPARE(titles.at(0).at(0).toString(), QStringLiteral("src"));
}

// "Empty" is a claim, and a page may only make it once the server has answered.
// An empty directory and a directory whose listing is still on the wire looked
// identical — a blank area under an address — and saying "empty" during the
// wait would be wrong for every directory that is not.
void TstMobileShell::anEmptyDirectorySaysSoOnlyOnceItHasAnswered()
{
    PageHost host(QStringLiteral("ViewerDirectoryPage"),
                  QStringLiteral("remotePath: \"/repo/\"\n"
                                 "        repoRoot: \"/repo\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();

    QQuickItem *notice =
        findItemShowing(page, QStringLiteral("This directory is empty."));
    QVERIFY2(notice, "the empty-directory notice is not in the page at all");
    QVERIFY2(!notice->isVisible(),
             "the pane called a directory empty before the server answered");

    const QVector<QJsonObject> listings =
        requestsFor(host.requests(), rpc::kMethodListDirectory);
    QCOMPARE(listings.size(), 1);
    host.deliver(listingFrame(requestId(listings.at(0)), {}, {}));
    QVERIFY(notice->isVisible());
}

// Leaving a file with unsaved edits ASKS, and until it is answered the page keeps
// naming the file whose bytes are actually in the buffer. The header used to
// follow `remotePath`, which the host has already moved on — so it named the file
// the user was about to open over the buffer of the one they were still editing,
// which is a lie about what the Save button would write.
void TstMobileShell::leavingADirtyBufferAsksFirstAndKeepsNamingTheOpenFile()
{
    PageHost host(QStringLiteral("ViewerEditorPage"),
                  QStringLiteral("remotePath: \"/repo/a.txt\"\n"
                                 "        paneId: \"p1\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();
    QVERIFY(openEditor(page, QStringLiteral("one"), QStringLiteral("r1"),
                       host.requests(), host.fixture.transport));
    host.requests();  // the watch and stat the load chains behind itself

    QQuickItem *editor = page->findChild<QQuickItem *>(QStringLiteral("editorText"));
    QVERIFY(editor);
    QCOMPARE(editor->property("text").toString(), QStringLiteral("one"));
    editor->setProperty("text", QStringLiteral("one edited"));
    QVERIFY(page->property("dirty").toBool());

    // The host retargets the pane while the buffer is dirty.
    page->setProperty("remotePath", QStringLiteral("/repo/b.txt"));

    QObject *guard = page->findChild<QObject *>(QStringLiteral("unsavedChangesDialog"));
    QVERIFY(guard);
    QVERIFY2(guard->property("visible").toBool(),
             "the pane moved off a dirty buffer without asking");
    QCOMPARE(page->property("openedPath").toString(),
             QStringLiteral("/repo/a.txt"));
    QVERIFY2(findItemShowing(page, QStringLiteral("/repo/a.txt \u2022")),
             "the header named a file the buffer does not hold");
    // Nothing was opened behind the question.
    QVERIFY(requestsFor(host.requests(), rpc::kMethodReadFile).isEmpty());

    // Cancel: the buffer, the header and the pending target are all left alone.
    QVERIFY(QMetaObject::invokeMethod(guard, "reject"));
    QCOMPARE(page->property("openedPath").toString(),
             QStringLiteral("/repo/a.txt"));
    QCOMPARE(page->property("pendingPath").toString(), QString());
    QCOMPARE(editor->property("text").toString(), QStringLiteral("one edited"));
    QVERIFY(page->property("dirty").toBool());
}

// A load landing on a buffer with unsaved edits does not win. The edits stay and
// the revision stays the one they were made against, so the next save reports a
// conflict and the user decides; adopting the incoming revision would let that
// save silently overwrite the very change the content came from.
void TstMobileShell::aLoadLandingOnADirtyBufferKeepsTheEditsAndFlagsAConflict()
{
    PageHost host(QStringLiteral("ViewerEditorPage"),
                  QStringLiteral("remotePath: \"/repo/a.txt\"\n"
                                 "        paneId: \"p1\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();
    QObject *controller = openEditor(page, QStringLiteral("one"),
                                     QStringLiteral("r1"), host.requests(),
                                     host.fixture.transport);
    QVERIFY(controller);

    QQuickItem *editor = page->findChild<QQuickItem *>(QStringLiteral("editorText"));
    QVERIFY(editor);
    editor->setProperty("text", QStringLiteral("mine"));
    QVERIFY(page->property("dirty").toBool());

    // The file changed on the server and the controller re-read it.
    QVERIFY(QMetaObject::invokeMethod(controller, "contentLoaded",
                                      Q_ARG(QString, QStringLiteral("theirs")),
                                      Q_ARG(QString, QStringLiteral("r2"))));

    QCOMPARE(editor->property("text").toString(), QStringLiteral("mine"));
    QVERIFY2(page->property("conflict").toBool(),
             "the buffer survived the reload but the user was never told");
    QCOMPARE(page->property("revision").toString(), QStringLiteral("r1"));
    QVERIFY(page->property("dirty").toBool());
}

// A keystroke that lands while the write is in flight is not in the file.
// Clearing the dirty flag unconditionally when the save answered disabled the
// Save button over an edit that had never been written anywhere.
void TstMobileShell::anEditMadeWhileASaveIsInFlightStaysUnsaved()
{
    PageHost host(QStringLiteral("ViewerEditorPage"),
                  QStringLiteral("remotePath: \"/repo/a.txt\"\n"
                                 "        paneId: \"p1\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();
    QVERIFY(openEditor(page, QStringLiteral("one"), QStringLiteral("r1"),
                       host.requests(), host.fixture.transport));
    host.requests();

    QQuickItem *editor = page->findChild<QQuickItem *>(QStringLiteral("editorText"));
    QVERIFY(editor);
    editor->setProperty("text", QStringLiteral("first edit"));

    QQuickItem *save = findItemShowing(page, QStringLiteral("Save"));
    QVERIFY(save);
    QVERIFY(save->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    const QVector<QJsonObject> writes =
        requestsFor(host.requests(), rpc::kMethodWriteFile);
    QCOMPARE(writes.size(), 1);
    QCOMPARE(requestPath(writes.at(0)), QStringLiteral("/repo/a.txt"));

    // The user keeps typing while the write is on the wire.
    editor->setProperty("text", QStringLiteral("second edit"));
    host.deliver(resultFrame(requestId(writes.at(0)),
                             QJsonObject{{"revision", "r2"}}));

    QVERIFY2(page->property("dirty").toBool(),
             "an edit made during the save was reported as saved");
    QVERIFY2(save->property("enabled").toBool(),
             "the Save button went dead over an unwritten edit");
}

// The FIRST keystroke is reported to the controller immediately, not after the
// 500 ms recovery debounce. reportContent() is also what marks the controller's
// buffer dirty, and ch::EditorController only auto-reloads a buffer it believes
// is clean — so an external change arriving inside the debounce window would
// re-read the file straight over what was just typed. The snapshot write is the
// observable half: it is on the wire before any timer could have fired.
void TstMobileShell::theFirstKeystrokeIsReportedWithoutWaitingForTheDebounce()
{
    PageHost host(QStringLiteral("ViewerEditorPage"),
                  QStringLiteral("remotePath: \"/repo/a.txt\"\n"
                                 "        paneId: \"p1\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();
    QVERIFY(openEditor(page, QStringLiteral("one"), QStringLiteral("r1"),
                       host.requests(), host.fixture.transport));
    // Recovery is keyed by the server-reported directory and this pane's id;
    // without a directory the snapshot is disabled and writes nothing.
    host.editors.setRecoveryDir(QStringLiteral("/srv/recovery"));
    host.requests();

    QQuickItem *editor = page->findChild<QQuickItem *>(QStringLiteral("editorText"));
    QVERIFY(editor);
    editor->setProperty("text", QStringLiteral("o"));

    const QVector<QJsonObject> writes =
        requestsFor(host.requests(), rpc::kMethodWriteFile);
    QCOMPARE(writes.size(), 1);
    QCOMPARE(requestPath(writes.at(0)), QStringLiteral("/srv/recovery/p1"));
    // And it was not the debounce that did it: the timer never started.
    QObject *timer = page->findChild<QObject *>(QStringLiteral("recoverySnapshotTimer"));
    QVERIFY(timer);
    QVERIFY(!timer->property("running").toBool());
}

// A CRLF file is split on every line terminator, not on "\n" alone. A carriage
// return left at the end of every line renders as a stray glyph and widens the
// line for wrapping, on a file the desktop shows cleanly.
void TstMobileShell::aCrlfFileIsSplitWithoutCarriageReturns()
{
    PageHost host(QStringLiteral("ViewerTextPage"),
                  QStringLiteral("remotePath: \"/repo/a.txt\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();

    const QVector<QJsonObject> reads =
        requestsFor(host.requests(), rpc::kMethodReadFile);
    QCOMPARE(reads.size(), 1);
    host.deliver(textReadFrame(requestId(reads.at(0)),
                               QStringLiteral("a\r\nb"), QStringLiteral("r1")));

    QCOMPARE(page->property("lines").toStringList(),
             QStringList({QStringLiteral("a"), QStringLiteral("b")}));
    QQuickItem *lines = page->findChild<QQuickItem *>(QStringLiteral("textLines"));
    QVERIFY(lines);
    QCOMPARE(lines->property("count").toInt(), 2);
    // QTRY, because a ListView creates its delegates when it lays out and not
    // when its model changes: the row items exist a turn of the loop later.
    QTRY_VERIFY(findItemShowing(page, QStringLiteral("a")));
    QTRY_VERIFY(findItemShowing(page, QStringLiteral("b")));
}

// The two list markers, which were the wrong way round. An unordered item gets
// the bullet every reader already knows; an ordered one gets a dash, because
// ch::MarkdownModel carries no ordinal and a bullet would claim the list was
// unnumbered.
void TstMobileShell::anOrderedListItemIsNotDrawnAsABullet()
{
    PageHost host(QStringLiteral("ViewerMarkdownPage"),
                  QStringLiteral("remotePath: \"/repo/notes.md\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();

    const QVector<QJsonObject> reads =
        requestsFor(host.requests(), rpc::kMethodReadFile);
    QCOMPARE(reads.size(), 1);
    host.deliver(textReadFrame(requestId(reads.at(0)),
                               QStringLiteral("- unordered\n\n1. ordered\n"),
                               QStringLiteral("r1")));

    QQuickItem *blocks = page->findChild<QQuickItem *>(QStringLiteral("markdownBlocks"));
    QVERIFY(blocks);
    QCOMPARE(blocks->property("count").toInt(), 2);
    // QTRY for the same reason as the text case: the markers live in delegates.
    QTRY_VERIFY2(findItemShowing(page, QString::fromUtf8("\u2022")),
                 "the unordered item is not drawn with a bullet");
    QTRY_VERIFY2(findItemShowing(page, QString::fromUtf8("\u2013")),
                 "the ordered item is not drawn with a dash");
}

// An image pane hands its bytes back when it moves on. The cache is LRU-bounded
// so this is not a leak, but an image may be 8 MiB and a phone has better uses
// for that than a picture nobody is looking at.
void TstMobileShell::retargetingTheImagePaneHandsBackTheBytes()
{
    PageHost host(QStringLiteral("ViewerImagePage"),
                  QStringLiteral("remotePath: \"/repo/a.png\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();

    const QVector<QJsonObject> reads =
        requestsFor(host.requests(), rpc::kMethodReadFile);
    QCOMPARE(reads.size(), 1);
    host.deliver(byteReadFrame(requestId(reads.at(0)), pngBytes()));
    QVERIFY(!host.viewers.cachedImageBytes(QStringLiteral("/repo/a.png")).isEmpty());

    page->setProperty("remotePath", QStringLiteral("/repo/b.png"));
    QVERIFY2(host.viewers.cachedImageBytes(QStringLiteral("/repo/a.png")).isEmpty(),
             "the pane kept the previous image's bytes after moving on");

    // And the read the pane ABANDONED on its way past: its reply lands after the
    // release, and must not put bytes into a cache nobody is left to hand back.
    const QVector<QJsonObject> abandoned =
        requestsFor(host.requests(), rpc::kMethodReadFile);
    QCOMPARE(abandoned.size(), 1);
    QCOMPARE(requestPath(abandoned.at(0)), QStringLiteral("/repo/b.png"));
    page->setProperty("remotePath", QStringLiteral("/repo/c.png"));
    host.deliver(byteReadFrame(requestId(abandoned.at(0)), pngBytes()));
    QVERIFY2(host.viewers.cachedImageBytes(QStringLiteral("/repo/b.png")).isEmpty(),
             "a released request still cached its bytes when it answered");
}

// The app-private PDF spool holds at most the document on screen. It exists only
// because QtQuick.Pdf's PdfDocument takes a URL and nothing else, so every file
// in it is a remote document sitting in this device's cache: one per pane, and
// none once the pane is gone.
//
// Three ways a spool file must go: the pane moving on, the pane dying, and a read
// the pane abandoned mid-flight — that last one is a reply nobody is left to
// release, so if it wrote its file anyway it would sit in the cache until the
// app exits.
void TstMobileShell::thePdfSpoolFileIsDeletedWhenThePaneMovesOnAndWhenItDies()
{
#if !CH_HAVE_QTPDF
    QSKIP("this build has no Qt Pdf, so ViewerPdfPage is not in the QML module");
#else
    const QString spoolDir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/pdf-spool");
    const auto spoolFor = [&spoolDir](const QString &path) {
        // The service names the file by a digest of the remote path, never by
        // its basename: a server-controlled name must not choose a filename on
        // this device.
        return spoolDir + QLatin1Char('/')
               + QString::fromLatin1(
                   QCryptographicHash::hash(path.toUtf8(),
                                            QCryptographicHash::Sha256)
                       .toHex())
               + QStringLiteral(".pdf");
    };

    PageHost host(QStringLiteral("ViewerPdfPage"),
                  QStringLiteral("remotePath: \"/repo/a.pdf\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();

    // The abandoned read first: retarget the pane while its reply is still on the
    // wire, then let it land.
    QVector<QJsonObject> reads = requestsFor(host.requests(), rpc::kMethodReadFile);
    QCOMPARE(reads.size(), 1);
    QCOMPARE(requestPath(reads.at(0)), QStringLiteral("/repo/a.pdf"));
    page->setProperty("remotePath", QStringLiteral("/repo/b.pdf"));
    const QVector<QJsonObject> live =
        requestsFor(host.requests(), rpc::kMethodReadFile);
    QCOMPARE(live.size(), 1);
    QCOMPARE(requestPath(live.at(0)), QStringLiteral("/repo/b.pdf"));
    host.deliver(byteReadFrame(requestId(reads.at(0)),
                               QByteArrayLiteral("%PDF-1.4\n")));
    QVERIFY2(!QFile::exists(spoolFor(QStringLiteral("/repo/a.pdf"))),
             "a released request still wrote its document into the cache");

    // The live one does produce a file.
    host.deliver(byteReadFrame(requestId(live.at(0)),
                               QByteArrayLiteral("%PDF-1.4\n")));
    QVERIFY(QFile::exists(spoolFor(QStringLiteral("/repo/b.pdf"))));

    // Moving on releases the document that was on screen.
    page->setProperty("remotePath", QStringLiteral("/repo/c.pdf"));
    QVERIFY2(!QFile::exists(spoolFor(QStringLiteral("/repo/b.pdf"))),
             "the previous document was left in the cache");

    reads = requestsFor(host.requests(), rpc::kMethodReadFile);
    QCOMPARE(reads.size(), 1);
    host.deliver(byteReadFrame(requestId(reads.at(0)),
                               QByteArrayLiteral("%PDF-1.4\n")));
    QVERIFY(QFile::exists(spoolFor(QStringLiteral("/repo/c.pdf"))));

    // And so does the pane going away. Only the QML object graph is destroyed
    // here: the service outlives it, so this is the PAGE releasing the file and
    // not the service's destructor purging the whole directory.
    host.root.reset();
    QVERIFY2(!QFile::exists(spoolFor(QStringLiteral("/repo/b.pdf"))),
             "the document outlived the pane that was showing it");
#endif
}

// A loaded page can navigate itself — a tapped link, a redirect, a meta refresh —
// and QtWebView has no request interceptor, so the start of the load is the
// earliest point this client can act. A file:// destination there would have the
// platform web view render the app's own sandbox (SPEC 7.4).
void TstMobileShell::aNavigationToANonHttpAddressIsRefusedAndSaidSo()
{
#if !CH_HAVE_QTWEBVIEW
    QSKIP("this build has no Qt WebView, so ViewerWebPage is not in the QML "
          "module");
#else
    PageHost host(QStringLiteral("ViewerWebPage"),
                  QStringLiteral("webUrl: \"https://example.com/\""));
    QVERIFY(showPage(host));
    QQuickItem *page = host.page();

    const QString refused = QStringLiteral("file:///data/data/app/secrets");
    QVERIFY(callQmlFunction(page, "noteNavigation", refused));
    QCOMPARE(page->property("refusedNavigation").toString(), refused);
    QQuickItem *banner = findItemShowing(
        page,
        QStringLiteral("This page tried to open an address that is not http or "
                       "https, and was stopped: %1").arg(refused));
    QVERIFY2(banner, "the refusal is not shown anywhere");
    QVERIFY(banner->isVisible());

    // An allowed destination clears it: a refusal must not outlive the page it
    // happened on.
    QVERIFY(callQmlFunction(page, "noteNavigation",
                            QStringLiteral("https://example.com/next")));
    QCOMPARE(page->property("refusedNavigation").toString(), QString());
    QVERIFY(!banner->isVisible());
#endif
}

// A QGuiApplication is required (this loads and renders real QML), and the style
// must match what src/mobile/main.cpp installs or the pages would be measured
// against different control metrics than they ship with.
int main(int argc, char *argv[])
{
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication app(argc, argv);
    TstMobileShell test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_mobileshell.moc"
