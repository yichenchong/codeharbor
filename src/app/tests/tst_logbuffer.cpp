#include <QtTest/QtTest>

#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QThread>

#include <atomic>
#include <memory>

#include "LogBuffer.h"

using namespace ch;

namespace {

QMutex g_handlerMutex;
int g_chainedWarnings = 0;

void chainedHandler(QtMsgType type, const QMessageLogContext&, const QString&)
{
    if (type == QtWarningMsg) {
        QMutexLocker locker(&g_handlerMutex);
        ++g_chainedWarnings;
    }
}

void quietHandler(QtMsgType, const QMessageLogContext&, const QString&)
{
}

class TstLogBuffer final : public QObject {
    Q_OBJECT

private slots:
    void entryAndByteCaps();
    void previousHandlerStillReceivesWarnings();
    void severityAndTextFilters();
    void remoteTextIsSplitPerLineAndClearAnnouncesItselfOnce();
    void zeroCapacityBufferKeepsNothing();
    void destructionRacesWithLogging();
};

void TstLogBuffer::entryAndByteCaps()
{
    LogBuffer buffer(3, 4096);
    for (int i = 0; i < 4; ++i) {
        buffer.append(QtInfoMsg, QStringLiteral("test"), QStringLiteral("unit"),
                      QStringLiteral("entry-%1").arg(i));
    }

    QCOMPARE(buffer.count(), 3);
    QVERIFY(buffer.totalBytes() <= buffer.maxBytes());
    const QVariantList rows = buffer.entries();
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.constFirst().toMap().value(QStringLiteral("message")).toString(),
             QStringLiteral("entry-1"));
    QCOMPARE(rows.constLast().toMap().value(QStringLiteral("message")).toString(),
             QStringLiteral("entry-3"));

    LogBuffer byteBound(8, 160);
    byteBound.append(QtInfoMsg, QStringLiteral("test"), QStringLiteral("unit"),
                     QString(1000, QLatin1Char('x')));
    byteBound.append(QtInfoMsg, QStringLiteral("test"), QStringLiteral("unit"),
                     QStringLiteral("newest"));
    QVERIFY(byteBound.totalBytes() <= byteBound.maxBytes());
    QVERIFY(byteBound.count() <= byteBound.maxEntries());
    QVERIFY(byteBound.entries().constLast().toMap().value(QStringLiteral("message"))
                .toString()
                .contains(QStringLiteral("newest")));
    const QString rocket = QString::fromUtf8("\xF0\x9F\x9A\x80");
    for (int maxBytes = 200; maxBytes < 210; ++maxBytes) {
        LogBuffer utf8(1, maxBytes);
        utf8.append(QtInfoMsg, QStringLiteral("test"), QStringLiteral("unit"),
                    rocket.repeated(100));
        QCOMPARE(utf8.count(), 1);
        QVERIFY(utf8.totalBytes() <= maxBytes);
        const QString message =
            utf8.entries().constFirst().toMap().value(QStringLiteral("message"))
                .toString();
        QVERIFY2(!message.contains(QChar::ReplacementCharacter),
                 "UTF-8 truncation must not create replacement characters");
    }
}


void TstLogBuffer::previousHandlerStillReceivesWarnings()
{
    g_chainedWarnings = 0;
    const QtMessageHandler previous = qInstallMessageHandler(&chainedHandler);
    {
        LogBuffer buffer(8, 4096);
        qWarning("log-buffer chaining marker");
        QCOMPARE(buffer.count(), 1);
    }
    qInstallMessageHandler(previous);

    QMutexLocker locker(&g_handlerMutex);
    QCOMPARE(g_chainedWarnings, 1);
}

void TstLogBuffer::severityAndTextFilters()
{
    LogBuffer buffer;
    buffer.append(QtDebugMsg, QStringLiteral("client"), QStringLiteral("unit"),
                  QStringLiteral("local trace"));
    buffer.append(QtWarningMsg, QStringLiteral("ssh"), QStringLiteral("pool"),
                  QStringLiteral("remote refused host"), QStringLiteral("ssh"));
    buffer.append(QtCriticalMsg, QStringLiteral("daemon"), QStringLiteral("rpc"),
                  QStringLiteral("remote crashed"), QStringLiteral("daemon"));

    const QVariantList warnings =
        buffer.filteredEntries(QStringLiteral("warning"), QStringLiteral("REFUSED"));
    QCOMPARE(warnings.size(), 1);
    const QVariantMap warning = warnings.constFirst().toMap();
    QCOMPARE(warning.value(QStringLiteral("severity")).toString(),
             QStringLiteral("warning"));
    QCOMPARE(warning.value(QStringLiteral("origin")).toString(), QStringLiteral("ssh"));

    const QVariantList remote =
        buffer.filteredEntries(QString(), QStringLiteral("remote"));
    QCOMPARE(remote.size(), 2);
    // "all" is the log pane's own sentinel for "no severity filter" and must
    // behave exactly like an empty string, including when a text filter is
    // combined with it.
    QCOMPARE(buffer.filteredEntries(QStringLiteral("all")).size(), 3);
    QCOMPARE(buffer.filteredEntries(QStringLiteral("ALL"),
                                    QStringLiteral("remote"))
                 .size(),
             2);
    QCOMPARE(buffer.filteredEntries(QString(), QString()).size(), 3);
    // A severity nothing carries hides everything rather than showing all.
    QVERIFY(buffer.filteredEntries(QStringLiteral("fatal")).isEmpty());

    // Filtering preserves the buffer's oldest-to-newest order, which is what
    // makes following the tail useful.
    QCOMPARE(remote.at(0).toMap().value(QStringLiteral("message")).toString(),
             QStringLiteral("remote refused host"));
    QCOMPARE(remote.at(1).toMap().value(QStringLiteral("message")).toString(),
             QStringLiteral("remote crashed"));
}

// Both caps are enforced BEFORE an entry is published, so a buffer configured
// with no room at all must publish nothing and wake nobody - not push a row
// and trim it afterwards. A negative configuration is clamped to zero, not
// treated as unbounded.
void TstLogBuffer::zeroCapacityBufferKeepsNothing()
{
    LogBuffer noEntries(0, 4096);
    QSignalSpy noEntriesChanged(&noEntries, &LogBuffer::entriesChanged);
    noEntries.append(QtWarningMsg, QStringLiteral("test"),
                     QStringLiteral("unit"), QStringLiteral("dropped"));
    QCOMPARE(noEntries.count(), 0);
    QCOMPARE(noEntries.totalBytes(), 0);
    QCOMPARE(noEntriesChanged.count(), 0);

    // The fixed fields (timestamp, severity and the separators) alone exceed a
    // byte budget this small, so there is no honest way to keep the row.
    LogBuffer noBytes(8, 4);
    QSignalSpy noBytesChanged(&noBytes, &LogBuffer::entriesChanged);
    noBytes.append(QtWarningMsg, QStringLiteral("test"), QStringLiteral("unit"),
                   QStringLiteral("dropped"));
    QCOMPARE(noBytes.count(), 0);
    QCOMPARE(noBytesChanged.count(), 0);

    LogBuffer negative(-3, -3);
    QCOMPARE(negative.maxEntries(), 0);
    QCOMPARE(negative.maxBytes(), 0);
    negative.append(QtWarningMsg, QStringLiteral("test"),
                    QStringLiteral("unit"), QStringLiteral("dropped"));
    QCOMPARE(negative.count(), 0);
}

// A daemon reports stderr as a stream, so one signal can carry a whole startup
// report. Each line has to become its own row (otherwise a long report hides
// the useful tail and neither filter can reach into it), blank noise has to
// vanish, and clear() has to announce itself exactly when something actually
// went away.
void TstLogBuffer::remoteTextIsSplitPerLineAndClearAnnouncesItselfOnce()
{
    LogBuffer buffer(16, 8192);
    QSignalSpy changed(&buffer, &LogBuffer::entriesChanged);

    buffer.appendRemote(QStringLiteral("daemon"), QStringLiteral("ssh.channel"),
                        QStringLiteral("rpc"),
                        QStringLiteral("first\n\n  second  \n"));
    QCOMPARE(buffer.count(), 2);
    QCOMPARE(changed.count(), 2);

    const QVariantList rows = buffer.entries();
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("message")).toString(),
             QStringLiteral("first"));
    // Trimmed, so the daemon's own indentation does not defeat a text filter.
    QCOMPARE(rows.at(1).toMap().value(QStringLiteral("message")).toString(),
             QStringLiteral("second"));
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("origin")).toString(),
             QStringLiteral("daemon"));
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("category")).toString(),
             QStringLiteral("ssh.channel"));

    // Whitespace-only output is not a log line and must not become a row - nor
    // wake every binding on the log pane.
    buffer.appendRemote(QStringLiteral("daemon"), QStringLiteral("ssh.channel"),
                        QStringLiteral("rpc"), QStringLiteral("   \n \n"));
    QCOMPARE(buffer.count(), 2);
    QCOMPARE(changed.count(), 2);

    buffer.clear();
    QCOMPARE(buffer.count(), 0);
    QCOMPARE(buffer.totalBytes(), 0);
    QCOMPARE(changed.count(), 3);
    // Clearing an empty buffer changed nothing, so it says nothing.
    buffer.clear();
    QCOMPARE(changed.count(), 3);
}

void TstLogBuffer::destructionRacesWithLogging()
{
    // Silence the fallback stderr path: this test intentionally emits many
    // messages while the QObject is being torn down.
    const QtMessageHandler previous = qInstallMessageHandler(&quietHandler);
    auto buffer = std::make_unique<LogBuffer>(32, 8192);
    std::atomic<bool> running{true};
    QThread* writer = QThread::create([&running] {
        while (running.load(std::memory_order_relaxed))
            qWarning("concurrent log-buffer marker");
    });
    writer->start();
    QTest::qWait(20);
    buffer.reset();
    running.store(false, std::memory_order_relaxed);
    QVERIFY(writer->wait(3000));
    delete writer;
    qInstallMessageHandler(previous);
}

} // namespace

// Guiless: nothing here needs a display, and ch_app links Qt6::Gui, so
// QTEST_MAIN would build a QGuiApplication that fails on a headless runner.
QTEST_GUILESS_MAIN(TstLogBuffer)
#include "tst_logbuffer.moc"
