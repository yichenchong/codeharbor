#pragma once

// The shared harness for the mobile navigation gates: a fake codeharbord over an
// in-process QIODevice, plus the object graph src/mobile/main.cpp assembles.
//
// It lives in a header rather than inside one test file because TWO gates drive
// exactly the same walk from different heights — tst_mobilenav asserts on
// ch::MobileAppController's state machine, tst_mobileshell asserts on what the
// real QML shell does with it — and a second copy of the fake server is how the
// two would drift into testing different servers while claiming to test one.

#include <QtTest/QtTest>

#include <QByteArray>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstring>

#include "AppController.h"
#include "CodeharbordClient.h"
#include "MobileAppController.h"
#include "PaneListModel.h"
#include "RpcTypes.h"
#include "SessionLayouts.h"
#include "SshConnectionPool.h"
#include "TerminalFactory.h"
#include "UiStateStore.h"
#include "WorkspaceDb.h"

namespace chtest {

// Minimal in-process QIODevice standing in for the RPC transport, the same
// technique src/app/tests/tst_appcontroller.cpp uses and for the same reasons: it
// captures the client's writes verbatim and injects server->client frames
// SYNCHRONOUSLY, so a case controls response ordering exactly and needs no event
// loop, no SSH server, no fixture and no display.
class FakeTransport : public QIODevice {
public:
    explicit FakeTransport(QObject* parent = nullptr) : QIODevice(parent)
    {
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return m_incoming.size() + QIODevice::bytesAvailable();
    }

    void deliver(const QByteArray& frame)
    {
        m_incoming.append(frame);
        emit readyRead();
    }

    QByteArray takeSent()
    {
        const QByteArray sent = m_sent;
        m_sent.clear();
        return sent;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_incoming.size());
        if (n > 0) {
            std::memcpy(data, m_incoming.constData(), static_cast<size_t>(n));
            m_incoming.remove(0, n);
        }
        return n;
    }

    qint64 writeData(const char* data, qint64 len) override
    {
        m_sent.append(data, len);
        return len;
    }

private:
    QByteArray m_incoming;
    QByteArray m_sent;
};

// Every request written since the last drain, EXCEPT the background pane-activity
// poll: ch::TmuxActivityPoller rides the same refresh these cases drive, and a
// case whose claim is "this tap produced these two getLayout calls" has to say so
// about those and not about unrelated housekeeping.
inline QVector<QJsonObject> takeRequests(FakeTransport& transport)
{
    QVector<QJsonObject> requests;
    const QList<QByteArray> lines = transport.takeSent().split('\n');
    for (const QByteArray& line : lines) {
        if (line.isEmpty())
            continue;
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (request.value(QStringLiteral("method")).toString()
            == QLatin1String(ch::rpc::kMethodPaneActivity)) {
            continue;
        }
        requests.push_back(request);
    }
    return requests;
}

inline QByteArray frame(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

inline QByteArray resultFrame(int id, const QJsonValue& result)
{
    return frame({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

// A JSON-RPC failure for one request. The layout cases use it to make a region's
// getLayout FAIL, which is a different outcome from an empty tree: the region is
// invalidated and the reason is reported through SessionLayouts::error().
inline QByteArray errorFrame(int id, const QString& message)
{
    return frame({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"error", QJsonObject{{"code", -32000},
                                        {"message", message}}}});
}

// A workspace.list answer: one group holding `sessionIds`, each with a
// repositoryRoot so activeSessionRepoRoot has something to report.
inline QByteArray listFrame(int id, const QStringList& sessionIds)
{
    QJsonArray sessions;
    for (const QString& sessionId : sessionIds) {
        sessions.append(QJsonObject{
            {"id", sessionId},
            {"name", sessionId},
            {"repositoryRoot", QStringLiteral("/srv/") + sessionId}});
    }
    const QJsonObject group{{"id", "g"}, {"name", "g"}, {"sessions", sessions}};
    return resultFrame(id, QJsonArray{group});
}

// A workspace.getLayout answer. Only "tree" is read by the client.
inline QByteArray layoutFrame(int id, const QJsonObject& tree)
{
    return resultFrame(id, QJsonObject{{"id", "layout-1"}, {"tree", tree}});
}

inline QJsonObject viewerTreeFor(const QString& paneId, const QString& url)
{
    return QJsonObject{{"type", "leaf"}, {"paneId", paneId}, {"url", url}};
}

// A terminal leaf already bound to its server-minted `terminal_panes` row, so
// loading it mints nothing and the socket stays quiet.
inline QJsonObject terminalTreeFor(const QString& paneId, const QString& rowId)
{
    return QJsonObject{
        {"type", "leaf"}, {"paneId", paneId}, {"terminalPaneId", rowId}};
}

// The controller, the layouts and the mobile navigation over one fake server, in
// exactly the shape src/mobile/main.cpp assembles them: ONE SessionLayouts, wired
// into AppController and watched by MobileAppController, so activateSession()'s
// load is the load the navigation is waiting for.
struct Fixture {
    FakeTransport transport;
    ch::CodeharbordClient client;
    ch::AppController controller{&client};
    ch::SessionLayouts layouts{controller.workspaceDb(), controller.uiState()};
    // A REAL pool and factory, both constructed without connecting: the factory
    // needs a pool to open a PTY channel on, and nothing here ever opens one. It
    // is wired in so ch::MobileAppController::createTerminalSession() takes its
    // production path — without a factory it returns nullptr, and a terminal
    // pane would silently be a pane with no terminal in it.
    ch::SshConnectionPool pool;
    ch::TerminalFactory terminalFactory{&pool};
    ch::MobileAppController mobile{&controller, &layouts, &pool};

    Fixture()
    {
        controller.setTerminalFactory(&terminalFactory);
        mobile.setTerminalFactory(&terminalFactory);
        controller.setConnection(nullptr, nullptr, nullptr, &layouts);
        // Before the transport, so the setter's own refresh() is the documented
        // silent no-op instead of a request the cases would have to drain.
        controller.setServerId(QStringLiteral("srv"));
        layouts.setServerId(QStringLiteral("srv"));
        // Hermetic: AppController's UiStateStore is the REAL per-user QSettings
        // (redirected process-wide by initTestCase), so a previous run must not
        // leave a remembered session or pane behind.
        controller.uiState()->setActiveSession(QStringLiteral("srv"), QString());
        controller.uiState()->setSelectedPane(QStringLiteral("s1"), QString());
        controller.uiState()->setSelectedPane(QStringLiteral("s2"), QString());
        client.setTransport(&transport);
        transport.takeSent();
    }

    // Answer one workspace.list with the given Dev Sessions, so activateSession()
    // has an authoritative tree to validate ids against.
    void listSessions(const QStringList& sessionIds)
    {
        controller.refresh();
        const QVector<QJsonObject> requests = takeRequests(transport);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.at(0).value(QStringLiteral("method")).toString(),
                 QStringLiteral("workspace.list"));
        transport.deliver(
            listFrame(requests.at(0).value(QStringLiteral("id")).toInt(),
                      sessionIds));
    }

    // The two getLayout requests one load issues, viewer first. Returned rather
    // than answered, so a case can hold them and answer out of order (or not at
    // all) to drive the stale-load path.
    QVector<QJsonObject> takeLayoutRequests()
    {
        QVector<QJsonObject> layoutRequests;
        for (const QJsonObject& request : takeRequests(transport)) {
            if (request.value(QStringLiteral("method")).toString()
                == QLatin1String("workspace.getLayout")) {
                layoutRequests.push_back(request);
            }
        }
        return layoutRequests;
    }

    void answerLayouts(const QVector<QJsonObject>& requests,
                       const QJsonObject& viewerTree,
                       const QJsonObject& terminalTree)
    {
        QCOMPARE(requests.size(), 2);
        transport.deliver(
            layoutFrame(requests.at(0).value(QStringLiteral("id")).toInt(),
                        viewerTree));
        transport.deliver(
            layoutFrame(requests.at(1).value(QStringLiteral("id")).toInt(),
                        terminalTree));
    }

    // Select `sessionId` and answer its layout with one viewer pane and one
    // terminal pane, leaving the navigation at the pane picker.
    void openSession(const QString& sessionId, const QString& viewerPaneId,
                     const QString& terminalPaneId)
    {
        mobile.selectSession(sessionId);
        answerLayouts(takeLayoutRequests(),
                      viewerTreeFor(viewerPaneId,
                                    QStringLiteral("file:///srv/%1/notes.md")
                                        .arg(sessionId)),
                      terminalTreeFor(terminalPaneId,
                                      QStringLiteral("row-") + terminalPaneId));
        // Anything the load wrote back (a normalisation, a title) is not this
        // fixture's business; the cases that care assert on it themselves.
        transport.takeSent();
    }
};

inline QStringList paneKeys(const ch::PaneListModel* model)
{
    QStringList keys;
    for (int row = 0; row < model->rowCount(); ++row) {
        keys << model->data(model->index(row, 0),
                            ch::PaneListModel::PaneKeyRole).toString();
    }
    return keys;
}

}  // namespace chtest
