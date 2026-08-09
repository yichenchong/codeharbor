// Connection profiles (ch::ServerProfiles) and the QML surfaces that display
// and edit them.
//
// The store half remains a direct CRUD/persistence test. The UI half loads the
// shipped SettingsWindow.qml server pane with a small AppController-shaped
// fixture, while the connector-only host-key and credential prompts continue
// to exercise ConnectSheet.qml (the component that still owns those prompts).
// Every QML warning from either source-loaded component is collected so a
// binding regression cannot poison the test run.

#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QLockFile>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickView>
#include <QSettings>
#include <QSignalSpy>
#include <QStringList>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
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
// Runtime binding warnings are logged with the component URL, not always
// through QQmlEngine::warnings(), so collect both source-loaded surfaces.
void warningCollector(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    if (type != QtDebugMsg && type != QtInfoMsg
        && (msg.contains(QLatin1String("ConnectSheet.qml"))
            || msg.contains(QLatin1String("SettingsWindow.qml")))) {
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

// A SECOND WRITER: another copy of the application, or the user's own text
// editor, putting a profile into the same ini file while a ServerProfiles
// instance is alive and knows nothing about it. Raw QSettings, exactly the
// documented layout, so nothing about the store's own writer is assumed.
// `extra` adds keys outside the seven-field whitelist, which a hand edit can.
void writeProfileBehindOurBack(const QString& path, const QString& id,
                               const QString& name, int ordinal,
                               const QVariantMap& extra = QVariantMap())
{
    QSettings raw(path, QSettings::IniFormat);
    const QString prefix = QStringLiteral("servers/") + id + QLatin1Char('/');
    raw.setValue(prefix + QStringLiteral("name"), name);
    raw.setValue(prefix + QStringLiteral("host"), name + QStringLiteral(".example"));
    raw.setValue(prefix + QStringLiteral("port"), 2200);
    raw.setValue(prefix + QStringLiteral("user"), QStringLiteral("otheruser"));
    raw.setValue(prefix + QStringLiteral("ordinal"), ordinal);
    for (auto it = extra.cbegin(); it != extra.cend(); ++it)
        raw.setValue(prefix + it.key(), it.value());
    raw.sync();
}

// The permission bits nobody but the owner may have on the profile store.
constexpr QFile::Permissions kForbiddenModeBits =
    QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther
    | QFile::WriteOther | QFile::ExeOther;

bool privatePermissionsHold(const QString& path)
{
#ifdef Q_OS_WIN
    // Windows uses ACLs rather than POSIX owner/group/other mode bits; Qt's
    // QFile::Permissions view cannot prove the POSIX invariant there.
    Q_UNUSED(path);
    return true;
#else
    return !(QFile::permissions(path) & kForbiddenModeBits);
#endif
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
// SettingsWindow owns profile mutations through the application's
// ServerProfiles object. This fixture exposes that object and records the
// AppController calls made by the Connect button; it deliberately contains no
// alternate profile logic.
class TestApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(ch::ServerProfiles* serverProfiles READ serverProfiles CONSTANT)
    Q_PROPERTY(QObject* settings READ settings CONSTANT)

public:
    explicit TestApp(ServerProfiles* profiles, QObject* parent = nullptr)
        : QObject(parent), m_profiles(profiles)
    {
    }

    ServerProfiles* serverProfiles() const { return m_profiles; }
    QObject* settings() const { return nullptr; }

    Q_INVOKABLE void connectToProfile(const QString& id) { connectRequests.append(id); }
    Q_INVOKABLE void disconnectServer() { ++disconnectRequests; }
    Q_INVOKABLE void upgradeRemoteService(const QString& id) { upgradeRequests.append(id); }

    QStringList connectRequests;
    QStringList upgradeRequests;
    int disconnectRequests = 0;

private:
    ServerProfiles* m_profiles = nullptr;
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
    void outOfRangeStoredPortsAreRepairedNotTruncated();
    void fractionalAndBooleanPortsAreRejected();
    void secretsInTheCallersMapNeverReachTheStore();
    void aSecondWritersProfileSurvivesEveryMutation();
    void aDeletionIsNeverResurrectedByTheMerge();
    void aConflictingEditIsLastWriteWins();
    void aMergeLeaksNoSecretAndNoWiderPermissions();
    void twoWritersRacingInSeparateProcessesLoseNothing();
    void interprocessWriterChild();
    void aSaveThatCannotTakeTheLockStillSavesAndSaysSo();
    void aStaleLockFromADeadHolderDoesNotWedgeTheStore();
    void aFirstRunWithNoConfigDirectoryLocksSilently();
    void blankHostOrUserRowsAreNotProfilesAndAreDroppedOnLoad();
    void hostAndUserWithEmbeddedWhitespaceAreNotProfiles();
    void aSaveDegradedHandlerMayMutateTheStore();

    // ---- SettingsWindow.qml server pane / ConnectSheet prompts ----
    void sheetLoadsSilentlyAndExposesItsApi();
    void sheetListsProfilesAndMarksTheActiveOne();
    void sheetEmitsConnectAndRemoveForTheSelection();
    void sheetSavesANewProfileThenEditsIt();
    void sheetHostKeyPromptDecidesBothWays();
    void sheetCredentialPromptMasksSubmitsAndKeepsTheSecretOffDisk();
    void sheetSurfacesErrorTextWithoutBlocking();
    void sheetOpensSshDiagnostics();
    void sheetIsUsableFromTheKeyboardAlone();
    void coldStartAddsAServerThenFindsItAgainAfterRelaunch();

private:
    QString iniPath(const QString& name) const { return m_dir.filePath(name); }

    // Loads the source component into a shown QQuickView.
    std::unique_ptr<QQuickView> loadQml(const QString& path, QObject* app = nullptr);
    std::unique_ptr<QQuickView> loadSettings(TestApp* app);
    std::unique_ptr<QQuickView> loadConnectSheet();

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

        id = store.addProfile(profileFields(
            QStringLiteral("Prod box"), QStringLiteral("10.0.0.4"), 65535,
            QStringLiteral("yichen"), QStringLiteral("/home/yichen/.local/bin/node"),
            QStringLiteral("/srv/codeharbor"),
            QStringLiteral("/home/yichen/.ssh/id_ed25519")));
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
        QCOMPARE(stored.value(QStringLiteral("identityFile")).toString(),
                 QStringLiteral("/home/yichen/.ssh/id_ed25519"));
        QCOMPARE(stored.value(QStringLiteral("nodePath")).toString(),
                 QStringLiteral("/home/yichen/.local/bin/node"));
        QCOMPARE(stored.value(QStringLiteral("repoRoot")).toString(),
                 QStringLiteral("/srv/codeharbor"));
        // The port stays an int, both in value and in metatype: QML binds it to
        // a spin/int field and SshConnectionPool takes a quint16.
        QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 65535);
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
    QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 65535);
    QCOMPARE(stored.value(QStringLiteral("port")).typeId(), QMetaType::Int);
    QCOMPARE(stored.value(QStringLiteral("user")).toString(), QStringLiteral("yichen"));
    QCOMPARE(stored.value(QStringLiteral("identityFile")).toString(),
             QStringLiteral("/home/yichen/.ssh/id_ed25519"));
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
    QVERIFY2(ini.contains(id + QStringLiteral("\\port=65535")), qPrintable(ini));
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

// The port is the one stored field where a hand edit turns from wrong into
// MISLEADING. Its consumer (AppController::connectToProfile) narrows whatever
// profile() hands back with a plain static_cast<quint16> before giving it to
// SshConnectionPool, so a value that escaped the store unchecked would not be
// rejected downstream - it would be silently truncated. A stored 70000 becomes
// 4464, a stored -1 becomes 65535, and the user is told the connection to a
// port they never typed failed, with nothing anywhere naming the real cause.
//
// There is exactly one place that may catch this and it is the store: the read
// side repairs anything outside 1..65535 to the SSH default, and the write side
// refuses it outright. handEditedStoreIsRepairedOnLoad() covers only the
// NON-NUMERIC spelling, which lands at 0 and is caught by the lower bound
// alone; the upper bound and the negative case had no test at all, so dropping
// either half of that check was a silent regression.
void TstServerProfiles::outOfRangeStoredPortsAreRepairedNotTruncated()
{
    const QString path = iniPath(QStringLiteral("ports.ini"));
    {
        QSettings raw(path, QSettings::IniFormat);
        const auto seed = [&raw](const QString& id, const QVariant& port) {
            const QString prefix = QStringLiteral("servers/") + id + QLatin1Char('/');
            raw.setValue(prefix + QStringLiteral("name"), id);
            raw.setValue(prefix + QStringLiteral("host"), id + QStringLiteral(".example"));
            raw.setValue(prefix + QStringLiteral("user"), QStringLiteral("u"));
            raw.setValue(prefix + QStringLiteral("port"), port);
        };
        seed(QStringLiteral("aaa-over"), 70000);  // quint16 truncation -> 4464
        seed(QStringLiteral("bbb-neg"), -1);      // -> 65535
        seed(QStringLiteral("ccc-wrap"), 65558);  // -> 22, i.e. wrong AND plausible
        seed(QStringLiteral("ddd-zero"), 0);
        seed(QStringLiteral("eee-top"), 65535);   // the legal boundary
        seed(QStringLiteral("fff-bottom"), 1);    // the other legal boundary
        raw.sync();
    }

    ServerProfiles store(path);
    const auto portOf = [&store](const char* id) {
        return store.profile(QLatin1String(id)).value(QStringLiteral("port")).toInt();
    };
    QCOMPARE(portOf("aaa-over"), 22);
    QCOMPARE(portOf("bbb-neg"), 22);
    QCOMPARE(portOf("ccc-wrap"), 22);
    QCOMPARE(portOf("ddd-zero"), 22);
    // ...and the legal boundaries are NOT "repaired". A store that answered 22
    // for everything would satisfy the four above and quietly break port 65535.
    QCOMPARE(portOf("eee-top"), 65535);
    QCOMPARE(portOf("fff-bottom"), 1);

    // Nothing a consumer can see changes value when it is narrowed to quint16,
    // which is the property the cast at the other end depends on.
    const QVariantList visible = store.profiles();
    QCOMPARE(visible.size(), 6);
    for (const QVariant& entry : visible) {
        const int port = entry.toMap().value(QStringLiteral("port")).toInt();
        QVERIFY2(port >= 1 && port <= 65535, qPrintable(QString::number(port)));
        QCOMPARE(static_cast<int>(static_cast<quint16>(port)), port);
    }

    // The repair is written back on the next save, so the bad value is not left
    // in the file for an older build - or a hand edit of a DIFFERENT field - to
    // pick up again.
    store.setActiveId(QStringLiteral("eee-top"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString ini = QString::fromUtf8(file.readAll());
    QVERIFY2(!ini.contains(QStringLiteral("port=70000")), qPrintable(ini));
    QVERIFY2(!ini.contains(QStringLiteral("port=-1")), qPrintable(ini));
    ServerProfiles reopened(path);
    QCOMPARE(reopened.profile(QStringLiteral("aaa-over"))
                 .value(QStringLiteral("port")).toInt(), 22);
    QCOMPARE(reopened.profile(QStringLiteral("eee-top"))
                 .value(QStringLiteral("port")).toInt(), 65535);

    // Same rule on the way IN, so the read side is repairing hand edits rather
    // than covering for this store's own writer: one rule, both ends.
    QVERIFY(store.addProfile(profileFields(QStringLiteral("g"), QStringLiteral("hg"),
                                           70000, QStringLiteral("u")))
                .isEmpty());
    QVERIFY(store.addProfile(profileFields(QStringLiteral("h"), QStringLiteral("hh"),
                                           -1, QStringLiteral("u")))
                .isEmpty());
    store.updateProfile(QStringLiteral("eee-top"),
                        QVariantMap{{QStringLiteral("port"), 70000}});
    QCOMPARE(portOf("eee-top"), 65535);
}

void TstServerProfiles::fractionalAndBooleanPortsAreRejected()
{
    const QString path = iniPath(QStringLiteral("port-types.ini"));
    {
        QSettings raw(path, QSettings::IniFormat);
        const auto seed = [&raw](const QString& id, const QVariant& port) {
            const QString prefix = QStringLiteral("servers/") + id + QLatin1Char('/');
            raw.setValue(prefix + QStringLiteral("name"), id);
            raw.setValue(prefix + QStringLiteral("host"), QStringLiteral("host"));
            raw.setValue(prefix + QStringLiteral("user"), QStringLiteral("user"));
            raw.setValue(prefix + QStringLiteral("port"), port);
        };
        seed(QStringLiteral("stored-fraction"), 22.5);
        seed(QStringLiteral("stored-bool"), true);
        raw.sync();
    }

    ServerProfiles store(path);
    QCOMPARE(store.profile(QStringLiteral("stored-fraction"))
                 .value(QStringLiteral("port")).toInt(),
             22);
    QCOMPARE(store.profile(QStringLiteral("stored-bool"))
                 .value(QStringLiteral("port")).toInt(),
             22);
    QVERIFY(store.addProfile(profileFields(QStringLiteral("fraction"),
                                           QStringLiteral("host"), 22.5,
                                           QStringLiteral("user")))
                .isEmpty());
    QVERIFY(store.addProfile(profileFields(QStringLiteral("fraction-text"),
                                           QStringLiteral("host"), QStringLiteral("22.5"),
                                           QStringLiteral("user")))
                .isEmpty());
    QVERIFY(store.addProfile(profileFields(QStringLiteral("boolean"),
                                           QStringLiteral("host"), true,
                                           QStringLiteral("user")))
                .isEmpty());

    const QString id = store.addProfile(profileFields(QStringLiteral("valid"),
                                                      QStringLiteral("host"), 22,
                                                      QStringLiteral("user")));
    QVERIFY(!id.isEmpty());
    store.updateProfile(id, QVariantMap{{QStringLiteral("port"), 22.5}});
    QCOMPARE(store.profile(id).value(QStringLiteral("port")).toInt(), 22);
    QVERIFY(store.addProfile(profileFields(QStringLiteral("list"),
                                           QStringLiteral("host"), QVariantList{22},
                                           QStringLiteral("user")))
                .isEmpty());
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

// A profile is hand-entered configuration: losing one is losing user data, and
// the user gets no error and no hint that it happened. The store reads the file
// once and rewrites the whole `servers` group from memory, so before merging
// every write destroyed anything a second writer had added since construction —
// two open windows, or an editor open on the file. Every mutation path
// (add/update/remove/select, and the ordinal rewrite a removal implies) goes
// through the same persist(), so all of them are exercised here.
void TstServerProfiles::aSecondWritersProfileSurvivesEveryMutation()
{
    const QString path = iniPath(QStringLiteral("secondwriter.ini"));
    ServerProfiles store(path);
    const QString a = store.addProfile(
        profileFields(QStringLiteral("a"), QStringLiteral("ha"), 22, QStringLiteral("u")));
    const QString b = store.addProfile(
        profileFields(QStringLiteral("b"), QStringLiteral("hb"), 22, QStringLiteral("u")));

    // Window B adds a server. Ordinal 0 is what a second instance really writes
    // for its first profile, so this also proves our own order still wins.
    const QString outsider = QStringLiteral("11111111-2222-3333-4444-555555555555");
    writeProfileBehindOurBack(path, outsider, QStringLiteral("fromWindowB"), 0);

    // ---- add ---- (this is the case that fails against the pre-merge store)
    const QString c = store.addProfile(
        profileFields(QStringLiteral("c"), QStringLiteral("hc"), 22, QStringLiteral("u")));
    QVERIFY(!c.isEmpty());
    {
        ServerProfiles reader(path);
        QCOMPARE(namesOf(reader.profiles()),
                 QStringList({QStringLiteral("a"), QStringLiteral("b"),
                              QStringLiteral("c"), QStringLiteral("fromWindowB")}));
        // Kept verbatim, not just by name.
        const QVariantMap kept = reader.profile(outsider);
        QCOMPARE(kept.value(QStringLiteral("host")).toString(),
                 QStringLiteral("fromWindowB.example"));
        QCOMPARE(kept.value(QStringLiteral("user")).toString(), QStringLiteral("otheruser"));
        QCOMPARE(kept.value(QStringLiteral("port")).toInt(), 2200);
    }
    // Merging is a write-time rule, not synchronisation: this instance does NOT
    // adopt the other window's profile into the list it shows.
    QCOMPARE(namesOf(store.profiles()),
             QStringList({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));

    // ---- update ----
    store.updateProfile(b, QVariantMap{{QStringLiteral("host"), QStringLiteral("hb2")}});
    {
        ServerProfiles reader(path);
        QCOMPARE(namesOf(reader.profiles()),
                 QStringList({QStringLiteral("a"), QStringLiteral("b"),
                              QStringLiteral("c"), QStringLiteral("fromWindowB")}));
    }

    // ---- select ----
    store.setActiveId(c);
    {
        ServerProfiles reader(path);
        QCOMPARE(idsOf(reader.profiles()), QStringList({a, b, c, outsider}));
        QCOMPARE(reader.activeId(), c);
    }

    // ---- remove, which also reorders: every survivor's ordinal is rewritten ----
    store.removeProfile(a);
    {
        ServerProfiles reader(path);
        QCOMPARE(idsOf(reader.profiles()), QStringList({b, c, outsider}));
    }

    // A second outsider is kept too. Outsiders keep the order the FILE gives
    // them, not the order they happened to arrive in: this one carries ordinal
    // 1 (window B's second slot) while the first outsider carries the ordinal 2
    // our last merge gave it, so the file's own order puts the newcomer first.
    // What matters is that it is deterministic and that neither is dropped.
    const QString outsider2 = QStringLiteral("22222222-3333-4444-5555-666666666666");
    writeProfileBehindOurBack(path, outsider2, QStringLiteral("alsoWindowB"), 1);
    store.updateProfile(c, QVariantMap{{QStringLiteral("name"), QStringLiteral("c2")}});
    ServerProfiles reader(path);
    QCOMPARE(idsOf(reader.profiles()), QStringList({b, c, outsider2, outsider}));
    QCOMPARE(namesOf(reader.profiles()),
             QStringList({QStringLiteral("b"), QStringLiteral("c2"),
                          QStringLiteral("alsoWindowB"), QStringLiteral("fromWindowB")}));
}

// The subtle half, and the reason a plain union of memory and file is wrong. The
// other window's copy of the list predates our deletion, so the deleted profile
// is still in the file when we merge — and "keep what I do not know about" would
// bring it back. A profile the user deleted staying deleted matters as much as
// one they typed staying present.
// It passes trivially against a store that simply overwrites everything; it is
// here to stop the merge from ever being written as a union of file and memory.
void TstServerProfiles::aDeletionIsNeverResurrectedByTheMerge()
{
    const QString path = iniPath(QStringLiteral("nodeleteundo.ini"));
    ServerProfiles store(path);
    const QString a = store.addProfile(
        profileFields(QStringLiteral("a"), QStringLiteral("ha"), 22, QStringLiteral("u")));
    const QString b = store.addProfile(
        profileFields(QStringLiteral("b"), QStringLiteral("hb"), 22, QStringLiteral("u")));

    store.removeProfile(a);
    QCOMPARE(idsOf(store.profiles()), QStringList({b}));

    // Window B, still holding `a` in its own memory, writes it back out.
    writeProfileBehindOurBack(path, a, QStringLiteral("a"), 0);
    {
        // It really is back in the file — an untainted third reader sees it.
        // (Order is by stored ordinal then id, which is not interesting here.)
        ServerProfiles bystander(path);
        QCOMPARE(bystander.profiles().size(), 2);
        QVERIFY(idsOf(bystander.profiles()).contains(a));
        QVERIFY(idsOf(bystander.profiles()).contains(b));
    }

    // Our next write must not mistake it for somebody else's new profile...
    const QString c = store.addProfile(
        profileFields(QStringLiteral("c"), QStringLiteral("hc"), 22, QStringLiteral("u")));
    {
        ServerProfiles reader(path);
        QCOMPARE(idsOf(reader.profiles()), QStringList({b, c}));
    }

    // ...and it stays gone however many times it reappears and whatever we do
    // next: the deletion is remembered for the lifetime of this instance.
    writeProfileBehindOurBack(path, a, QStringLiteral("a"), 0);
    store.updateProfile(b, QVariantMap{{QStringLiteral("host"), QStringLiteral("hb2")}});
    writeProfileBehindOurBack(path, a, QStringLiteral("a"), 0);
    store.removeProfile(c);
    ServerProfiles reader(path);
    QCOMPARE(idsOf(reader.profiles()), QStringList({b}));
}

// The documented conflict rule (see the CONCURRENT WRITERS block in
// ServerProfiles.h): a profile this instance holds is written from OUR memory,
// whole. The write in progress is the later one, so it wins; there are no
// per-field timestamps in a hand-editable ini and the user's own most recent
// action is the best tiebreak available. The consequence is deliberate and
// pinned here in both directions.
void TstServerProfiles::aConflictingEditIsLastWriteWins()
{
    const QString path = iniPath(QStringLiteral("conflict.ini"));
    ServerProfiles store(path);
    const QString id = store.addProfile(profileFields(QStringLiteral("box"),
                                                      QStringLiteral("h1"), 22,
                                                      QStringLiteral("u")));
    const QString other = store.addProfile(
        profileFields(QStringLiteral("other"), QStringLiteral("h2"), 22, QStringLiteral("u")));

    // Window B edits the SAME profile: new name, new host, new user, new port.
    writeProfileBehindOurBack(path, id, QStringLiteral("editedElsewhere"), 0);

    // We then edit it ourselves. Our whole profile replaces theirs — the edit we
    // carry applies, and every field we did not touch reverts to what THIS
    // instance believes, not what they wrote.
    store.updateProfile(id, QVariantMap{{QStringLiteral("port"), 2022}});
    {
        ServerProfiles reader(path);
        const QVariantMap won = reader.profile(id);
        QCOMPARE(won.value(QStringLiteral("name")).toString(), QStringLiteral("box"));
        QCOMPARE(won.value(QStringLiteral("host")).toString(), QStringLiteral("h1"));
        QCOMPARE(won.value(QStringLiteral("user")).toString(), QStringLiteral("u"));
        QCOMPARE(won.value(QStringLiteral("port")).toInt(), 2022);
        QCOMPARE(reader.profiles().size(), 2); // one profile, not two rival copies
    }

    // The rule is per WRITE, not per profile: a write about something else still
    // rewrites every profile we hold, so their edit is lost then too. Stated
    // plainly because it is the price of the rule, not an accident.
    writeProfileBehindOurBack(path, id, QStringLiteral("editedAgainElsewhere"), 0);
    store.updateProfile(other, QVariantMap{{QStringLiteral("host"), QStringLiteral("h2b")}});
    {
        ServerProfiles reader(path);
        QCOMPARE(reader.profile(id).value(QStringLiteral("name")).toString(),
                 QStringLiteral("box"));
    }

    // The selection is last-write-wins on the same terms, and the invariant
    // holds through it: `active` still names a profile that exists. (Our own
    // selection is unchanged, so it takes an unrelated mutation to write it —
    // a store that changes nothing writes nothing, and then the other writer's
    // selection simply stands.)
    {
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue(QStringLiteral("servers/active"), other);
        raw.sync();
    }
    QCOMPARE(store.activeId(), id);
    store.updateProfile(other, QVariantMap{{QStringLiteral("host"), QStringLiteral("h2c")}});
    ServerProfiles reader(path);
    QCOMPARE(reader.activeId(), id);
    QVERIFY(!reader.profile(reader.activeId()).isEmpty());
}

// Both security properties of this class have to survive the merge path, which
// is a SECOND place where profile fields get written. A hand edit can put a
// `password` key straight into the file; carrying an unknown key forward "for
// compatibility" would mean the store copying a secret it refuses to accept from
// its own API. And the merge must not be a way to end up with a store other
// accounts can read — or, worse, edit to redirect `host` at a machine they own.
void TstServerProfiles::aMergeLeaksNoSecretAndNoWiderPermissions()
{
    const QString path = iniPath(QStringLiteral("mergesecurity.ini"));
    const QString secret = QStringLiteral("hunter2-from-the-other-writer");

    ServerProfiles store(path);
    const QString mine = store.addProfile(
        profileFields(QStringLiteral("mine"), QStringLiteral("hm"), 22, QStringLiteral("u")));
    QVERIFY(!mine.isEmpty());
    QVERIFY(privatePermissionsHold(path)); // narrow to begin with

    // A second writer adds a profile and, in the same file, a password key.
    const QString outsider = QStringLiteral("33333333-4444-5555-6666-777777777777");
    writeProfileBehindOurBack(path, outsider, QStringLiteral("fromWindowB"), 0,
                             QVariantMap{{QStringLiteral("password"), secret}});
    // A second QSettings writer over the same file is expected to leave the mode
    // alone; assert it rather than assume it, since a rewrite that recreated the
    // file would silently reopen it to the umask default.
    QVERIFY2(privatePermissionsHold(path),
             "a second settings writer widened the store's permissions");
    // And even when it does not — a hand edit through an editor that recreates
    // the file will — the merge must narrow it back, not inherit it.
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner
                                            | QFile::ReadGroup | QFile::WriteGroup
                                            | QFile::ReadOther));

    store.updateProfile(mine, QVariantMap{{QStringLiteral("host"), QStringLiteral("hm2")}});

    // The outsider's profile survived...
    ServerProfiles reader(path);
    QCOMPARE(namesOf(reader.profiles()),
             QStringList({QStringLiteral("mine"), QStringLiteral("fromWindowB")}));
    // ...with exactly the whitelist and nothing else...
    QCOMPARE(reader.profile(outsider).keys(),
             QStringList({QStringLiteral("host"), QStringLiteral("id"),
                          QStringLiteral("identityFile"), QStringLiteral("name"),
                          QStringLiteral("nodePath"), QStringLiteral("port"),
                          QStringLiteral("repoRoot"), QStringLiteral("user")}));
    // ...and the secret the merge could have copied forward is gone from disk.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString ini = QString::fromUtf8(file.readAll());
    QVERIFY2(!ini.contains(secret), qPrintable(ini));
    QVERIFY2(!ini.contains(QStringLiteral("password")), qPrintable(ini));
    // The write narrowed the mode back to owner-only.
    QVERIFY2(privatePermissionsHold(path),
             qPrintable(QString::number(static_cast<uint>(QFile::permissions(path)), 16)));
}

// Merging closes the gap only when the merge is not itself interleaved. Two
// processes can both re-read, both compute the same merged list, and both write
// it: whoever finishes second has never seen the profile the other just added,
// and it is gone. This is the case the class comment promises cannot happen, and
// the only way to prove it is with two real processes — two ServerProfiles in
// ONE process share Qt's per-file QSettings cache, so they cannot reproduce it.
//
// The child is this same test binary re-run with a single function name and the
// ini path in the environment. Both sides add the same number of profiles; the
// parent does not start until the child says it is about to, so the writes
// genuinely overlap. Nothing may be missing at the end.
void TstServerProfiles::twoWritersRacingInSeparateProcessesLoseNothing()
{
    constexpr int kEach = 60;
    const QString path = iniPath(QStringLiteral("race.ini"));
    const QString ready = path + QStringLiteral(".child-ready");

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("CH_PROFILE_CHILD_INI"), path);
    env.insert(QStringLiteral("CH_PROFILE_CHILD_READY"), ready);
    env.insert(QStringLiteral("CH_PROFILE_CHILD_COUNT"), QString::number(kEach));

    QProcess child;
    child.setProcessEnvironment(env);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(),
                QStringList({QStringLiteral("interprocessWriterChild")}));
    QVERIFY2(child.waitForStarted(30000), qPrintable(child.errorString()));

    QVERIFY2(QTest::qWaitFor([&] { return QFile::exists(ready); }, 30000),
             "the child never reached its first write");

    ServerProfiles store(path);
    QStringList mine;
    for (int i = 0; i < kEach; ++i) {
        const QString name = QStringLiteral("parent-%1").arg(i);
        QVERIFY(!store.addProfile(profileFields(name, QStringLiteral("hp"), 22,
                                                QStringLiteral("u")))
                     .isEmpty());
        mine.append(name);
    }

    QVERIFY2(child.waitForFinished(120000), qPrintable(child.errorString()));
    QVERIFY2(child.exitCode() == 0, qPrintable(QString::fromLocal8Bit(child.readAll())));

    ServerProfiles reader(path);
    const QStringList names = namesOf(reader.profiles());
    for (const QString& name : std::as_const(mine))
        QVERIFY2(names.contains(name), qPrintable(QStringLiteral("lost %1").arg(name)));
    for (int i = 0; i < kEach; ++i) {
        const QString name = QStringLiteral("child-%1").arg(i);
        QVERIFY2(names.contains(name), qPrintable(QStringLiteral("lost %1").arg(name)));
    }
    QCOMPARE(names.size(), 2 * kEach);
    // The race left no lock file behind and did not widen the store.
    QVERIFY(!QFile::exists(path + QStringLiteral(".merge-lock")));
    QVERIFY(privatePermissionsHold(path));
}

// The other half of the case above, running in its own process.
void TstServerProfiles::interprocessWriterChild()
{
    const QString path = qEnvironmentVariable("CH_PROFILE_CHILD_INI");
    if (path.isEmpty())
        QSKIP("helper: driven by twoWritersRacingInSeparateProcessesLoseNothing");

    ServerProfiles store(path);
    {
        QFile ready(qEnvironmentVariable("CH_PROFILE_CHILD_READY"));
        QVERIFY(ready.open(QIODevice::WriteOnly));
    }
    const int count = qEnvironmentVariable("CH_PROFILE_CHILD_COUNT").toInt();
    for (int i = 0; i < count; ++i) {
        QVERIFY(!store.addProfile(profileFields(QStringLiteral("child-%1").arg(i),
                                                QStringLiteral("hc"), 22,
                                                QStringLiteral("u")))
                     .isEmpty());
    }
}

// What happens when the lock cannot be had at all. The decision (documented in
// ServerProfiles.h) is to save anyway and say so: throwing away the profile the
// user just typed is a certain loss taken to avoid an unlikely one, and doing
// the unlocked write quietly would make the class comment a lie. Both halves are
// pinned here — the profile IS saved, and saveDegraded() names the holder — plus
// the fact that the wait is bounded, because this runs on the UI thread.
void TstServerProfiles::aSaveThatCannotTakeTheLockStillSavesAndSaysSo()
{
    const QString path = iniPath(QStringLiteral("lockedout.ini"));
    const QString lockPath = path + QStringLiteral(".merge-lock");

    ServerProfiles store(path);
    QSignalSpy degraded(&store, &ServerProfiles::saveDegraded);
    const QString first = store.addProfile(profileFields(
        QStringLiteral("first"), QStringLiteral("h1"), 22, QStringLiteral("u")));
    QVERIFY(!first.isEmpty());
    QCOMPARE(degraded.count(), 0);
    // An ordinary save leaves no lock file lying in the user's config directory.
    QVERIFY(!QFile::exists(lockPath));

    // Somebody else takes it and does not let go. This process is alive and owns
    // the lock file, so it is not stale by any measure and the wait runs out.
    QLockFile blocker(lockPath);
    QVERIFY(blocker.tryLock(5000));

    QElapsedTimer waited;
    waited.start();
    const QString second = store.addProfile(profileFields(
        QStringLiteral("second"), QStringLiteral("h2"), 22, QStringLiteral("u")));
    const qint64 elapsed = waited.elapsed();

    QVERIFY2(!second.isEmpty(), "the user's profile was refused instead of saved");
    QCOMPARE(degraded.count(), 1);
    const QString reason = degraded.first().first().toString();
    // The CAUSE, not a finished sentence: AppController owns the wording the
    // user reads. It names the holder, which here is this very process.
    QVERIFY2(reason.contains(QStringLiteral("holding it")), qPrintable(reason));
    QVERIFY2(reason.contains(QString::number(QCoreApplication::applicationPid())),
             qPrintable(reason));
    // Bounded: it waited for the timeout and then gave up, rather than hanging.
    QVERIFY2(elapsed >= 1000 && elapsed < 15000,
             qPrintable(QStringLiteral("waited %1 ms").arg(elapsed)));

    // Edge-triggered: the second and third blocked save say nothing. A profile
    // sheet saves on every field commit, and one toast per keystroke would bury
    // the message it repeats.
    QVERIFY(!store.addProfile(profileFields(QStringLiteral("third"),
                                            QStringLiteral("h3"), 22,
                                            QStringLiteral("u")))
                 .isEmpty());
    store.updateProfile(first, QVariantMap{{QStringLiteral("host"),
                                            QStringLiteral("h1b")}});
    QCOMPARE(degraded.count(), 1);

    blocker.unlock();
    ServerProfiles reader(path);
    QCOMPARE(namesOf(reader.profiles()),
             QStringList({QStringLiteral("first"), QStringLiteral("second"),
                          QStringLiteral("third")}));
    QVERIFY(privatePermissionsHold(path));

    // A save that DOES get the lock is silent and re-arms the report, so the
    // next outage is announced instead of being swallowed for the session.
    store.addProfile(profileFields(QStringLiteral("fourth"), QStringLiteral("h4"), 22,
                                   QStringLiteral("u")));
    QCOMPARE(degraded.count(), 1);
    QVERIFY(blocker.tryLock(5000));
    store.addProfile(profileFields(QStringLiteral("fifth"), QStringLiteral("h5"), 22,
                                   QStringLiteral("u")));
    QCOMPARE(degraded.count(), 2);
    blocker.unlock();
}

// A process killed mid-save leaves its lock file behind. If that wedged the
// store, every later save would sit through the timeout and take the degraded
// path forever — the store would be permanently broken by one crash. QLockFile
// records the holder's pid, so a lock whose holder is gone is recognised and
// cleared; this pins that, using a pid that really is dead (a process this test
// ran and reaped) rather than a made-up number.
void TstServerProfiles::aStaleLockFromADeadHolderDoesNotWedgeTheStore()
{
    const QString path = iniPath(QStringLiteral("stalelock.ini"));
    const QString lockPath = path + QStringLiteral(".merge-lock");

    ServerProfiles store(path);
    QSignalSpy degraded(&store, &ServerProfiles::saveDegraded);
    QVERIFY(!store.addProfile(profileFields(QStringLiteral("first"), QStringLiteral("h1"),
                                            22, QStringLiteral("u")))
                 .isEmpty());

    QProcess corpse;
    corpse.start(QCoreApplication::applicationFilePath(),
                 QStringList({QStringLiteral("interprocessWriterChild")}));
    QVERIFY2(corpse.waitForStarted(30000), qPrintable(corpse.errorString()));
    const qint64 deadPid = corpse.processId();
    QVERIFY(corpse.waitForFinished(30000));
    QVERIFY(deadPid > 0);

    // QLockFile's format: pid, application name, hostname, one per line. Written
    // world-writable, the way a crashed older build or a text editor would.
    {
        QFile stale(lockPath);
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write(QByteArray::number(deadPid) + "\nghost\n"
                    + QSysInfo::machineHostName().toUtf8() + "\n");
        stale.close();
        QVERIFY(stale.setPermissions(QFile::ReadOwner | QFile::WriteOwner
                                     | QFile::ReadGroup | QFile::WriteGroup
                                     | QFile::ReadOther | QFile::WriteOther));
    }

    QElapsedTimer waited;
    waited.start();
    QVERIFY(!store.addProfile(profileFields(QStringLiteral("second"), QStringLiteral("h2"),
                                            22, QStringLiteral("u")))
                 .isEmpty());
    const qint64 elapsed = waited.elapsed();

    // Cleared, not waited out: no degraded save, and nowhere near the timeout.
    QCOMPARE(degraded.count(), 0);
    QVERIFY2(elapsed < 1000, qPrintable(QStringLiteral("waited %1 ms").arg(elapsed)));
    // The dead holder's world-writable lock file is gone, not inherited.
    QVERIFY(!QFile::exists(lockPath));
    QVERIFY(privatePermissionsHold(path));

    ServerProfiles reader(path);
    QCOMPARE(namesOf(reader.profiles()),
             QStringList({QStringLiteral("first"), QStringLiteral("second")}));
}

// The case every new installation hits, and the one the lock broke: on a first
// run ~/.config/CodeHarbor does not exist yet. QSettings creates it lazily, when
// it first writes — which is after the lock is taken — and QLockFile cannot make
// its lock file in a directory that is not there, so the very first save of the
// very first profile took the degraded path and told the user their server list
// was saved without its safeguard. On a clean first run there is provably no
// second writer: the lock must succeed and nothing may be said.
void TstServerProfiles::aFirstRunWithNoConfigDirectoryLocksSilently()
{
    // Two levels deep and absent, the way a fresh XDG config path is.
    const QString directory = m_dir.filePath(QStringLiteral("fresh/CodeHarbor"));
    const QString path = directory + QStringLiteral("/CodeHarbor.conf");
    QVERIFY(!QFileInfo::exists(directory));

    ServerProfiles store(path);
    QSignalSpy degraded(&store, &ServerProfiles::saveDegraded);
    const QString id = store.addProfile(profileFields(
        QStringLiteral("first ever"), QStringLiteral("h1"), 22, QStringLiteral("u")));

    QVERIFY(!id.isEmpty());
    QVERIFY2(degraded.isEmpty(),
             qPrintable(degraded.isEmpty() ? QString()
                                           : degraded.first().first().toString()));
    // It really saved, and the ordinary post-conditions hold on the new store.
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(!QFile::exists(path + QStringLiteral(".merge-lock")));
    QVERIFY(privatePermissionsHold(path));
    ServerProfiles reader(path);
    QCOMPARE(namesOf(reader.profiles()), QStringList({QStringLiteral("first ever")}));
    QCOMPARE(reader.activeId(), id);
}

// Read-side counterpart to addRejectsInputThatCouldNeverConnect(): the save
// path refuses to store a profile with no host or no user, so a store that has
// one has been hand edited (or written by something that is not this class).
// Such a row is not a profile - connectToHost(host, port, user) needs all
// three - it is a server entry that can only fail the moment it is selected,
// and before this it loaded, listed, and could be made active.
//
// The port is the contrast, and the reason this lives at the same site: a
// nonsense port is REPAIRED, because "absent" already means 22 so the repair
// invents nothing. There is no such answer for a blank host.
void TstServerProfiles::blankHostOrUserRowsAreNotProfilesAndAreDroppedOnLoad()
{
    const QString path = iniPath(QStringLiteral("blankfields.ini"));
    {
        QSettings raw(path, QSettings::IniFormat);
        // Usable.
        raw.setValue(QStringLiteral("servers/aaa/name"), QStringLiteral("Good"));
        raw.setValue(QStringLiteral("servers/aaa/host"), QStringLiteral("good.example"));
        raw.setValue(QStringLiteral("servers/aaa/user"), QStringLiteral("u"));
        raw.setValue(QStringLiteral("servers/aaa/port"), 22);
        // Host key deleted by hand.
        raw.setValue(QStringLiteral("servers/bbb/name"), QStringLiteral("NoHost"));
        raw.setValue(QStringLiteral("servers/bbb/user"), QStringLiteral("u"));
        raw.setValue(QStringLiteral("servers/bbb/port"), 22);
        // Host present but blank.
        raw.setValue(QStringLiteral("servers/ccc/name"), QStringLiteral("BlankHost"));
        raw.setValue(QStringLiteral("servers/ccc/host"), QStringLiteral("   "));
        raw.setValue(QStringLiteral("servers/ccc/user"), QStringLiteral("u"));
        raw.setValue(QStringLiteral("servers/ccc/port"), 22);
        // User blank.
        raw.setValue(QStringLiteral("servers/ddd/name"), QStringLiteral("NoUser"));
        raw.setValue(QStringLiteral("servers/ddd/host"), QStringLiteral("d.example"));
        raw.setValue(QStringLiteral("servers/ddd/user"), QString());
        raw.setValue(QStringLiteral("servers/ddd/port"), 22);
        // The selection points at one of the unusable rows.
        raw.setValue(QStringLiteral("servers/active"), QStringLiteral("ccc"));
        raw.sync();
    }

    ServerProfiles store(path);
    QCOMPARE(namesOf(store.profiles()), QStringList({QStringLiteral("Good")}));
    QVERIFY(store.profile(QStringLiteral("bbb")).isEmpty());
    QVERIFY(store.profile(QStringLiteral("ccc")).isEmpty());
    QVERIFY(store.profile(QStringLiteral("ddd")).isEmpty());
    // A selection naming a row that is not a profile is dangling like any other.
    QVERIFY(store.activeId().isEmpty());
    // The one usable row is untouched.
    QCOMPARE(store.profile(QStringLiteral("aaa")).value(QStringLiteral("host")).toString(),
             QStringLiteral("good.example"));

    // The drop is written back on the next save, exactly like the port repair:
    // no unusable row is left in the file for an older build to pick up, and no
    // orphan `servers/<id>/*` keys survive the wipe-and-rewrite.
    store.setActiveId(QStringLiteral("aaa"));
    {
        QSettings raw(path, QSettings::IniFormat);
        raw.sync();
        raw.beginGroup(QStringLiteral("servers"));
        QCOMPARE(raw.childGroups(), QStringList({QStringLiteral("aaa")}));
        raw.endGroup();
    }
    ServerProfiles reopened(path);
    QCOMPARE(namesOf(reopened.profiles()), QStringList({QStringLiteral("Good")}));
    QCOMPARE(reopened.activeId(), QStringLiteral("aaa"));
}

// The other half of "this row is not a profile". A blank host is caught above;
// a host that is not blank but still cannot be one is the case a real user
// produces, because the connect sheet is a plain text field: "box.local -p
// 2222" pasted whole out of an ssh command line, a tab dragged in with a copy
// out of a table, a login name carrying the newline the document it came from
// ended with.
//
// None of those can ever resolve - no hostname, address literal or POSIX login
// name may contain whitespace - and the store used to keep them, so the profile
// listed, looked right, and failed with an opaque name-resolution error every
// single time it was selected. The newline case is the worst of the three: the
// ini writer escapes it as "\n" and reads it back, so it survives a full round
// trip and reaches the resolver intact.
void TstServerProfiles::hostAndUserWithEmbeddedWhitespaceAreNotProfiles()
{
    const QString path = iniPath(QStringLiteral("whitespacefields.ini"));
    ServerProfiles store(path);
    QSignalSpy profilesSpy(&store, &ServerProfiles::profilesChanged);

    const QList<QVariantMap> broken = {
        profileFields(QStringLiteral("n"), QStringLiteral("box.local -p 2222"), 22,
                      QStringLiteral("u")),
        profileFields(QStringLiteral("n"), QStringLiteral("a\tb"), 22,
                      QStringLiteral("u")),
        profileFields(QStringLiteral("n"), QStringLiteral("box\nlocal"), 22,
                      QStringLiteral("u")),
        // Same rule for the login name.
        profileFields(QStringLiteral("n"), QStringLiteral("box.local"), 22,
                      QStringLiteral("my user")),
        profileFields(QStringLiteral("n"), QStringLiteral("box.local"), 22,
                      QStringLiteral("u\nroot")),
    };
    for (const QVariantMap& fields : broken) {
        QVERIFY2(store.addProfile(fields).isEmpty(),
                 qPrintable(QDebug::toString(fields)));
    }
    QVERIFY(store.profiles().isEmpty());
    QCOMPARE(profilesSpy.count(), 0);

    // ...and the rule is NOT a hostname grammar. ':', '%' and '@' are all legal
    // in values people really store - an IPv6 literal with a zone id here - and
    // a store that second-guesses libssh's own parsing refuses connections that
    // work perfectly.
    const QString v6 = store.addProfile(profileFields(
        QStringLiteral("v6"), QStringLiteral("fe80::1%eth0"), 22,
        QStringLiteral("u")));
    QVERIFY(!v6.isEmpty());
    QCOMPARE(store.profile(v6).value(QStringLiteral("host")).toString(),
             QStringLiteral("fe80::1%eth0"));

    // An edit cannot smuggle one into a working profile either: the stored
    // profile survives the bad edit intact.
    store.updateProfile(v6, QVariantMap{{QStringLiteral("host"),
                                         QStringLiteral("box.local -p 22")}});
    QCOMPARE(store.profile(v6).value(QStringLiteral("host")).toString(),
             QStringLiteral("fe80::1%eth0"));

    // READ side, exactly as for a blank host: a hand edit that puts one in the
    // file is dropped rather than listed, and the drop is written back by the
    // next save so no unusable row is left for an older build to pick up.
    {
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue(QStringLiteral("servers/zzz/name"), QStringLiteral("Pasted"));
        raw.setValue(QStringLiteral("servers/zzz/host"),
                     QStringLiteral("box.local -p 2222"));
        raw.setValue(QStringLiteral("servers/zzz/user"), QStringLiteral("u"));
        raw.setValue(QStringLiteral("servers/zzz/port"), 22);
        raw.sync();
    }
    ServerProfiles reopened(path);
    QCOMPARE(idsOf(reopened.profiles()), QStringList({v6}));
    QVERIFY(reopened.profile(QStringLiteral("zzz")).isEmpty());

    reopened.updateProfile(v6, QVariantMap{{QStringLiteral("name"),
                                            QStringLiteral("v6 renamed")}});
    QSettings raw(path, QSettings::IniFormat);
    raw.sync();
    raw.beginGroup(QStringLiteral("servers"));
    QCOMPARE(raw.childGroups(), QStringList({v6}));
    raw.endGroup();
}

// persist() carries no re-entrancy guard, and this is the path that would need
// one if anything inside the locked region ever emitted: saveDegraded() is
// emitted from inside persist(), and a handler is explicitly allowed to react
// by mutating the store. QLockFile is not recursive, so if that emit ever moved
// above the unlock the nested save would sit out the full timeout against a
// lock its own call stack holds and then blame "another process".
//
// Pinned here so the ordering is a tested property rather than a comment: the
// nested save completes, both profiles survive, and the whole thing stays
// inside a bound that a self-deadlocked wait could not.
void TstServerProfiles::aSaveDegradedHandlerMayMutateTheStore()
{
    const QString path = iniPath(QStringLiteral("reentrant.ini"));
    const QString lockPath = path + QStringLiteral(".merge-lock");

    ServerProfiles store(path);
    // Held by a live process that never lets go, so every save below takes the
    // degraded path and therefore really does emit.
    QLockFile blocker(lockPath);
    QVERIFY(blocker.tryLock(5000));

    int handlerRuns = 0;
    QString nested;
    QObject::connect(&store, &ServerProfiles::saveDegraded, &store,
                     [&store, &handlerRuns, &nested](const QString&) {
                         if (handlerRuns++ > 0)
                             return; // one level is the property; not a loop test
                         nested = store.addProfile(profileFields(
                             QStringLiteral("from the handler"),
                             QStringLiteral("h2"), 22, QStringLiteral("u")));
                     });

    QElapsedTimer waited;
    waited.start();
    const QString first = store.addProfile(profileFields(
        QStringLiteral("outer"), QStringLiteral("h1"), 22, QStringLiteral("u")));
    const qint64 elapsed = waited.elapsed();

    QCOMPARE(handlerRuns, 1);
    QVERIFY(!first.isEmpty());
    QVERIFY2(!nested.isEmpty(), "the nested save was refused");
    // Two sequential 1.5 s timeouts, not one plus a deadlock.
    QVERIFY2(elapsed < 15000, qPrintable(QStringLiteral("waited %1 ms").arg(elapsed)));

    blocker.unlock();
    ServerProfiles reader(path);
    QCOMPARE(namesOf(reader.profiles()),
             QStringList({QStringLiteral("outer"),
                          QStringLiteral("from the handler")}));
}

// ---------------------------------------------------------------------------
// Source-loaded QML fixtures
// ---------------------------------------------------------------------------

std::unique_ptr<QQuickView> TstServerProfiles::loadQml(const QString& path,
                                                        QObject* app)
{
    takeLoggedWarnings();
    m_engineWarnings.clear();

    auto view = std::make_unique<QQuickView>();
    QObject::connect(view->engine(), &QQmlEngine::warnings, view.get(),
                     [this](const QList<QQmlError>& warnings) {
                         for (const QQmlError& warning : warnings)
                             m_engineWarnings.append(warning.toString());
                     });

    static const int themeTypeId = qmlRegisterSingletonType(
        QUrl::fromLocalFile(QFileInfo(path).absoluteDir().filePath(QStringLiteral("Theme.qml"))),
        "CodeHarborThemeForTest", 1, 0, "Theme");
    QObject* const theme = view->engine()->singletonInstance<QObject*>(themeTypeId);
    if (!theme) {
        qWarning("QML fixture: Theme.qml could not be instantiated");
        return nullptr;
    }
    view->engine()->rootContext()->setContextProperty(QStringLiteral("Theme"), theme);
    if (app)
        view->engine()->rootContext()->setContextProperty(QStringLiteral("app"), app);
    if (path.endsWith(QStringLiteral("SettingsWindow.qml")))
        view->engine()->rootContext()->setContextProperty(
            QStringLiteral("initialSettingsGroup"), QStringLiteral("server"));

    view->setSource(QUrl::fromLocalFile(path));
    if (view->status() != QQuickView::Ready) {
        QStringList errors;
        for (const QQmlError& error : view->errors())
            errors.append(error.toString());
        qWarning("QML fixture failed to load: %s", qPrintable(errors.join(QLatin1Char('\n'))));
        return nullptr;
    }
    view->show();
    if (!QTest::qWaitForWindowExposed(view.get())) {
        qWarning("QML fixture window was never exposed");
        return nullptr;
    }
    return view;
}

std::unique_ptr<QQuickView> TstServerProfiles::loadSettings(TestApp* app)
{
    auto view = loadQml(QStringLiteral(CH_SETTINGSWINDOW_QML), app);
    if (!view || !view->rootObject())
        return view;
    QObject* const loader =
        view->rootObject()->findChild<QObject*>(QStringLiteral("settingsGroupLoader"));
    if (!loader)
        return nullptr;
    // Finish the default pane before selecting the server pane. Switching a
    // Loader while its first component is incubating produces teardown warnings
    // and can leave the test engine with a destroyed incubation context.
    loader->setProperty("asynchronous", false);
    const auto waitForPane = [&] {
        QElapsedTimer paneWait;
        paneWait.start();
        while (loader->property("status").toInt() != 1 && paneWait.elapsed() < 1000) {
            QCoreApplication::processEvents();
            QTest::qWait(10);
        }
        return loader->property("status").toInt() == 1;
    };
    if (!waitForPane())
        return nullptr;
    view->rootObject()->setProperty("selectedGroup", QStringLiteral("server"));
    if (!waitForPane())
        return nullptr;
    QCoreApplication::processEvents();
    return view;
}

std::unique_ptr<QQuickView> TstServerProfiles::loadConnectSheet()
{
    return loadQml(QStringLiteral(CH_CONNECTSHEET_QML));
}

#define CH_LOAD_SETTINGS(view, root, app)                                                          \
    TestApp app##Fixture(&(app));                                                                  \
    const std::unique_ptr<QQuickView> view = loadSettings(&app##Fixture);                          \
    QVERIFY(view != nullptr);                                                                      \
    QQuickItem* const root = view->rootObject();                                                   \
    QVERIFY(root != nullptr)

#define CH_LOAD_CONNECT(view, root)                                                                \
    const std::unique_ptr<QQuickView> view = loadConnectSheet();                                   \
    QVERIFY(view != nullptr);                                                                      \
    QQuickItem* const root = view->rootObject();                                                   \
    QVERIFY(root != nullptr)

#define CH_ASSERT_SILENT()                                                                         \
    do {                                                                                           \
        QCoreApplication::processEvents();                                                         \
        const QStringList logged = takeLoggedWarnings();                                           \
        QVERIFY2(logged.isEmpty(), qPrintable(logged.join(QLatin1Char('\n'))));                    \
        QVERIFY2(m_engineWarnings.isEmpty(), qPrintable(m_engineWarnings.join(QLatin1Char('\n'))));\
    } while (false)

void TstServerProfiles::sheetLoadsSilentlyAndExposesItsApi()
{
    ServerProfiles store(iniPath(QStringLiteral("settings-api.ini")));
    CH_LOAD_SETTINGS(view, root, store);

    const QMetaObject* mo = root->metaObject();
    const QList<QPair<QByteArray, int>> expectedProperties = {
        {QByteArrayLiteral("profileEntries"), QMetaType::QVariant},
        {QByteArrayLiteral("selectedProfileId"), QMetaType::QString},
        {QByteArrayLiteral("profileName"), QMetaType::QString},
        {QByteArrayLiteral("profileHost"), QMetaType::QString},
        {QByteArrayLiteral("profilePort"), QMetaType::QString},
        {QByteArrayLiteral("profileUser"), QMetaType::QString},
        {QByteArrayLiteral("profileIdentityFile"), QMetaType::QString},
        {QByteArrayLiteral("profileNodePath"), QMetaType::QString},
        {QByteArrayLiteral("profileRepoRoot"), QMetaType::QString},
    };
    for (const auto& expected : expectedProperties) {
        const int index = mo->indexOfProperty(expected.first.constData());
        QVERIFY2(index >= 0, expected.first.constData());
        QCOMPARE(mo->property(index).metaType().id(), expected.second);
    }
    for (const QByteArray& signature : {QByteArrayLiteral("dismissed()"),
                                        QByteArrayLiteral("profileNameFocusRequested()")}) {
        QVERIFY2(mo->indexOfSignal(signature.constData()) >= 0, signature.constData());
    }

    QCOMPARE(root->property("selectedProfileId").toString(), QString());
    QObject* const emptyHint = root->findChild<QObject*>(QStringLiteral("serverEmptyHint"));
    QVERIFY(emptyHint);
    QVERIFY(emptyHint->property("visible").toBool());
    QObject* const addButton = root->findChild<QObject*>(QStringLiteral("serverAddButton"));
    QObject* const deleteButton = root->findChild<QObject*>(QStringLiteral("serverDeleteButton"));
    QVERIFY(addButton && deleteButton);
    QVERIFY(addButton->property("enabled").toBool());
    QVERIFY(!deleteButton->property("enabled").toBool());
    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetListsProfilesAndMarksTheActiveOne()
{
    ServerProfiles store(iniPath(QStringLiteral("settings-list.ini")));
    const QString first = store.addProfile(
        profileFields(QStringLiteral("Alpha"), QStringLiteral("alpha.example"), 22,
                      QStringLiteral("ua"), QStringLiteral("/usr/bin/node"),
                      QStringLiteral("/srv/a")));
    const QString second = store.addProfile(
        profileFields(QStringLiteral(u"Bêta 服务器"), QStringLiteral("beta.example"), 2222,
                      QStringLiteral("ub"), QStringLiteral("/opt/node bin/node"),
                      QStringLiteral("/srv/b")));
    store.setActiveId(second);
    TestApp app(&store);
    const std::unique_ptr<QQuickView> view = loadSettings(&app);
    QVERIFY(view != nullptr);
    QQuickItem* const root = view->rootObject();
    QVERIFY(root != nullptr);

    QTRY_VERIFY(findByName(root, QStringLiteral("serverProfile:%1").arg(second)) != nullptr);
    QCOMPARE(root->property("selectedProfileId").toString(), second);
    QCOMPARE(stringOf(findByName(root, QStringLiteral("serverField:name")), "text"),
             QStringLiteral(u"Bêta 服务器"));
    QCOMPARE(stringOf(findByName(root, QStringLiteral("serverField:host")), "text"),
             QStringLiteral("beta.example"));
    QCOMPARE(stringOf(findByName(root, QStringLiteral("serverField:port")), "text"),
             QStringLiteral("2222"));
    QCOMPARE(stringOf(findByName(root, QStringLiteral("serverField:user")), "text"),
             QStringLiteral("ub"));
    QVERIFY(findByName(root, QStringLiteral("serverProfile:%1").arg(second))
                ->property("checked").toBool());
    QVERIFY(!findByName(root, QStringLiteral("serverProfile:%1").arg(first))
                 ->property("checked").toBool());

    QMetaObject::invokeMethod(findByName(root, QStringLiteral("serverProfile:%1").arg(first)),
                              "clicked");
    QCOMPARE(root->property("selectedProfileId").toString(), first);
    QCOMPARE(store.activeId(), second);
    QCOMPARE(stringOf(findByName(root, QStringLiteral("serverField:host")), "text"),
             QStringLiteral("alpha.example"));
    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetEmitsConnectAndRemoveForTheSelection()
{
    ServerProfiles store(iniPath(QStringLiteral("settings-actions.ini")));
    const QString id = store.addProfile(
        profileFields(QStringLiteral("Alpha"), QStringLiteral("alpha.example"), 22,
                      QStringLiteral("ua")));
    TestApp app(&store);
    const std::unique_ptr<QQuickView> view = loadSettings(&app);
    QVERIFY(view != nullptr);
    QQuickItem* const root = view->rootObject();
    QVERIFY(root != nullptr);

    QObject* const connectButton = root->findChild<QObject*>(QStringLiteral("serverConnectButton"));
    QObject* const deleteButton = root->findChild<QObject*>(QStringLiteral("serverDeleteButton"));
    QObject* const dialog = root->findChild<QObject*>(QStringLiteral("serverDeleteDialog"));
    QVERIFY(connectButton && deleteButton && dialog);
    QVERIFY(connectButton->property("enabled").toBool());
    QMetaObject::invokeMethod(connectButton, "clicked");
    QCOMPARE(app.connectRequests, QStringList({id}));

    QMetaObject::invokeMethod(deleteButton, "clicked");
    QVERIFY(dialog->property("visible").toBool());
    QMetaObject::invokeMethod(dialog, "accept");
    QTRY_VERIFY(store.profiles().isEmpty());
    QCOMPARE(store.activeId(), QString());
    QVERIFY(!deleteButton->property("enabled").toBool());
    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetSavesANewProfileThenEditsIt()
{
    const QString path = iniPath(QStringLiteral("settings-edit.ini"));
    ServerProfiles store(path);
    TestApp app(&store);
    const std::unique_ptr<QQuickView> view = loadSettings(&app);
    QVERIFY(view != nullptr);
    QQuickItem* const root = view->rootObject();
    QVERIFY(root != nullptr);

    QObject* const addButton = root->findChild<QObject*>(QStringLiteral("serverAddButton"));
    QVERIFY(addButton);
    QMetaObject::invokeMethod(addButton, "clicked");
    QTRY_COMPARE(store.profiles().size(), 1);
    const QString id = store.profiles().constFirst().toMap().value(QStringLiteral("id")).toString();
    QVERIFY(!id.isEmpty());
    QCOMPARE(root->property("selectedProfileId").toString(), id);

    QObject* const nameField = findByName(root, QStringLiteral("serverField:name"));
    QObject* const hostField = findByName(root, QStringLiteral("serverField:host"));
    QObject* const portField = findByName(root, QStringLiteral("serverField:port"));
    QObject* const userField = findByName(root, QStringLiteral("serverField:user"));
    QObject* const identityField = findByName(root, QStringLiteral("serverField:identityFile"));
    QObject* const nodeField = findByName(root, QStringLiteral("serverField:nodePath"));
    QObject* const repoField = findByName(root, QStringLiteral("serverField:repoRoot"));
    QVERIFY(nameField && hostField && portField && userField && identityField && nodeField
            && repoField);
    QCOMPARE(stringOf(portField, "text"), QStringLiteral("22"));

    hostField->setProperty("text", QStringLiteral("box.local"));
    userField->setProperty("text", QStringLiteral("yichen"));
    portField->setProperty("text", QStringLiteral("70000"));
    QVERIFY(root->property("profileDirty").toBool());
    QVERIFY(root->findChild<QObject*>(QStringLiteral("serverValidationHint"))
                ->property("visible").toBool());
    QMetaObject::invokeMethod(portField, "editingFinished");
    QCOMPARE(store.profile(id).value(QStringLiteral("port")).toInt(), 22);

    portField->setProperty("text", QStringLiteral("2222"));
    nameField->setProperty("text", QStringLiteral("  Prod box  "));
    identityField->setProperty("text", QStringLiteral("~/.ssh/prod key"));
    nodeField->setProperty("text", QStringLiteral("/home/user name/.local/bin/node"));
    repoField->setProperty("text", QStringLiteral("/srv/my repo"));
    for (QObject* field : {nameField, hostField, portField, userField, identityField, nodeField,
                           repoField})
        QVERIFY(QMetaObject::invokeMethod(field, "editingFinished"));

    const QVariantMap saved = store.profile(id);
    QCOMPARE(saved.value(QStringLiteral("name")).toString(), QStringLiteral("Prod box"));
    QCOMPARE(saved.value(QStringLiteral("host")).toString(), QStringLiteral("box.local"));
    QCOMPARE(saved.value(QStringLiteral("port")).toInt(), 2222);
    QCOMPARE(saved.value(QStringLiteral("user")).toString(), QStringLiteral("yichen"));
    QCOMPARE(saved.value(QStringLiteral("identityFile")).toString(),
             QStringLiteral("~/.ssh/prod key"));
    QCOMPARE(saved.value(QStringLiteral("nodePath")).toString(),
             QStringLiteral("/home/user name/.local/bin/node"));
    QCOMPARE(saved.value(QStringLiteral("repoRoot")).toString(), QStringLiteral("/srv/my repo"));

    ServerProfiles reopened(path);
    TestApp reopenedApp(&reopened);
    const std::unique_ptr<QQuickView> reopenedView = loadSettings(&reopenedApp);
    QVERIFY(reopenedView != nullptr);
    QQuickItem* const reopenedRoot = reopenedView->rootObject();
    QVERIFY(reopenedRoot != nullptr);
    QTRY_COMPARE(reopenedRoot->property("selectedProfileId").toString(), id);
    QCOMPARE(stringOf(findByName(reopenedRoot, QStringLiteral("serverField:host")), "text"),
             QStringLiteral("box.local"));
    CH_ASSERT_SILENT();
}


void TstServerProfiles::sheetHostKeyPromptDecidesBothWays()
{
    CH_LOAD_CONNECT(view, root);
    QSignalSpy decisionSpy(root, SIGNAL(hostKeyDecision(bool)));
    QObject* const prompt = root->findChild<QObject*>(QStringLiteral("hostKeyPrompt"));
    QVERIFY(prompt);
    QVERIFY(!prompt->property("visible").toBool());

    const QVariantMap pending{
        {QStringLiteral("host"), QStringLiteral("box.local")},
        {QStringLiteral("keyType"), QStringLiteral("ssh-ed25519")},
        {QStringLiteral("fingerprint"),
         QStringLiteral("SHA256:6dGRJ0mCkQeAxQ0nQ0mm2b3xk0e3iF0jnq0oO3pP1qQ")}};
    root->setProperty("pendingHostKey", pending);
    QVERIFY(prompt->property("visible").toBool());
    QObject* const fingerprint = root->findChild<QObject*>(QStringLiteral("hostKeyFingerprint"));
    QObject* const hostLine = root->findChild<QObject*>(QStringLiteral("hostKeyHost"));
    QVERIFY(fingerprint && hostLine);
    QCOMPARE(stringOf(fingerprint, "text"), pending.value(QStringLiteral("fingerprint")).toString());
    QVERIFY(stringOf(hostLine, "text").contains(QStringLiteral("box.local")));
    QVERIFY(stringOf(hostLine, "text").contains(QStringLiteral("ssh-ed25519")));

    QObject* const accept = root->findChild<QObject*>(QStringLiteral("hostKeyAcceptButton"));
    QObject* const reject = root->findChild<QObject*>(QStringLiteral("hostKeyRejectButton"));
    QVERIFY(accept && reject);
    QMetaObject::invokeMethod(accept, "clicked");
    QMetaObject::invokeMethod(reject, "clicked");
    QCOMPARE(decisionSpy.count(), 2);
    QCOMPARE(decisionSpy.at(0).at(0).toBool(), true);
    QCOMPARE(decisionSpy.at(1).at(0).toBool(), false);
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
// credentialSubmitted() — never through the profile editor in SettingsWindow.
void TstServerProfiles::sheetCredentialPromptMasksSubmitsAndKeepsTheSecretOffDisk()
{
    CH_LOAD_CONNECT(view, root);
    const QString secret = QStringLiteral("correct-horse-battery-42");

    QSignalSpy submitSpy(root, SIGNAL(credentialSubmitted(QString,QString)));
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

    // The prompt has no profile-saving path; the only emitted payloads above
    // are the explicit credential answer signals, so this secret cannot enter
    // ServerProfiles through the connector component.
    QVERIFY(root->metaObject()->indexOfSignal("profileSaved(QVariant)") < 0);

    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetSurfacesErrorTextWithoutBlocking()
{
    CH_LOAD_CONNECT(view, root);

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
    CH_LOAD_CONNECT(view, root);
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
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    CH_ASSERT_SILENT();
}

void TstServerProfiles::sheetIsUsableFromTheKeyboardAlone()
{
    CH_LOAD_CONNECT(view, root);

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
    // A source-loaded QQuickView is not guaranteed to activate its window on
    // construction; explicitly focus the list before exercising key handling.
    list->forceActiveFocus();
    QVERIFY(list->hasActiveFocus());
    QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
    QCOMPARE(root->property("selectedId").toString(), QStringLiteral("id-a"));

    // Down moves the selection, and Enter connects the highlighted profile.
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QCoreApplication::sendEvent(list, &down);
    QCOMPARE(list->property("currentIndex").toInt(), 1);
    QCOMPARE(root->property("selectedId").toString(), QStringLiteral("id-b"));

    QSignalSpy connectSpy(root, SIGNAL(connectRequested(QString)));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(list, &enter);
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(connectSpy.at(0).at(0).toString(), QStringLiteral("id-b"));

    // Escape closes the sheet.
    QSignalSpy dismissSpy(root, SIGNAL(dismissed()));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(root, &escape);
    QCOMPARE(dismissSpy.count(), 1);

    // Every interactive control is reachable by Tab.
    const QStringList tabbable = {QStringLiteral("profileList"),
                                  QStringLiteral("connectButton"),
                                  QStringLiteral("openSettingsButton")};
    for (const QString &name : tabbable) {
        QObject *const control = root->findChild<QObject *>(name);
        QVERIFY2(control != nullptr, qPrintable(name));
        QVERIFY2(control->property("activeFocusOnTab").toBool() || name == QStringLiteral("profileList"),
                 qPrintable(name));
    }

    CH_ASSERT_SILENT();
}

// A fresh Settings server pane creates a profile before any network is needed,
// persists each edited field through ServerProfiles, and reloads it selected.
void TstServerProfiles::coldStartAddsAServerThenFindsItAgainAfterRelaunch()
{
    const QString path = iniPath(QStringLiteral("coldstart.ini"));
    QString storedId;
    {
        ServerProfiles store(path);
        QVERIFY(store.profiles().isEmpty());
        TestApp app(&store);
        const std::unique_ptr<QQuickView> view = loadSettings(&app);
        QVERIFY(view != nullptr);
        QQuickItem* const root = view->rootObject();
        QVERIFY(root != nullptr);

        QObject* const add = root->findChild<QObject*>(QStringLiteral("serverAddButton"));
        QVERIFY(add);
        QMetaObject::invokeMethod(add, "clicked");
        QTRY_COMPARE(store.profiles().size(), 1);
        storedId = store.profiles().constFirst().toMap().value(QStringLiteral("id")).toString();
        QVERIFY(!storedId.isEmpty());

        const QList<QPair<QString, QString>> values = {
            {QStringLiteral("serverField:name"), QStringLiteral("Fixture")},
            {QStringLiteral("serverField:host"), QStringLiteral("127.0.0.1")},
            {QStringLiteral("serverField:port"), QStringLiteral("2222")},
            {QStringLiteral("serverField:user"), QStringLiteral("yichen")},
            {QStringLiteral("serverField:nodePath"), QStringLiteral("/home/yichen/.local/bin/node")},
            {QStringLiteral("serverField:repoRoot"),
             QStringLiteral("/home/yichen/projects/codeharbor")}};
        for (const auto& value : values) {
            QObject* const field = findByName(root, value.first);
            QVERIFY(field);
            field->setProperty("text", value.second);
            QVERIFY(QMetaObject::invokeMethod(field, "editingFinished"));
        }

        const QVariantMap stored = store.profile(storedId);
        QCOMPARE(stored.value(QStringLiteral("host")).toString(), QStringLiteral("127.0.0.1"));
        QCOMPARE(stored.value(QStringLiteral("port")).toInt(), 2222);
        QCOMPARE(stored.value(QStringLiteral("user")).toString(), QStringLiteral("yichen"));
        QCOMPARE(store.activeId(), storedId);
        QObject* const connect = root->findChild<QObject*>(QStringLiteral("serverConnectButton"));
        QVERIFY(connect && connect->property("enabled").toBool());
        QMetaObject::invokeMethod(connect, "clicked");
        QCOMPARE(app.connectRequests, QStringList({storedId}));
        CH_ASSERT_SILENT();
    }

    ServerProfiles reopened(path);
    TestApp reopenedApp(&reopened);
    const std::unique_ptr<QQuickView> view = loadSettings(&reopenedApp);
    QVERIFY(view != nullptr);
    QQuickItem* const root = view->rootObject();
    QVERIFY(root != nullptr);
    QTRY_COMPARE(root->property("selectedProfileId").toString(), storedId);
    QCOMPARE(stringOf(findByName(root, QStringLiteral("serverField:host")), "text"),
             QStringLiteral("127.0.0.1"));
    QCOMPARE(stringOf(findByName(root, QStringLiteral("serverField:port")), "text"),
             QStringLiteral("2222"));
    QObject* const deleteButton = root->findChild<QObject*>(QStringLiteral("serverDeleteButton"));
    QObject* const dialog = root->findChild<QObject*>(QStringLiteral("serverDeleteDialog"));
    QVERIFY(deleteButton && dialog);
    QMetaObject::invokeMethod(deleteButton, "clicked");
    QVERIFY(dialog->property("visible").toBool());
    QMetaObject::invokeMethod(dialog, "accept");
    QTRY_VERIFY(reopened.profiles().isEmpty());
    ServerProfiles afterRemoval(path);
    QVERIFY(afterRemoval.profiles().isEmpty());
    CH_ASSERT_SILENT();
}

QTEST_MAIN(TstServerProfiles)
#include "tst_serverprofiles.moc"
