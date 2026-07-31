// Connection profiles (ch::ServerProfiles) and the QML sheet that edits them.
//
// Two halves, one gate:
//   * the store — CRUD, persistence across instances against an explicit .ini
//     path (never the developer's real settings), the activeId invariant, and
//     refusal of input that could never connect;
//   * ConnectSheet.qml — loaded straight from the source tree (by file path, not
//     through the CodeHarbor QML module this target does not link) and driven
//     through every signal it exposes, failing on ANY QML warning so it cannot
//     poison tst_qmlload. Loading it outside its module means the module's
//     `Theme` singleton is not there, so loadSheet() registers the real
//     src/qml/Theme.qml and exposes its one instance under the same name.

#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickView>
#include <QSettings>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <utility>

#include "ServerProfiles.h"

using namespace ch;

namespace {

// ---------------------------------------------------------------------------
// QML warning capture: the sheet must be silent. Two nets, mirroring
// src/qml/tests/tst_qmlload.cpp: the engine's own warning signal plus a message
// handler filtered to this file's URL (runtime binding errors are logged, not
// signalled, when they happen outside load()).
// ---------------------------------------------------------------------------

QMutex g_logMutex;
QStringList g_loggedWarnings;
QtMessageHandler g_previousHandler = nullptr;

void warningCollector(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    if (type != QtDebugMsg && type != QtInfoMsg && msg.contains(QLatin1String("ConnectSheet.qml"))) {
        QMutexLocker locker(&g_logMutex);
        g_loggedWarnings.append(msg);
    }
    if (g_previousHandler)
        g_previousHandler(type, context, msg);
}

QStringList takeLoggedWarnings()
{
    QMutexLocker locker(&g_logMutex);
    return std::exchange(g_loggedWarnings, QStringList());
}

QVariantMap profileFields(const QString& name, const QString& host, const QVariant& port,
                          const QString& user, const QString& nodePath = QString(),
                          const QString& repoRoot = QString(),
                          const QString& identityFile = QString())
{
    return QVariantMap{{QStringLiteral("name"), name},
                       {QStringLiteral("host"), host},
                       {QStringLiteral("port"), port},
                       {QStringLiteral("user"), user},
                       {QStringLiteral("identityFile"), identityFile},
                       {QStringLiteral("nodePath"), nodePath},
                       {QStringLiteral("repoRoot"), repoRoot}};
}

QStringList idsOf(const QVariantList& profiles)
{
    QStringList ids;
    ids.reserve(profiles.size());
    for (const QVariant& entry : profiles)
        ids.append(entry.toMap().value(QStringLiteral("id")).toString());
    return ids;
}

QStringList namesOf(const QVariantList& profiles)
{
    QStringList names;
    names.reserve(profiles.size());
    for (const QVariant& entry : profiles)
        names.append(entry.toMap().value(QStringLiteral("name")).toString());
    return names;
}

// A QML `var` signal argument arrives as a QJSValue, not a QVariantMap.
QVariantMap asMap(const QVariant& value)
{
    if (value.userType() == qMetaTypeId<QJSValue>())
        return value.value<QJSValue>().toVariant().toMap();
    return value.toMap();
}

QString stringOf(QObject* object, const char* property)
{
    return object ? object->property(property).toString() : QString();
}

// QObject::findChild() is not enough for QML: view delegates are parented into
// the VISUAL tree (contentItem) while their QObject parent stays elsewhere, so
// a ListView row is invisible to findChild. Walk both lists, like
// src/qml/tests/tst_qmlload.cpp does.
QObject* findByName(QObject* root, const QString& name, QSet<const QObject*>* seen = nullptr)
{
    QSet<const QObject*> local;
    QSet<const QObject*>& visited = seen ? *seen : local;
    if (!root || visited.contains(root))
        return nullptr;
    visited.insert(root);
    if (root->objectName() == name)
        return root;

    for (QObject* child : root->children()) {
        if (QObject* found = findByName(child, name, &visited))
            return found;
    }
    if (auto* item = qobject_cast<QQuickItem*>(root)) {
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = findByName(child, name, &visited))
                return found;
        }
    }
    return nullptr;
}

} // namespace

// The wiring the orchestrator owns, spelled out once so this test can prove the
// store and the sheet actually compose: a sheet signal becomes a ServerProfiles
// mutation, and the store's change notifications are pushed straight back into
// the sheet's input properties. Main.qml/AppController will do exactly this.
class SheetBridge : public QObject {
    Q_OBJECT

public:
    SheetBridge(ServerProfiles* store, QQuickItem* sheet)
        : QObject(sheet), m_store(store), m_sheet(sheet)
    {
        connect(sheet, SIGNAL(profileSaved(QVariant)), this, SLOT(onProfileSaved(QVariant)));
        connect(sheet, SIGNAL(profileRemoved(QString)), this, SLOT(onProfileRemoved(QString)));
        connect(sheet, SIGNAL(connectRequested(QString)), this, SLOT(onConnectRequested(QString)));
        connect(store, &ServerProfiles::profilesChanged, this, &SheetBridge::push);
        connect(store, &ServerProfiles::activeIdChanged, this, &SheetBridge::push);
        push();
    }

    QStringList connectRequests;
    QString lastSavedId;

public slots:
    void onProfileSaved(QVariant fields)
    {
        const QVariantMap map = asMap(fields);
        const QString id = map.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            lastSavedId = m_store->addProfile(map);
        } else {
            m_store->updateProfile(id, map);
            lastSavedId = id;
        }
    }
    void onProfileRemoved(QString id) { m_store->removeProfile(id); }
    void onConnectRequested(QString id) { connectRequests.append(id); }
    void push()
    {
        m_sheet->setProperty("profiles", m_store->profiles());
        m_sheet->setProperty("activeId", m_store->activeId());
    }

private:
    ServerProfiles* m_store = nullptr;
    QQuickItem* m_sheet = nullptr;
};

class TstServerProfiles : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // ---- ch::ServerProfiles ----
    void emptyStoreHasNothingSelected();
    void addRoundTripsEveryFieldAcrossInstances();
    void addRejectsInputThatCouldNeverConnect();
    void addNormalizesFields();
    void activeIdOnlyEverNamesAnExistingProfile();
    void removeAdvancesTheActiveSelection();
    void updateMergesFieldsAndRefusesGarbage();
    void unicodeAndSpacesSurviveTheIniRoundTrip();
    void insertionOrderSurvivesReload();
    void handEditedStoreIsRepairedOnLoad();
    void secretsInTheCallersMapNeverReachTheStore();

    // ---- ConnectSheet.qml ----
    void sheetLoadsSilentlyAndExposesItsApi();
    void sheetListsProfilesAndMarksTheActiveOne();
    void sheetEmitsConnectAndRemoveForTheSelection();
    void sheetSavesANewProfileThenEditsIt();
    void sheetHostKeyPromptDecidesBothWays();
    void sheetCredentialPromptMasksSubmitsAndKeepsTheSecretOffDisk();
    void sheetSurfacesErrorTextWithoutBlocking();
    void sheetOpensSshDiagnostics();
    void sheetIsUsableFromTheKeyboardAlone();

    // ---- both halves together ----
    void coldStartAddsAServerThenFindsItAgainAfterRelaunch();

private:
    // Fresh ini path inside the case-scoped temp dir.
    QString iniPath(const QString& name) const { return m_dir.filePath(name); }

    // Loads ConnectSheet.qml into a shown QQuickView, asserting a clean load.
    // Returns nullptr (after recording failures) if anything went wrong.
    std::unique_ptr<QQuickView> loadSheet();

    QTemporaryDir m_dir;
    QStringList m_engineWarnings;
};

void TstServerProfiles::initTestCase()
{
    QVERIFY(m_dir.isValid());
    g_previousHandler = qInstallMessageHandler(warningCollector);
}

void TstServerProfiles::cleanupTestCase()
{
    qInstallMessageHandler(g_previousHandler);
}

// ---------------------------------------------------------------------------
// ch::ServerProfiles
// ---------------------------------------------------------------------------

void TstServerProfiles::emptyStoreHasNothingSelected()
{
    ServerProfiles store(iniPath(QStringLiteral("empty.ini")));
    QVERIFY(store.profiles().isEmpty());
    QVERIFY(store.activeId().isEmpty());
    QVERIFY(store.profile(QStringLiteral("nope")).isEmpty());
    // Selecting a profile that does not exist is ignored, not stored.
    store.setActiveId(QStringLiteral("nope"));
    QVERIFY(store.activeId().isEmpty());
}

void TstServerProfiles::addRoundTripsEveryFieldAcrossInstances()
{
    const QString path = iniPath(QStringLiteral("roundtrip.ini"));
    QString id;

    {
        ServerProfiles store(path);
        QSignalSpy profilesSpy(&store, &ServerProfiles::profilesChanged);
        QSignalSpy activeSpy(&store, &ServerProfiles::activeIdChanged);

        id = store.addProfile(profileFields(QStringLiteral("Prod box"),
                                            QStringLiteral("10.0.0.4"), 2222,
                                            QStringLiteral("yichen"),
                                            QStringLiteral("/home/yichen/.local/bin/node"),
                                            QStringLiteral("/srv/codeharbor")));
        QVERIFY(!id.isEmpty());
        // Ids are QUuids without braces, so they are safe as INI key fragments.
        QVERIFY(!QUuid::fromString(id).isNull());
        QVERIFY(!id.contains(QLatin1Char('{')));
        QCOMPARE(profilesSpy.count(), 1);
        // The first profile added with nothing selected becomes the selection.
        QCOMPARE(activeSpy.count(), 1);
        QCOMPARE(store.activeId(), id);

        QCOMPARE(store.profiles().size(), 1);
        const QVariantMap stored = store.profile(id);
        QCOMPARE(stored.value(QStringLiteral("id")).toString(), id);
        QCOMPARE(stored.value(QStringLiteral("name")).toString(), QStringLiteral("Prod box"));
        QCOMPARE(stored.value(QStringLiteral("host")).toString(), QStringLiteral("10.0.0.4"));
        QCOMPARE(stored.value(QStringLiteral("user")).toString(), QStringLiteral("yichen"));
        QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(),
                 QStringLiteral("/home/yichen/.local/bin/node"));
        QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(),
                 QStringLiteral("/srv/codeharbor"));
        // The port stays an int, both in value and in metatype: QML binds it to
        // a spin/int field and SshConnectionPool takes a quint16.
        QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 2222);
        QCOMPARE(stored.value(QStringLiteral("port")).typeId(), QMetaType::Int);
        // profiles() and profile() agree.
        QCOMPARE(store.profiles().first().toMap(), stored);
    }

    // A brand-new instance over the same file sees exactly the same records.
    ServerProfiles reopened(path);
    QCOMPARE(reopened.profiles().size(), 1);
    QCOMPARE(reopened.activeId(), id);
    const QVariantMap stored = reopened.profile(id);
    QCOMPARE(stored.value(QStringLiteral("name")).toString(), QStringLiteral("Prod box"));
    QCOMPARE(stored.value(QStringLiteral("host")).toString(), QStringLiteral("10.0.0.4"));
    QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 2222);
    QCOMPARE(stored.value(QStringLiteral("port")).typeId(), QMetaType::Int);
    QCOMPARE(stored.value(QStringLiteral("user")).toString(), QStringLiteral("yichen"));
    QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(),
             QStringLiteral("/home/yichen/.local/bin/node"));
    QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(),
             QStringLiteral("/srv/codeharbor"));

    // Pin the documented on-disk layout: [servers] with <id>/<field> keys.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString ini = QString::fromUtf8(file.readAll());
    QVERIFY2(ini.contains(QStringLiteral("[servers]")), qPrintable(ini));
    // IniFormat escapes the '/' inside a key as '\', so `servers/<id>/host`
    // lands as key `<id>\host` under [servers].
    QVERIFY2(ini.contains(id + QStringLiteral("\\host=10.0.0.4")), qPrintable(ini));
    QVERIFY2(ini.contains(id + QStringLiteral("\\port=2222")), qPrintable(ini));
    QVERIFY2(ini.contains(QStringLiteral("active=") + id), qPrintable(ini));
}

void TstServerProfiles::addRejectsInputThatCouldNeverConnect()
{
    ServerProfiles store(iniPath(QStringLiteral("reject.ini")));
    QSignalSpy profilesSpy(&store, &ServerProfiles::profilesChanged);

    const QList<QVariantMap> broken = {
        // no host at all
        QVariantMap{{QStringLiteral("user"), QStringLiteral("u")}},
        // empty / whitespace-only host
        profileFields(QStringLiteral("n"), QString(), 22, QStringLiteral("u")),
        profileFields(QStringLiteral("n"), QStringLiteral("   "), 22, QStringLiteral("u")),
        // no user: connectToHost(host, port, user) cannot work without one
        QVariantMap{{QStringLiteral("host"), QStringLiteral("h")}},
        profileFields(QStringLiteral("n"), QStringLiteral("h"), 22, QStringLiteral("  ")),
        // ports outside 1..65535, and non-numeric text
        profileFields(QStringLiteral("n"), QStringLiteral("h"), 0, QStringLiteral("u")),
        profileFields(QStringLiteral("n"), QStringLiteral("h"), -1, QStringLiteral("u")),
        profileFields(QStringLiteral("n"), QStringLiteral("h"), 65536, QStringLiteral("u")),
        profileFields(QStringLiteral("n"), QStringLiteral("h"), QStringLiteral("22 or so"),
                      QStringLiteral("u")),
    };

    for (const QVariantMap& fields : broken) {
        const QString id = store.addProfile(fields);
        QVERIFY2(id.isEmpty(), qPrintable(QDebug::toString(fields)));
    }

    // Nothing was stored and nothing was announced.
    QVERIFY(store.profiles().isEmpty());
    QCOMPARE(profilesSpy.count(), 0);
    QVERIFY(store.activeId().isEmpty());

    // ...and the rejections left no residue on disk either.
    ServerProfiles reopened(iniPath(QStringLiteral("reject.ini")));
    QVERIFY(reopened.profiles().isEmpty());
}

void TstServerProfiles::addNormalizesFields()
{
    ServerProfiles store(iniPath(QStringLiteral("normalize.ini")));

    // Surrounding whitespace from a UI field is trimmed everywhere, a blank
    // name falls back to the host, and a port given as text becomes an int.
    const QString id = store.addProfile(profileFields(QStringLiteral("  "),
                                                      QStringLiteral("  box.local  "),
                                                      QStringLiteral(" 2222 "),
                                                      QStringLiteral(" yichen\t"),
                                                      QStringLiteral(" /usr/bin/node "),
                                                      QStringLiteral(" /srv/repo ")));
    QVERIFY(!id.isEmpty());
    const QVariantMap stored = store.profile(id);
    QCOMPARE(stored.value(QStringLiteral("name")).toString(), QStringLiteral("box.local"));
    QCOMPARE(stored.value(QStringLiteral("host")).toString(), QStringLiteral("box.local"));
    QCOMPARE(stored.value(QStringLiteral("user")).toString(), QStringLiteral("yichen"));
    QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(), QStringLiteral("/usr/bin/node"));
    QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(), QStringLiteral("/srv/repo"));
    QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 2222);
    QCOMPARE(stored.value(QStringLiteral("port")).typeId(), QMetaType::Int);

    // An omitted or blank port means "the SSH default", not a rejection.
    const QString noPort = store.addProfile(
        QVariantMap{{QStringLiteral("host"), QStringLiteral("a")},
                    {QStringLiteral("user"), QStringLiteral("u")}});
    QVERIFY(!noPort.isEmpty());
    QCOMPARE(store.profile(noPort).value(QStringLiteral("port")).toInt(), 22);

    const QString blankPort = store.addProfile(profileFields(
        QStringLiteral("b"), QStringLiteral("b"), QStringLiteral(""), QStringLiteral("u")));
    QVERIFY(!blankPort.isEmpty());
    QCOMPARE(store.profile(blankPort).value(QStringLiteral("port")).toInt(), 22);

    // A caller-supplied id is ignored: ids are minted by the store.
    QVariantMap withId = profileFields(QStringLiteral("c"), QStringLiteral("c"), 22,
                                       QStringLiteral("u"));
    withId.insert(QStringLiteral("id"), QStringLiteral("i-picked-this"));
    const QString minted = store.addProfile(withId);
    QVERIFY(!minted.isEmpty());
    QVERIFY(minted != QStringLiteral("i-picked-this"));
    QVERIFY(store.profile(QStringLiteral("i-picked-this")).isEmpty());
}

void TstServerProfiles::activeIdOnlyEverNamesAnExistingProfile()
{
    const QString path = iniPath(QStringLiteral("active.ini"));
    ServerProfiles store(path);

    const QString first = store.addProfile(
        profileFields(QStringLiteral("one"), QStringLiteral("h1"), 22, QStringLiteral("u")));
    const QString second = store.addProfile(
        profileFields(QStringLiteral("two"), QStringLiteral("h2"), 22, QStringLiteral("u")));
    QCOMPARE(store.activeId(), first); // auto-selected, and a second add leaves it alone

    QSignalSpy activeSpy(&store, &ServerProfiles::activeIdChanged);

    store.setActiveId(second);
    QCOMPARE(store.activeId(), second);
    QCOMPARE(activeSpy.count(), 1);

    // Re-selecting the same profile is not a change.
    store.setActiveId(second);
    QCOMPARE(activeSpy.count(), 1);

    // An unknown id is ignored outright.
    store.setActiveId(QStringLiteral("00000000-0000-0000-0000-000000000000"));
    QCOMPARE(store.activeId(), second);
    QCOMPARE(activeSpy.count(), 1);

    // The empty string is the one legal non-id: it clears the selection.
    store.setActiveId(QString());
    QVERIFY(store.activeId().isEmpty());
    QCOMPARE(activeSpy.count(), 2);

    // With nothing selected, the next add takes the selection again.
    const QString third = store.addProfile(
        profileFields(QStringLiteral("three"), QStringLiteral("h3"), 22, QStringLiteral("u")));
    QCOMPARE(store.activeId(), third);
    QCOMPARE(activeSpy.count(), 3);

    // ...and the selection is persistent.
    ServerProfiles reopened(path);
    QCOMPARE(reopened.activeId(), third);
}

void TstServerProfiles::removeAdvancesTheActiveSelection()
{
    const QString path = iniPath(QStringLiteral("remove.ini"));
    ServerProfiles store(path);
    const QString a = store.addProfile(
        profileFields(QStringLiteral("a"), QStringLiteral("ha"), 22, QStringLiteral("u")));
    const QString b = store.addProfile(
        profileFields(QStringLiteral("b"), QStringLiteral("hb"), 22, QStringLiteral("u")));
    const QString c = store.addProfile(
        profileFields(QStringLiteral("c"), QStringLiteral("hc"), 22, QStringLiteral("u")));

    QSignalSpy profilesSpy(&store, &ServerProfiles::profilesChanged);
    QSignalSpy activeSpy(&store, &ServerProfiles::activeIdChanged);

    // Removing an unknown id changes nothing at all.
    store.removeProfile(QStringLiteral("not-here"));
    QCOMPARE(store.profiles().size(), 3);
    QCOMPARE(profilesSpy.count(), 0);
    QCOMPARE(activeSpy.count(), 0);

    // Removing a non-active profile leaves the selection alone.
    store.setActiveId(b);
    activeSpy.clear();
    store.removeProfile(a);
    QCOMPARE(idsOf(store.profiles()), QStringList({b, c}));
    QCOMPARE(store.activeId(), b);
    QCOMPARE(profilesSpy.count(), 1);
    QCOMPARE(activeSpy.count(), 0);

    // Removing the active profile advances to whatever took its row.
    store.removeProfile(b);
    QCOMPARE(idsOf(store.profiles()), QStringList({c}));
    QCOMPARE(store.activeId(), c);
    QCOMPARE(activeSpy.count(), 1);

    // Removing the last remaining profile clears the selection.
    store.removeProfile(c);
    QVERIFY(store.profiles().isEmpty());
    QVERIFY(store.activeId().isEmpty());
    QCOMPARE(activeSpy.count(), 2);

    // Removing the active *last* profile falls back to the new last one.
    const QString d = store.addProfile(
        profileFields(QStringLiteral("d"), QStringLiteral("hd"), 22, QStringLiteral("u")));
    const QString e = store.addProfile(
        profileFields(QStringLiteral("e"), QStringLiteral("he"), 22, QStringLiteral("u")));
    store.setActiveId(e);
    store.removeProfile(e);
    QCOMPARE(store.activeId(), d);

    // Removals are durable, not just in-memory.
    ServerProfiles reopened(path);
    QCOMPARE(idsOf(reopened.profiles()), QStringList({d}));
    QCOMPARE(reopened.activeId(), d);
}

void TstServerProfiles::updateMergesFieldsAndRefusesGarbage()
{
    const QString path = iniPath(QStringLiteral("update.ini"));
    ServerProfiles store(path);
    const QString id = store.addProfile(profileFields(QStringLiteral("box"),
                                                      QStringLiteral("old.host"), 22,
                                                      QStringLiteral("olduser"),
                                                      QStringLiteral("/usr/bin/node"),
                                                      QStringLiteral("/srv/repo")));
    QSignalSpy profilesSpy(&store, &ServerProfiles::profilesChanged);

    // A partial map only touches the keys it carries.
    store.updateProfile(id, QVariantMap{{QStringLiteral("host"), QStringLiteral("new.host")},
                                        {QStringLiteral("port"), 2200}});
    QCOMPARE(profilesSpy.count(), 1);
    QVariantMap stored = store.profile(id);
    QCOMPARE(stored.value(QStringLiteral("host")).toString(), QStringLiteral("new.host"));
    QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 2200);
    QCOMPARE(stored.value(QStringLiteral("name")).toString(), QStringLiteral("box"));
    QCOMPARE(stored.value(QStringLiteral("user")).toString(), QStringLiteral("olduser"));
    QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(), QStringLiteral("/usr/bin/node"));
    QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(), QStringLiteral("/srv/repo"));
    QCOMPARE(stored.value(QStringLiteral("id")).toString(), id);

    // An edit that would make the profile unusable is refused outright: the
    // stored profile must survive a bad edit intact.
    const QVariantMap before = store.profile(id);
    store.updateProfile(id, QVariantMap{{QStringLiteral("host"), QStringLiteral("  ")}});
    store.updateProfile(id, QVariantMap{{QStringLiteral("user"), QString()}});
    store.updateProfile(id, QVariantMap{{QStringLiteral("port"), 0}});
    store.updateProfile(id, QVariantMap{{QStringLiteral("port"), 70000}});
    QCOMPARE(store.profile(id), before);
    QCOMPARE(profilesSpy.count(), 1);

    // An unknown id and a no-op edit are both silent.
    store.updateProfile(QStringLiteral("nope"), QVariantMap{{QStringLiteral("host"),
                                                             QStringLiteral("x")}});
    store.updateProfile(id, QVariantMap{{QStringLiteral("host"), QStringLiteral("new.host")}});
    QCOMPARE(profilesSpy.count(), 1);
    QCOMPARE(store.profiles().size(), 1);

    // Updates are durable.
    ServerProfiles reopened(path);
    QCOMPARE(reopened.profile(id), before);
}

void TstServerProfiles::unicodeAndSpacesSurviveTheIniRoundTrip()
{
    const QString path = iniPath(QStringLiteral("unicode.ini"));
    // Every INI metacharacter that could break the writer, plus non-latin text
    // and paths with spaces — all of which a real user will type.
    const QString name = QStringLiteral(u"Ärger — 服务器 🚀 «prod» = [main]; a/b\\c \"q\"");
    const QString host = QStringLiteral("hôte.exemple.fr");
    const QString user = QStringLiteral("user.name-42");
    const QString nodePath = QStringLiteral("/home/user name/.local/bin (x86)/node");
    const QString repoRoot = QStringLiteral(u"/srv/my repo/ünicode dir/");
    const QString identityFile =
        QStringLiteral(u"/home/user name/.ssh/秘密 key");

    QString id;
    {
        ServerProfiles store(path);
        id = store.addProfile(profileFields(name, host, 22222, user, nodePath,
                                            repoRoot, identityFile));
        QVERIFY(!id.isEmpty());
        QCOMPARE(store.profile(id).value(QStringLiteral("name")).toString(), name);
    }

    ServerProfiles reopened(path);
    const QVariantMap stored = reopened.profile(id);
    QCOMPARE(stored.value(QStringLiteral("name")).toString(), name);
    QCOMPARE(stored.value(QStringLiteral("host")).toString(), host);
    QCOMPARE(stored.value(QStringLiteral("user")).toString(), user);
    QCOMPARE(stored.value(QStringLiteral("identityFile")).toString(), identityFile);
    QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(), nodePath);
    QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(), repoRoot);
    QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 22222);
    QCOMPARE(reopened.activeId(), id);

    // Editing such a profile keeps it intact too (the whole group is rewritten
    // on every mutation, so a bad escape would surface here).
    reopened.updateProfile(id, QVariantMap{{QStringLiteral("port"), 2022}});
    ServerProfiles third(path);
    QCOMPARE(third.profile(id).value(QStringLiteral("name")).toString(), name);
    QCOMPARE(third.profile(id).value(QStringLiteral("identityFile")).toString(),
             identityFile);
    QCOMPARE(third.profile(id).value(QStringLiteral("repoRoot")).toString(), repoRoot);
    QCOMPARE(third.profile(id).value(QStringLiteral("port")).toInt(), 2022);
}

void TstServerProfiles::insertionOrderSurvivesReload()
{
    const QString path = iniPath(QStringLiteral("order.ini"));
    QStringList expectedNames;
    QStringList expectedIds;
    {
        ServerProfiles store(path);
        for (int i = 0; i < 8; ++i) {
            const QString name = QStringLiteral("p%1").arg(i);
            const QString id = store.addProfile(profileFields(
                name, QStringLiteral("h%1").arg(i), 22, QStringLiteral("u")));
            QVERIFY(!id.isEmpty());
            expectedNames.append(name);
            expectedIds.append(id);
        }
        QCOMPARE(namesOf(store.profiles()), expectedNames);
    }

    // Reopened, the list is still in insertion order. Ids are random UUIDs, so a
    // store that simply returned QSettings::childGroups() (id-sorted) would
    // shuffle the user's list on every launch.
    ServerProfiles reopened(path);
    QCOMPARE(namesOf(reopened.profiles()), expectedNames);
    QCOMPARE(idsOf(reopened.profiles()), expectedIds);

    // A removal in the middle does not disturb the order of the survivors.
    reopened.removeProfile(expectedIds.at(3));
    expectedNames.removeAt(3);
    expectedIds.removeAt(3);
    QCOMPARE(namesOf(reopened.profiles()), expectedNames);
    ServerProfiles third(path);
    QCOMPARE(namesOf(third.profiles()), expectedNames);
}

void TstServerProfiles::handEditedStoreIsRepairedOnLoad()
{
    const QString path = iniPath(QStringLiteral("handedited.ini"));
    {
        // Written with raw QSettings, exactly the way the documented layout says
        // — no ordinals (an older writer), a nonsense port, and a selection that
        // points at a profile which does not exist.
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue(QStringLiteral("servers/aaa/name"), QStringLiteral("Alpha"));
        raw.setValue(QStringLiteral("servers/aaa/host"), QStringLiteral("alpha.example"));
        raw.setValue(QStringLiteral("servers/aaa/user"), QStringLiteral("u"));
        raw.setValue(QStringLiteral("servers/aaa/port"), QStringLiteral("not-a-port"));
        raw.setValue(QStringLiteral("servers/bbb/name"), QStringLiteral("Beta"));
        raw.setValue(QStringLiteral("servers/bbb/host"), QStringLiteral("beta.example"));
        raw.setValue(QStringLiteral("servers/bbb/user"), QStringLiteral("u"));
        raw.setValue(QStringLiteral("servers/bbb/port"), 2222);
        raw.setValue(QStringLiteral("servers/active"), QStringLiteral("ccc"));
        raw.sync();
    }

    ServerProfiles store(path);
    // Both profiles load (nothing of the user's is thrown away), ordinal-less
    // entries sort deterministically by id...
    QCOMPARE(namesOf(store.profiles()), QStringList({QStringLiteral("Alpha"),
                                                     QStringLiteral("Beta")}));
    // ...an unusable port reads back as the SSH default rather than reaching
    // connectToHost()...
    QCOMPARE(store.profile(QStringLiteral("aaa")).value(QStringLiteral("port")).toInt(), 22);
    QCOMPARE(store.profile(QStringLiteral("bbb")).value(QStringLiteral("port")).toInt(), 2222);
    // ...and the dangling selection is dropped: activeId is empty or existing.
    QVERIFY(store.activeId().isEmpty());

    // The repaired state is written back on the next mutation, ordinals and all.
    store.setActiveId(QStringLiteral("bbb"));
    ServerProfiles reopened(path);
    QCOMPARE(reopened.activeId(), QStringLiteral("bbb"));
    QCOMPARE(namesOf(reopened.profiles()), QStringList({QStringLiteral("Alpha"),
                                                        QStringLiteral("Beta")}));
}

// The store is the one file on disk that says how to REACH a machine, and it
// must never say how to AUTHENTICATE to it. addProfile()/updateProfile() take a
// free-form map straight from QML, and the realistic way a secret gets in is not
// malice but proximity: the connect sheet holds a password or key passphrase in
// the same object as the profile fields, and one careless spread operator later
// it is in the map. Seven fields are whitelisted; everything else is dropped
// before anything is written, so that mistake cannot reach the disk.
void TstServerProfiles::secretsInTheCallersMapNeverReachTheStore()
{
    const QString path = iniPath(QStringLiteral("secrets.ini"));
    const QString password = QStringLiteral("hunter2-must-not-be-stored");
    const QString passphrase = QStringLiteral("keyphrase-must-not-be-stored");
    const QString keyFile = QStringLiteral("/home/u/.ssh/id_ed25519");

    QVariantMap fields = profileFields(
        QStringLiteral("box"), QStringLiteral("h"), 22, QStringLiteral("u"),
        QStringLiteral("/usr/bin/node"), QStringLiteral("/srv/repo"), keyFile);
    fields.insert(QStringLiteral("password"), password);
    fields.insert(QStringLiteral("passphrase"), passphrase);

    QString id;
    {
        ServerProfiles store(path);
        id = store.addProfile(fields);
        QVERIFY(!id.isEmpty());

        // Exactly the whitelist, plus the minted id, and nothing else.
        QCOMPARE(store.profile(id).keys(),
                 QStringList({QStringLiteral("host"), QStringLiteral("id"),
                              QStringLiteral("identityFile"),
                              QStringLiteral("name"), QStringLiteral("nodePath"),
                              QStringLiteral("port"), QStringLiteral("repoRoot"),
                              QStringLiteral("user")}));
        // The key FILE is legitimate profile state; the phrase that unlocks it
        // is not, and the two must not be confused for each other.
        QCOMPARE(store.profile(id).value(QStringLiteral("identityFile")).toString(),
                 keyFile);

        // An edit cannot smuggle one in either, and the edit that carries it
        // still applies its legitimate half.
        store.updateProfile(id,
                            QVariantMap{{QStringLiteral("host"), QStringLiteral("h2")},
                                        {QStringLiteral("password"), password}});
        QCOMPARE(store.profile(id).value(QStringLiteral("host")).toString(),
                 QStringLiteral("h2"));
        QVERIFY(!store.profile(id).contains(QStringLiteral("password")));
    }

    // ...and no trace of either reached the file, neither as a value nor as a
    // key somebody could later start reading.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString ini = QString::fromUtf8(file.readAll());
    QVERIFY2(!ini.contains(password), qPrintable(ini));
    QVERIFY2(!ini.contains(passphrase), qPrintable(ini));
    QVERIFY2(!ini.contains(QStringLiteral("password")), qPrintable(ini));
    QVERIFY2(!ini.contains(QStringLiteral("passphrase")), qPrintable(ini));

    ServerProfiles reopened(path);
    QVERIFY(!reopened.profile(id).contains(QStringLiteral("password")));
    QCOMPARE(reopened.profile(id).value(QStringLiteral("identityFile")).toString(),
             keyFile);
}

// ---------------------------------------------------------------------------
// ConnectSheet.qml
// ---------------------------------------------------------------------------

std::unique_ptr<QQuickView> TstServerProfiles::loadSheet()
{
    takeLoggedWarnings();
    m_engineWarnings.clear();

    auto view = std::make_unique<QQuickView>();
    QObject::connect(view->engine(), &QQmlEngine::warnings, view.get(),
                     [this](const QList<QQmlError>& warnings) {
                         for (const QQmlError& warning : warnings)
                             m_engineWarnings.append(warning.toString());
                     });

    // Inside the CodeHarbor QML module, `Theme` is a registered singleton and
    // every colour and metric in the sheet comes from it. This target loads the
    // sheet by file path, outside that module, where the name would not resolve
    // and every binding that reads it would warn. Register the REAL
    // src/qml/Theme.qml (found next to the sheet, so the two can never drift)
    // and expose its single instance under the same name: the sheet under test
    // is then drawing from the same vocabulary the application ships.
    static const int themeTypeId = qmlRegisterSingletonType(
        QUrl::fromLocalFile(
            QFileInfo(QStringLiteral(CH_CONNECTSHEET_QML))
                .absoluteDir()
                .filePath(QStringLiteral("Theme.qml"))),
        "CodeHarborThemeForTest", 1, 0, "Theme");
    QObject* const theme =
        view->engine()->singletonInstance<QObject*>(themeTypeId);
    if (!theme) {
        qWarning("ConnectSheet.qml: src/qml/Theme.qml could not be instantiated");
        return nullptr;
    }
    view->engine()->rootContext()->setContextProperty(QStringLiteral("Theme"),
                                                      theme);

    view->setSource(QUrl::fromLocalFile(QStringLiteral(CH_CONNECTSHEET_QML)));
    if (view->status() != QQuickView::Ready) {
        QStringList errors;
        for (const QQmlError& error : view->errors())
            errors.append(error.toString());
        qWarning("ConnectSheet.qml failed to load: %s", qPrintable(errors.join(QLatin1Char('\n'))));
        return nullptr;
    }
    view->show();
    if (!QTest::qWaitForWindowExposed(view.get())) {
        qWarning("ConnectSheet.qml view was never exposed");
        return nullptr;
    }
    return view;
}

#define CH_LOAD_SHEET(view, root)                                                                  \
    const std::unique_ptr<QQuickView> view = loadSheet();                                          \
    QVERIFY(view != nullptr);                                                                      \
    QQuickItem* const root = view->rootObject();                                                   \
    QVERIFY(root != nullptr)

// Every warning the sheet produced so far must be none.
#define CH_ASSERT_SILENT()                                                                         \
    do {                                                                                           \
        QCoreApplication::processEvents();                                                         \
        const QStringList logged = takeLoggedWarnings();                                           \
        QVERIFY2(logged.isEmpty(), qPrintable(logged.join(QLatin1Char('\n'))));                    \
        QVERIFY2(m_engineWarnings.isEmpty(), qPrintable(m_engineWarnings.join(QLatin1Char('\n'))));\
    } while (false)

void TstServerProfiles::sheetLoadsSilentlyAndExposesItsApi()
{
    CH_LOAD_SHEET(view, root);

    // The exact component API the orchestrator binds against.
    const QMetaObject* mo = root->metaObject();
    const QList<QPair<QByteArray, int>> expectedProperties = {
        {QByteArrayLiteral("profiles"), QMetaType::QVariant},
        {QByteArrayLiteral("activeId"), QMetaType::QString},
        {QByteArrayLiteral("connectionState"), QMetaType::QString},
        {QByteArrayLiteral("errorText"), QMetaType::QString},
        {QByteArrayLiteral("pendingHostKey"), QMetaType::QVariant},
        {QByteArrayLiteral("pendingCredential"), QMetaType::QVariant},
    };
    for (const auto& expected : expectedProperties) {
        const int index = mo->indexOfProperty(expected.first.constData());
        QVERIFY2(index >= 0, expected.first.constData());
        QCOMPARE(mo->property(index).metaType().id(), expected.second);
    }

    const QByteArrayList expectedSignals = {
        QByteArrayLiteral("connectRequested(QString)"),
        QByteArrayLiteral("profileSaved(QVariant)"),
        QByteArrayLiteral("profileRemoved(QString)"),
        QByteArrayLiteral("hostKeyDecision(bool)"),
        QByteArrayLiteral("credentialSubmitted(QString,QString)"),
        QByteArrayLiteral("dismissed()"),
    };
    for (const QByteArray& signature : expectedSignals) {
        const int index = mo->indexOfSignal(signature.constData());
        QVERIFY2(index >= 0, signature.constData());
    }

    // Defaults are the "fresh config" state: no servers, nothing pending, and
    // the empty-list hint visible so the user knows what to do.
    QCOMPARE(root->property("activeId").toString(), QString());
    QVERIFY(root->property("pendingHostKey").isNull()
            || !root->property("pendingHostKey").toBool());
    QVERIFY(root->findChild<QObject*>(QStringLiteral("emptyHint"))->property("visible").toBool());
    QVERIFY(!root->findChild<QObject*>(QStringLiteral("hostKeyPrompt"))->property("visible").toBool());
    QVERIFY(!root->findChild<QObject*>(QStringLiteral("credentialPrompt"))->property("visible").toBool());
    QVERIFY(!root->findChild<QObject*>(QStringLiteral("errorBanner"))->property("visible").toBool());
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("stateLabel")), "text"),
             QStringLiteral("disconnected"));
    // Nothing can be connected to or removed before a profile exists.
    QVERIFY(!root->findChild<QObject*>(QStringLiteral("connectButton"))->property("enabled").toBool());
    QVERIFY(!root->findChild<QObject*>(QStringLiteral("removeButton"))->property("enabled").toBool());
    QVERIFY(!root->findChild<QObject*>(QStringLiteral("saveButton"))->property("enabled").toBool());

    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetListsProfilesAndMarksTheActiveOne()
{
    CH_LOAD_SHEET(view, root);

    const QVariantList profiles = {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("id-a")},
                    {QStringLiteral("name"), QStringLiteral("Alpha")},
                    {QStringLiteral("host"), QStringLiteral("alpha.example")},
                    {QStringLiteral("port"), 22},
                    {QStringLiteral("user"), QStringLiteral("ua")},
                    {QStringLiteral("nodePath"), QStringLiteral("/usr/bin/node")},
                    {QStringLiteral("repoRoot"), QStringLiteral("/srv/a")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("id-b")},
                    {QStringLiteral("name"), QStringLiteral(u"Bêta 服务器")},
                    {QStringLiteral("host"), QStringLiteral("beta.example")},
                    {QStringLiteral("port"), 2222},
                    {QStringLiteral("user"), QStringLiteral("ub")},
                    {QStringLiteral("nodePath"), QStringLiteral("/opt/node bin/node")},
                    {QStringLiteral("repoRoot"), QStringLiteral("/srv/b")}},
    };
    root->setProperty("profiles", profiles);
    root->setProperty("activeId", QStringLiteral("id-b"));

    // Two rows, the active one badged, and the form follows the active profile.
    QTRY_VERIFY(findByName(root, QStringLiteral("profileRow1")) != nullptr);
    QVERIFY(!findByName(root, QStringLiteral("emptyHint"))->property("visible").toBool());
    QVERIFY(!findByName(root, QStringLiteral("activeBadge0"))->property("visible").toBool());
    QVERIFY(findByName(root, QStringLiteral("activeBadge1"))->property("visible").toBool());

    QCOMPARE(root->property("editingId").toString(), QStringLiteral("id-b"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("nameField")), "text"),
             QStringLiteral(u"Bêta 服务器"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("hostField")), "text"),
             QStringLiteral("beta.example"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("portField")), "text"),
             QStringLiteral("2222"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("userField")), "text"),
             QStringLiteral("ub"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("nodePathField")), "text"),
             QStringLiteral("/opt/node bin/node"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("repoRootField")), "text"),
             QStringLiteral("/srv/b"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("formTitle")), "text"),
             QStringLiteral("Edit server"));

    // Clicking the other row moves the form to it without touching activeId.
    QMetaObject::invokeMethod(findByName(root, QStringLiteral("profileRow0")), "clicked");
    QCOMPARE(root->property("editingId").toString(), QStringLiteral("id-a"));
    QCOMPARE(root->property("activeId").toString(), QStringLiteral("id-b"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("hostField")), "text"),
             QStringLiteral("alpha.example"));

    // The connection state is surfaced verbatim.
    root->setProperty("connectionState", QStringLiteral("Authenticating"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("stateLabel")), "text"),
             QStringLiteral("Authenticating"));

    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetEmitsConnectAndRemoveForTheSelection()
{
    CH_LOAD_SHEET(view, root);

    const QVariantList profiles = {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("id-a")},
                    {QStringLiteral("name"), QStringLiteral("Alpha")},
                    {QStringLiteral("host"), QStringLiteral("alpha.example")},
                    {QStringLiteral("port"), 22},
                    {QStringLiteral("user"), QStringLiteral("ua")}},
    };
    root->setProperty("profiles", profiles);

    QSignalSpy connectSpy(root, SIGNAL(connectRequested(QString)));
    QSignalSpy removeSpy(root, SIGNAL(profileRemoved(QString)));
    QSignalSpy dismissSpy(root, SIGNAL(dismissed()));

    QObject* const connectButton = root->findChild<QObject*>(QStringLiteral("connectButton"));
    QVERIFY(connectButton->property("enabled").toBool());
    QMetaObject::invokeMethod(connectButton, "clicked");
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(connectSpy.at(0).at(0).toString(), QStringLiteral("id-a"));

    QMetaObject::invokeMethod(root->findChild<QObject*>(QStringLiteral("removeButton")), "clicked");
    QCOMPARE(removeSpy.count(), 1);
    QCOMPARE(removeSpy.at(0).at(0).toString(), QStringLiteral("id-a"));

    // The sheet never removes anything itself: the host owns the store.
    QCOMPARE(root->property("profiles").toList().size(), 1);

    QMetaObject::invokeMethod(root->findChild<QObject*>(QStringLiteral("cancelButton")), "clicked");
    QMetaObject::invokeMethod(root->findChild<QObject*>(QStringLiteral("closeButton")), "clicked");
    QCOMPARE(dismissSpy.count(), 2);

    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetSavesANewProfileThenEditsIt()
{
    CH_LOAD_SHEET(view, root);
    QSignalSpy savedSpy(root, SIGNAL(profileSaved(QVariant)));

    QObject* const nameField = root->findChild<QObject*>(QStringLiteral("nameField"));
    QObject* const hostField = root->findChild<QObject*>(QStringLiteral("hostField"));
    QObject* const portField = root->findChild<QObject*>(QStringLiteral("portField"));
    QObject* const userField = root->findChild<QObject*>(QStringLiteral("userField"));
    QObject* const identityField =
        root->findChild<QObject*>(QStringLiteral("identityFileField"));
    QObject* const nodeField = root->findChild<QObject*>(QStringLiteral("nodePathField"));
    QObject* const repoField = root->findChild<QObject*>(QStringLiteral("repoRootField"));
    QObject* const saveButton = root->findChild<QObject*>(QStringLiteral("saveButton"));

    // A fresh sheet starts on the new-profile form, defaulted to port 22.
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("formTitle")), "text"),
             QStringLiteral("New server"));
    QCOMPARE(stringOf(portField, "text"), QStringLiteral("22"));
    QVERIFY(!saveButton->property("enabled").toBool());

    // Host alone is not enough; the sheet refuses to submit an unusable profile
    // for the same reason the store refuses to keep one.
    hostField->setProperty("text", QStringLiteral("box.local"));
    QVERIFY(!saveButton->property("enabled").toBool());
    userField->setProperty("text", QStringLiteral("yichen"));
    QVERIFY(saveButton->property("enabled").toBool());
    portField->setProperty("text", QStringLiteral("70000"));
    QVERIFY(!saveButton->property("enabled").toBool());
    QVERIFY(root->findChild<QObject*>(QStringLiteral("validationHint"))->property("visible").toBool());
    portField->setProperty("text", QStringLiteral("2222"));
    QVERIFY(saveButton->property("enabled").toBool());

    nameField->setProperty("text", QStringLiteral("  Prod box  "));
    identityField->setProperty("text", QStringLiteral("~/.ssh/prod key"));
    nodeField->setProperty("text", QStringLiteral("/home/user name/.local/bin/node"));
    repoField->setProperty("text", QStringLiteral("/srv/my repo"));

    QMetaObject::invokeMethod(saveButton, "clicked");
    QCOMPARE(savedSpy.count(), 1);
    QVariantMap fields = asMap(savedSpy.at(0).at(0));
    QCOMPARE(fields.value(QStringLiteral("id")).toString(), QString()); // "" == create
    QCOMPARE(fields.value(QStringLiteral("name")).toString(), QStringLiteral("Prod box"));
    QCOMPARE(fields.value(QStringLiteral("host")).toString(), QStringLiteral("box.local"));
    QCOMPARE(fields.value(QStringLiteral("port")).toInt(), 2222);
    QCOMPARE(fields.value(QStringLiteral("user")).toString(), QStringLiteral("yichen"));
    QCOMPARE(fields.value(QStringLiteral("identityFile")).toString(),
             QStringLiteral("~/.ssh/prod key"));
    QCOMPARE(fields.value(QStringLiteral("nodePath")).toString(),
             QStringLiteral("/home/user name/.local/bin/node"));
    QCOMPARE(fields.value(QStringLiteral("repoRoot")).toString(), QStringLiteral("/srv/my repo"));

    // The host stores it and hands the list back: the sheet must land on the new
    // profile so the very next click can Connect (the cold-start path).
    const QVariantList stored = {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("new-id")},
                    {QStringLiteral("name"), QStringLiteral("Prod box")},
                    {QStringLiteral("host"), QStringLiteral("box.local")},
                    {QStringLiteral("port"), 2222},
                    {QStringLiteral("user"), QStringLiteral("yichen")},
                    {QStringLiteral("identityFile"), QStringLiteral("~/.ssh/prod key")},
                    {QStringLiteral("nodePath"), QStringLiteral("/home/user name/.local/bin/node")},
                    {QStringLiteral("repoRoot"), QStringLiteral("/srv/my repo")}},
    };
    root->setProperty("profiles", stored);
    QCOMPARE(root->property("editingId").toString(), QStringLiteral("new-id"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("formTitle")), "text"),
             QStringLiteral("Edit server"));
    QVERIFY(root->findChild<QObject*>(QStringLiteral("connectButton"))->property("enabled").toBool());
    // ...and the keyboard lands on the list, where Enter is Connect.
    QVERIFY(qobject_cast<QQuickItem*>(findByName(root, QStringLiteral("profileList")))
                ->hasActiveFocus());

    // Editing now submits an update carrying that id.
    hostField->setProperty("text", QStringLiteral("box2.local"));
    QMetaObject::invokeMethod(saveButton, "clicked");
    QCOMPARE(savedSpy.count(), 2);
    fields = asMap(savedSpy.at(1).at(0));
    QCOMPARE(fields.value(QStringLiteral("id")).toString(), QStringLiteral("new-id"));
    QCOMPARE(fields.value(QStringLiteral("host")).toString(), QStringLiteral("box2.local"));

    // "Add" returns to a blank new-profile form without disturbing the store.
    QMetaObject::invokeMethod(root->findChild<QObject*>(QStringLiteral("addButton")), "clicked");
    QCOMPARE(root->property("editingId").toString(), QString());
    QCOMPARE(stringOf(hostField, "text"), QString());
    QCOMPARE(stringOf(portField, "text"), QStringLiteral("22"));
    QVERIFY(!root->findChild<QObject*>(QStringLiteral("connectButton"))->property("enabled").toBool());
    // Focus moves into the form so the user can just start typing.
    QVERIFY(qobject_cast<QQuickItem*>(findByName(root, QStringLiteral("nameField")))
                ->property("input").value<QQuickItem*>()->hasActiveFocus());

    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetHostKeyPromptDecidesBothWays()
{
    CH_LOAD_SHEET(view, root);
    QSignalSpy decisionSpy(root, SIGNAL(hostKeyDecision(bool)));

    QObject* const prompt = root->findChild<QObject*>(QStringLiteral("hostKeyPrompt"));
    QVERIFY(!prompt->property("visible").toBool());

    const QVariantMap pending{
        {QStringLiteral("host"), QStringLiteral("box.local")},
        {QStringLiteral("keyType"), QStringLiteral("ssh-ed25519")},
        {QStringLiteral("fingerprint"),
         QStringLiteral("SHA256:6dGRJ0mCkQeAxQ0nQ0mm2b3xk0e3iF0jnq0oO3pP1qQ")}};
    root->setProperty("pendingHostKey", pending);

    QVERIFY(prompt->property("visible").toBool());
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("hostKeyFingerprint")), "text"),
             pending.value(QStringLiteral("fingerprint")).toString());
    const QString hostLine = stringOf(root->findChild<QObject*>(QStringLiteral("hostKeyHost")),
                                      "text");
    QVERIFY2(hostLine.contains(QStringLiteral("box.local")), qPrintable(hostLine));
    QVERIFY2(hostLine.contains(QStringLiteral("ssh-ed25519")), qPrintable(hostLine));

    QMetaObject::invokeMethod(root->findChild<QObject*>(QStringLiteral("hostKeyAcceptButton")),
                              "clicked");
    QCOMPARE(decisionSpy.count(), 1);
    QCOMPARE(decisionSpy.at(0).at(0).toBool(), true);

    QMetaObject::invokeMethod(root->findChild<QObject*>(QStringLiteral("hostKeyRejectButton")),
                              "clicked");
    QCOMPARE(decisionSpy.count(), 2);
    QCOMPARE(decisionSpy.at(1).at(0).toBool(), false);

    // Escape is a rejection, never an accidental accept.
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(root, &escape);
    QCOMPARE(decisionSpy.count(), 3);
    QCOMPARE(decisionSpy.at(2).at(0).toBool(), false);
    QCOMPARE(QSignalSpy(root, SIGNAL(dismissed())).count(), 0);

    // Clearing the pending key puts the sheet back.
    root->setProperty("pendingHostKey", QVariant::fromValue(nullptr));
    QVERIFY(!prompt->property("visible").toBool());

    CH_ASSERT_SILENT();
}

// The masked secret field. SshConnectionPool's third auth rung asks for a
// password or key passphrase, and until this existed the sheet collected no
// secret at all, so the rung was unreachable from the product.
//
// What is gated here is not the pixels but the two properties that make it safe
// to type into: the field is MASKED, and the secret leaves only through
// credentialSubmitted() — never through profileSaved(), which is the one signal
// that reaches ServerProfiles and therefore the ini file on disk.
void TstServerProfiles::sheetCredentialPromptMasksSubmitsAndKeepsTheSecretOffDisk()
{
    CH_LOAD_SHEET(view, root);
    const QString secret = QStringLiteral("correct-horse-battery-42");

    // A REAL store, fed exactly the way Main.qml feeds it: whatever the sheet
    // emits on profileSaved() is what reaches ServerProfiles, and therefore the
    // ini file on disk.
    const QString ini = iniPath(QStringLiteral("credential.ini"));
    ServerProfiles store(ini);

    QSignalSpy submitSpy(root, SIGNAL(credentialSubmitted(QString,QString)));
    QSignalSpy savedSpy(root, SIGNAL(profileSaved(QVariant)));
    QSignalSpy dismissSpy(root, SIGNAL(dismissed()));
    QObject* const prompt = root->findChild<QObject*>(QStringLiteral("credentialPrompt"));
    QVERIFY(prompt);
    QVERIFY(!prompt->property("visible").toBool());

    root->setProperty("pendingCredential",
                      QVariantMap{{QStringLiteral("user"), QStringLiteral("yichen")},
                                  {QStringLiteral("host"), QStringLiteral("box.local")},
                                  {QStringLiteral("prompt"), QStringLiteral("Password")},
                                  {QStringLiteral("kind"), QStringLiteral("password")}});
    QVERIFY(prompt->property("visible").toBool());

    const QString target =
        stringOf(root->findChild<QObject*>(QStringLiteral("credentialTarget")), "text");
    QVERIFY2(target.contains(QStringLiteral("yichen")), qPrintable(target));
    QVERIFY2(target.contains(QStringLiteral("box.local")), qPrintable(target));
    // A server with `AuthenticationMethods publickey,password` asks for the
    // password AFTER accepting the key, so a password request must not tell the
    // user their key could not authenticate: that sends them off debugging a key
    // that is working perfectly.
    QVERIFY2(!target.contains(QStringLiteral("could not authenticate")),
             qPrintable(target));
    QVERIFY2(target.contains(QStringLiteral("password")), qPrintable(target));

    QObject* const field = root->findChild<QObject*>(QStringLiteral("credentialField"));
    QVERIFY(field);
    // MASKED. TextInput.Password == 2; a plain-text password box would be the
    // whole point of this field missed.
    QCOMPARE(field->property("echoMode").toInt(), 2);
    // Nothing can be submitted until something is typed.
    QVERIFY(!root->findChild<QObject*>(QStringLiteral("credentialSubmitButton"))
                 ->property("enabled").toBool());

    field->setProperty("text", secret);
    QMetaObject::invokeMethod(
        root->findChild<QObject*>(QStringLiteral("credentialSubmitButton")), "clicked");

    QCOMPARE(submitSpy.count(), 1);
    QCOMPARE(submitSpy.at(0).at(0).toString(), secret);
    QCOMPARE(submitSpy.at(0).at(1).toString(), QStringLiteral("password"));
    // Wiped from the field in the same turn it was handed up, so it is not left
    // living in a QML item (and its undo stack) after being spent.
    QCOMPARE(stringOf(field, "text"), QString());

    // When the pool asks for a key passphrase, users may explicitly choose
    // password auth; the field stays masked and the selection is carried in the
    // second signal argument rather than inferred from its contents.
    root->setProperty("pendingCredential",
                      QVariantMap{{QStringLiteral("user"), QStringLiteral("yichen")},
                                  {QStringLiteral("host"), QStringLiteral("box.local")},
                                  {QStringLiteral("kind"),
                                   QStringLiteral("keyPassphrase")}});
    field->setProperty("text", secret);
    QMetaObject::invokeMethod(
        root->findChild<QObject*>(QStringLiteral("credentialPasswordButton")), "clicked");
    QCOMPARE(submitSpy.count(), 2);
    QCOMPARE(submitSpy.at(1).at(0).toString(), secret);
    QCOMPARE(submitSpy.at(1).at(1).toString(), QStringLiteral("password"));

    // Cancel is an empty answer, and Escape is a cancel — never an accidental
    // submit of whatever happens to be typed.
    field->setProperty("text", secret);
    QMetaObject::invokeMethod(
        root->findChild<QObject*>(QStringLiteral("credentialCancelButton")), "clicked");
    QCOMPARE(submitSpy.count(), 3);
    QCOMPARE(submitSpy.at(2).at(0).toString(), QString());
    QCOMPARE(submitSpy.at(2).at(1).toString(), QStringLiteral("keyPassphrase"));
    QCOMPARE(stringOf(field, "text"), QString());

    field->setProperty("text", secret);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(root, &escape);
    QCOMPARE(submitSpy.count(), 4);
    QCOMPARE(submitSpy.at(3).at(0).toString(), QString());
    QCOMPARE(submitSpy.at(3).at(1).toString(), QStringLiteral("keyPassphrase"));
    QCOMPARE(dismissSpy.count(), 0);

    // Clearing the prompt puts the sheet back and leaves nothing behind.
    root->setProperty("pendingCredential", QVariant::fromValue(nullptr));
    QVERIFY(!prompt->property("visible").toBool());
    QCOMPARE(stringOf(field, "text"), QString());

    // Now save a profile through the very path that DOES persist, and prove the
    // secret is nowhere in the resulting store. A passphrase in the config file
    // would be a worse defect than the missing prompt this fixes.
    root->findChild<QObject*>(QStringLiteral("hostField"))
        ->setProperty("text", QStringLiteral("box.local"));
    root->findChild<QObject*>(QStringLiteral("userField"))
        ->setProperty("text", QStringLiteral("yichen"));
    QMetaObject::invokeMethod(
        root->findChild<QObject*>(QStringLiteral("saveButton")), "clicked");
    QCOMPARE(savedSpy.count(), 1);

    // Whatever the sheet chose to publish is ALL that can ever be persisted.
    const QVariantMap fields = savedSpy.at(0).at(0).toMap();
    for (const QVariant& value : fields)
        QVERIFY2(value.toString() != secret, qPrintable(fields.key(value)));
    QVERIFY(!store.addProfile(fields).isEmpty());
    QCOMPARE(store.profiles().size(), 1);

    QFile file(ini);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray onDisk = file.readAll();
    QVERIFY(!onDisk.isEmpty());
    QVERIFY2(!onDisk.contains(secret.toUtf8()), onDisk.constData());

    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetSurfacesErrorTextWithoutBlocking()
{
    CH_LOAD_SHEET(view, root);

    const QVariantList profiles = {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("id-a")},
                    {QStringLiteral("name"), QStringLiteral("Alpha")},
                    {QStringLiteral("host"), QStringLiteral("alpha.example")},
                    {QStringLiteral("port"), 22},
                    {QStringLiteral("user"), QStringLiteral("ua")}},
    };
    root->setProperty("profiles", profiles);

    QObject* const banner = root->findChild<QObject*>(QStringLiteral("errorBanner"));
    QVERIFY(!banner->property("visible").toBool());

    const QString message = QStringLiteral("ssh: connect to host alpha.example port 22: refused");
    root->setProperty("errorText", message);
    QVERIFY(banner->property("visible").toBool());
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("errorLabel")), "text"), message);
    root->setProperty("connectionState", QStringLiteral("error"));

    // Non-blocking: the sheet stays fully operable while the error is shown.
    QSignalSpy connectSpy(root, SIGNAL(connectRequested(QString)));
    QVERIFY(root->findChild<QObject*>(QStringLiteral("connectButton"))->property("enabled").toBool());
    QMetaObject::invokeMethod(root->findChild<QObject*>(QStringLiteral("connectButton")), "clicked");
    QCOMPARE(connectSpy.count(), 1);

    root->setProperty("errorText", QString());
    QVERIFY(!banner->property("visible").toBool());

    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetOpensSshDiagnostics()
{
    CH_LOAD_SHEET(view, root);
    const QString diagnostic = QStringLiteral(
        "Starting SSH connection to alpha.example:22.\n"
        "libssh[4] ssh_set_client_kex: kex algorithms: curve25519-sha256");
    root->setProperty("errorText", QStringLiteral("SSH connection failed."));
    root->setProperty("diagnosticText", diagnostic);

    QObject* const details = root->findChild<QObject*>(QStringLiteral("sshDetailsButton"));
    QObject* const dialog = root->findChild<QObject*>(QStringLiteral("sshDiagnosticsDialog"));
    QObject* const text = root->findChild<QObject*>(QStringLiteral("sshDiagnosticsText"));
    QVERIFY(details && dialog && text);
    QVERIFY(details->property("visible").toBool());
    QCOMPARE(stringOf(text, "text"), diagnostic);

    QMetaObject::invokeMethod(details, "clicked");
    QVERIFY(dialog->property("visible").toBool());
    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetIsUsableFromTheKeyboardAlone()
{
    CH_LOAD_SHEET(view, root);

    const QVariantList profiles = {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("id-a")},
                    {QStringLiteral("name"), QStringLiteral("Alpha")},
                    {QStringLiteral("host"), QStringLiteral("alpha.example")},
                    {QStringLiteral("port"), 22},
                    {QStringLiteral("user"), QStringLiteral("ua")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("id-b")},
                    {QStringLiteral("name"), QStringLiteral("Beta")},
                    {QStringLiteral("host"), QStringLiteral("beta.example")},
                    {QStringLiteral("port"), 22},
                    {QStringLiteral("user"), QStringLiteral("ub")}},
    };
    root->setProperty("profiles", profiles);
    QTRY_VERIFY(findByName(root, QStringLiteral("profileRow1")) != nullptr);

    QQuickItem* const list =
        root->findChild<QQuickItem*>(QStringLiteral("profileList"));
    QVERIFY(list != nullptr);
    // Gaining a selection hands focus to the list, so arrow keys and Enter work
    // without ever touching the mouse.
    QVERIFY(list->hasActiveFocus());
    QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
    QCOMPARE(root->property("editingId").toString(), QStringLiteral("id-a"));

    // Down moves the selection and the form follows it.
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QCoreApplication::sendEvent(list, &down);
    QCOMPARE(list->property("currentIndex").toInt(), 1);
    QCOMPARE(root->property("editingId").toString(), QStringLiteral("id-b"));
    QCOMPARE(stringOf(root->findChild<QObject*>(QStringLiteral("hostField")), "text"),
             QStringLiteral("beta.example"));

    // Enter on the list connects to the highlighted profile.
    QSignalSpy connectSpy(root, SIGNAL(connectRequested(QString)));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(list, &enter);
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(connectSpy.at(0).at(0).toString(), QStringLiteral("id-b"));

    // Enter in a form field saves.
    QSignalSpy savedSpy(root, SIGNAL(profileSaved(QVariant)));
    QQuickItem* const hostField =
        root->findChild<QQuickItem*>(QStringLiteral("hostField"))->property("input")
            .value<QQuickItem*>();
    QVERIFY(hostField != nullptr);
    QKeyEvent fieldEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(hostField, &fieldEnter);
    QCOMPARE(savedSpy.count(), 1);
    QCOMPARE(asMap(savedSpy.at(0).at(0)).value(QStringLiteral("id")).toString(),
             QStringLiteral("id-b"));

    // Escape closes the sheet.
    QSignalSpy dismissSpy(root, SIGNAL(dismissed()));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(root, &escape);
    QCOMPARE(dismissSpy.count(), 1);

    // Every interactive control is reachable by Tab.
    const QStringList tabbable = {
        QStringLiteral("addButton"),  QStringLiteral("removeButton"),
        QStringLiteral("saveButton"), QStringLiteral("connectButton"),
        QStringLiteral("cancelButton"), QStringLiteral("closeButton"),
    };
    for (const QString& name : tabbable) {
        QObject* const control = root->findChild<QObject*>(name);
        QVERIFY2(control != nullptr, qPrintable(name));
        QVERIFY2(control->property("activeFocusOnTab").toBool(), qPrintable(name));
    }
    for (const QString& name : {QStringLiteral("nameField"), QStringLiteral("hostField"),
                                QStringLiteral("portField"), QStringLiteral("userField"),
                                QStringLiteral("nodePathField"), QStringLiteral("repoRootField")}) {
        QQuickItem* const input =
            root->findChild<QQuickItem*>(name)->property("input").value<QQuickItem*>();
        QVERIFY2(input != nullptr, qPrintable(name));
        QVERIFY2(input->property("activeFocusOnTab").toBool(), qPrintable(name));
    }

    CH_ASSERT_SILENT();
}

// The half of the cold-start walkthrough this slice owns: a user with a fresh
// config types a server into the sheet, it is stored, it comes back selected and
// connectable, Connect asks the host to connect to exactly that profile, and a
// relaunch still finds it. Nothing here injects an environment variable.
void TstServerProfiles::coldStartAddsAServerThenFindsItAgainAfterRelaunch()
{
    const QString path = iniPath(QStringLiteral("coldstart.ini"));
    QString storedId;

    {
        ServerProfiles store(path);
        QVERIFY(store.profiles().isEmpty()); // fresh config: nothing to connect to

        CH_LOAD_SHEET(view, root);
        SheetBridge bridge(&store, root);

        // Type the server in. Every field the SSH spine needs, by keyboard.
        findByName(root, QStringLiteral("nameField"))
            ->setProperty("text", QStringLiteral("Fixture"));
        findByName(root, QStringLiteral("hostField"))
            ->setProperty("text", QStringLiteral("127.0.0.1"));
        findByName(root, QStringLiteral("portField"))
            ->setProperty("text", QStringLiteral("2222"));
        findByName(root, QStringLiteral("userField"))
            ->setProperty("text", QStringLiteral("yichen"));
        findByName(root, QStringLiteral("nodePathField"))
            ->setProperty("text", QStringLiteral("/home/yichen/.local/bin/node"));
        findByName(root, QStringLiteral("repoRootField"))
            ->setProperty("text", QStringLiteral("/home/yichen/projects/codeharbor"));

        QMetaObject::invokeMethod(findByName(root, QStringLiteral("saveButton")), "clicked");
        storedId = bridge.lastSavedId;
        QVERIFY(!storedId.isEmpty());

        // The store kept exactly what was typed...
        const QVariantMap stored = store.profile(storedId);
        QCOMPARE(stored.value(QStringLiteral("host")).toString(), QStringLiteral("127.0.0.1"));
        QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 2222);
        QCOMPARE(stored.value(QStringLiteral("user")).toString(), QStringLiteral("yichen"));
        QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(),
                 QStringLiteral("/home/yichen/.local/bin/node"));
        QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(),
                 QStringLiteral("/home/yichen/projects/codeharbor"));
        QCOMPARE(store.activeId(), storedId);

        // ...and the sheet came back showing it, selected and connectable.
        QTRY_VERIFY(findByName(root, QStringLiteral("profileRow0")) != nullptr);
        QCOMPARE(root->property("editingId").toString(), storedId);
        QVERIFY(findByName(root, QStringLiteral("activeBadge0"))->property("visible").toBool());
        QObject* const connectButton = findByName(root, QStringLiteral("connectButton"));
        QVERIFY(connectButton->property("enabled").toBool());

        QMetaObject::invokeMethod(connectButton, "clicked");
        QCOMPARE(bridge.connectRequests, QStringList({storedId}));

        CH_ASSERT_SILENT();
    }

    // Relaunch: a brand-new store and a brand-new sheet over the same config.
    ServerProfiles reopened(path);
    CH_LOAD_SHEET(view, root);
    SheetBridge bridge(&reopened, root);

    QTRY_VERIFY(findByName(root, QStringLiteral("profileRow0")) != nullptr);
    QCOMPARE(root->property("editingId").toString(), storedId);
    QCOMPARE(stringOf(findByName(root, QStringLiteral("hostField")), "text"),
             QStringLiteral("127.0.0.1"));
    QCOMPARE(stringOf(findByName(root, QStringLiteral("portField")), "text"),
             QStringLiteral("2222"));
    QVERIFY(findByName(root, QStringLiteral("activeBadge0"))->property("visible").toBool());
    QVERIFY(findByName(root, QStringLiteral("connectButton"))->property("enabled").toBool());
    QVERIFY(!findByName(root, QStringLiteral("emptyHint"))->property("visible").toBool());

    // Removing it through the sheet empties the store and resets the form.
    QMetaObject::invokeMethod(findByName(root, QStringLiteral("removeButton")), "clicked");
    QVERIFY(reopened.profiles().isEmpty());
    QVERIFY(reopened.activeId().isEmpty());
    QCOMPARE(root->property("editingId").toString(), QString());
    QVERIFY(findByName(root, QStringLiteral("emptyHint"))->property("visible").toBool());
    QVERIFY(!findByName(root, QStringLiteral("connectButton"))->property("enabled").toBool());

    ServerProfiles afterRemoval(path);
    QVERIFY(afterRemoval.profiles().isEmpty());

    CH_ASSERT_SILENT();
}

QTEST_MAIN(TstServerProfiles)
#include "tst_serverprofiles.moc"
