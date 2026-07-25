#include <QtTest/QtTest>

#include <QTemporaryDir>
#include <QPair>

#include "AppController.h"
#include "UiStateStore.h"
#include "WorkspaceDb.h"
#include "WorkspaceTypes.h"

using namespace ch;

class TstAppController : public QObject {
    Q_OBJECT

private slots:
    void toGroupRowsMapsNestedNodes();
    void toGroupRowsEmptyIsEmpty();
    void uiStateStorePersistsAcrossInstances();
    void uiStateStoreDocumentedDefaults();
    void toGroupRowsSubtitleHandlesTrailingSlashAndEmpty();
    void uiStateStoreDistinctAndSpecialIds();
    void uiStateStoreRegionWidthsPersistWithoutPerCallSync();
};

// Two GroupNodes with sessions map to GroupRows preserving order, with the
// session subtitle set to the basename of repositoryRoot.
void TstAppController::toGroupRowsMapsNestedNodes()
{
    GroupNode g1;
    g1.group.id = GroupId{QStringLiteral("g1")};
    g1.group.name = QStringLiteral("Work");

    SessionNode s1;
    s1.session.id = DevSessionId{QStringLiteral("s1")};
    s1.session.name = QStringLiteral("codeharbor");
    s1.session.repositoryRoot = QStringLiteral("/home/u/proj");

    SessionNode s2;
    s2.session.id = DevSessionId{QStringLiteral("s2")};
    s2.session.name = QStringLiteral("docs");
    s2.session.repositoryRoot = QStringLiteral("/home/u/manual");
    g1.sessions = {s1, s2};

    GroupNode g2;
    g2.group.id = GroupId{QStringLiteral("g2")};
    g2.group.name = QStringLiteral("Personal");

    SessionNode s3;
    s3.session.id = DevSessionId{QStringLiteral("s3")};
    s3.session.name = QStringLiteral("dotfiles");
    s3.session.repositoryRoot = QStringLiteral("/home/u/config");
    g2.sessions = {s3};

    const QVector<GroupRow> rows = AppController::toGroupRows({g1, g2});

    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).group.name, QStringLiteral("Work"));
    QCOMPARE(rows.at(1).group.name, QStringLiteral("Personal"));

    QCOMPARE(rows.at(0).sessions.size(), 2);
    QCOMPARE(rows.at(0).sessions.at(0).session.name, QStringLiteral("codeharbor"));
    QCOMPARE(rows.at(0).sessions.at(0).subtitle, QStringLiteral("proj"));
    QCOMPARE(rows.at(0).sessions.at(1).subtitle, QStringLiteral("manual"));
    QVERIFY(rows.at(0).sessions.at(0).terminals.isEmpty());

    QCOMPARE(rows.at(1).sessions.size(), 1);
    QCOMPARE(rows.at(1).sessions.at(0).session.name, QStringLiteral("dotfiles"));
    QCOMPARE(rows.at(1).sessions.at(0).subtitle, QStringLiteral("config"));
}

void TstAppController::toGroupRowsEmptyIsEmpty()
{
    QVERIFY(AppController::toGroupRows({}).isEmpty());
}

// A fresh store over the same .ini file reads back exactly what a previous
// instance wrote — proving persistence via QSettings' flush on destruction
// (setRegionWidths no longer sync()s per call, to avoid handle-drag jank).
void TstAppController::uiStateStorePersistsAcrossInstances()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("ui.ini"));

    {
        UiStateStore store(iniPath);
        store.setRegionWidths(200, 0, 400);
        store.setSelectedPane(QStringLiteral("s1"), QStringLiteral("p9"));
    }

    UiStateStore reopened(iniPath);
    QCOMPARE(reopened.sidebarWidth(), 200);
    QCOMPARE(reopened.viewerWidth(), 0);
    QCOMPARE(reopened.terminalWidth(), 400);
    QCOMPARE(reopened.selectedPane(QStringLiteral("s1")), QStringLiteral("p9"));
}

// Documented defaults when nothing has been written.
void TstAppController::uiStateStoreDocumentedDefaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("empty.ini"));

    UiStateStore store(iniPath);
    QCOMPARE(store.sidebarWidth(), 260);
    QCOMPARE(store.viewerWidth(), 0);
    QCOMPARE(store.terminalWidth(), 520);
    QVERIFY(store.selectedPane(QStringLiteral("unknown")).isEmpty());
}

// repositoryRoot basenames: a trailing slash (or several), a "." segment, and
// relative paths still yield the final path component; an empty or root path
// yields an empty subtitle. Guards the QFileInfo trailing-slash pitfall
// (QFileInfo("/a/b/").fileName() == "").
void TstAppController::toGroupRowsSubtitleHandlesTrailingSlashAndEmpty()
{
    GroupNode g;
    g.group.id = GroupId{QStringLiteral("g")};

    const QVector<QPair<QString, QString>> cases = {
        {QStringLiteral("/home/u/proj/"), QStringLiteral("proj")},
        {QStringLiteral("/home/u/proj//"), QStringLiteral("proj")},
        {QStringLiteral("/home/u/./proj"), QStringLiteral("proj")},
        {QStringLiteral("relative/"), QStringLiteral("relative")},
        {QString(), QString()},
        {QStringLiteral("/"), QString()},
    };
    for (const auto& c : cases) {
        SessionNode s;
        s.session.id = DevSessionId{c.first};
        s.session.repositoryRoot = c.first;
        g.sessions.push_back(s);
    }

    const QVector<GroupRow> rows = AppController::toGroupRows({g});
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).sessions.size(), cases.size());
    for (qsizetype i = 0; i < cases.size(); ++i)
        QCOMPARE(rows.at(0).sessions.at(i).subtitle, cases.at(i).second);
}

// Distinct devSessionIds address independent panes (no key collision), and ids
// containing separator/special characters round-trip intact across a reopen.
void TstAppController::uiStateStoreDistinctAndSpecialIds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("panes.ini"));

    {
        UiStateStore store(iniPath);
        store.setSelectedPane(QStringLiteral("s1"), QStringLiteral("viewer"));
        store.setSelectedPane(QStringLiteral("s2"), QStringLiteral("terminal"));
        // Separator/space-bearing ids must not collide or corrupt the key.
        store.setSelectedPane(QStringLiteral("srv/grp:has space"),
                              QStringLiteral("editor"));
    }

    UiStateStore reopened(iniPath);
    QCOMPARE(reopened.selectedPane(QStringLiteral("s1")), QStringLiteral("viewer"));
    QCOMPARE(reopened.selectedPane(QStringLiteral("s2")), QStringLiteral("terminal"));
    QCOMPARE(reopened.selectedPane(QStringLiteral("srv/grp:has space")),
             QStringLiteral("editor"));
}

// setRegionWidths no longer calls QSettings::sync() on every invocation (a
// handle drag fires it repeatedly; a synchronous disk write per pixel caused
// jank). Simulate a drag with many writes, then prove the final values still
// persist to a fresh instance via the destructor flush — no explicit sync.
void TstAppController::uiStateStoreRegionWidthsPersistWithoutPerCallSync()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("drag.ini"));

    {
        UiStateStore store(iniPath);
        // Rapid intermediate writes, as during a handle drag.
        for (int w = 180; w <= 300; ++w)
            store.setRegionWidths(w, 0, 640 - w);
        // Final settled widths.
        store.setRegionWidths(300, 0, 340);
        // No explicit sync() here: the destructor at end of scope flushes.
    }

    UiStateStore reopened(iniPath);
    QCOMPARE(reopened.sidebarWidth(), 300);
    QCOMPARE(reopened.viewerWidth(), 0);
    QCOMPARE(reopened.terminalWidth(), 340);
}

QTEST_GUILESS_MAIN(TstAppController)
#include "tst_appcontroller.moc"
