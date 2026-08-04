#include "LogBuffer.h"

#include <QMutexLocker>
#include <QVariantMap>
#include <QWaitCondition>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ch {
struct LogBuffer::HandlerLease {
    QMutex mutex;
    QWaitCondition drained;
    LogBuffer* buffer = nullptr;
    int activeCalls = 0;
    bool closing = false;

    bool acquire(LogBuffer** result)
    {
        QMutexLocker locker(&mutex);
        if (closing || !buffer)
            return false;
        ++activeCalls;
        *result = buffer;
        return true;
    }

    void release()
    {
        QMutexLocker locker(&mutex);
        --activeCalls;
        if (closing && activeCalls == 0)
            drained.wakeAll();
    }

    void closeAndWait()
    {
        QMutexLocker locker(&mutex);
        closing = true;
        buffer = nullptr;
        while (activeCalls != 0)
            drained.wait(&mutex);
    }
};

QMutex LogBuffer::s_handlerMutex;
QVector<std::shared_ptr<LogBuffer::HandlerLease>> LogBuffer::s_handlerBuffers;
QtMessageHandler LogBuffer::s_previousHandler = nullptr;

namespace {
QString messageSource(const QMessageLogContext& context)
{
    QString source;
    if (context.file && *context.file) {
        source = QString::fromUtf8(context.file);
        if (context.line > 0)
            source += QLatin1Char(':') + QString::number(context.line);
    }
    if (context.function && *context.function) {
        if (!source.isEmpty())
            source += QStringLiteral(" ");
        source += QString::fromUtf8(context.function);
    }
    return source;
}

int severityValue(QtMsgType severity)
{
    switch (severity) {
    case QtDebugMsg:
        return 0;
    case QtInfoMsg:
        return 1;
    case QtWarningMsg:
        return 2;
    case QtCriticalMsg:
        return 3;
    case QtFatalMsg:
        return 4;
    }
    return 1;
}

} // namespace

LogBuffer::LogBuffer(QObject* parent)
    : LogBuffer(kDefaultMaxEntries, kDefaultMaxBytes, parent)
{
}

LogBuffer::LogBuffer(int maxEntries, int maxBytes, QObject* parent)
    : QObject(parent)
    , m_handlerLease(std::make_shared<HandlerLease>())
    , m_maxEntries(std::max(0, maxEntries))
    , m_maxBytes(std::max(0, maxBytes))
{
    if (m_maxEntries > 0)
        m_entries.reserve(m_maxEntries);
    m_handlerLease->buffer = this;

    // Qt exposes one process-wide handler. Keep a stack of shared leases rather
    // than raw QObject pointers. The message thread takes a strong lease before
    // dropping the registry mutex; destruction marks that lease closed and waits
    // only for already-running deliveries, so the pointer cannot dangle and no
    // Qt signal is emitted while either lifetime lock is held.
    QMutexLocker locker(&s_handlerMutex);
    if (s_handlerBuffers.isEmpty())
        s_previousHandler = qInstallMessageHandler(&LogBuffer::messageHandler);
    s_handlerBuffers.push_back(m_handlerLease);
}

LogBuffer::~LogBuffer()
{
    const std::shared_ptr<HandlerLease> lease = m_handlerLease;
    bool wasLast = false;
    {
        QMutexLocker locker(&s_handlerMutex);
        s_handlerBuffers.removeAll(lease);
        wasLast = s_handlerBuffers.isEmpty();
    }

    // A handler that already acquired `lease` may still be using this QObject.
    // Wait outside s_handlerMutex so a concurrent handler can finish and so a
    // new buffer can register without a lock-order deadlock.
    lease->closeAndWait();
    if (!wasLast)
        return;

    QMutexLocker locker(&s_handlerMutex);
    if (!s_handlerBuffers.isEmpty())
        return;

    // qInstallMessageHandler() is also the only way to inspect the current
    // handler. A test is allowed to install its own handler after us, so do not
    // overwrite it: temporarily put back the handler that preceded us, then
    // restore the test handler when it was not ours.
    const QtMessageHandler previous = s_previousHandler;
    const QtMessageHandler current = qInstallMessageHandler(previous);
    if (current != &LogBuffer::messageHandler)
        qInstallMessageHandler(current);
    s_previousHandler = nullptr;
}

QString LogBuffer::severityName(QtMsgType severity)
{
    switch (severity) {
    case QtDebugMsg:
        return QStringLiteral("debug");
    case QtInfoMsg:
        return QStringLiteral("info");
    case QtWarningMsg:
        return QStringLiteral("warning");
    case QtCriticalMsg:
        return QStringLiteral("critical");
    case QtFatalMsg:
        return QStringLiteral("fatal");
    }
    return QStringLiteral("info");
}

QVariantList LogBuffer::entries() const
{
    QMutexLocker locker(&m_mutex);
    QVariantList result;
    result.reserve(m_entries.size());
    for (const StoredEntry& stored : m_entries)
        result.push_back(toMap(stored.value));
    return result;
}

int LogBuffer::count() const
{
    QMutexLocker locker(&m_mutex);
    return m_entries.size();
}

int LogBuffer::totalBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_totalBytes;
}

void LogBuffer::append(QtMsgType severity, const QString& category,
                       const QString& source, const QString& message,
                       const QString& origin)
{
    Entry entry;
    entry.timestamp = QDateTime::currentDateTimeUtc();
    entry.severity = severity;
    entry.origin = origin.trimmed();
    entry.category = category.trimmed();
    entry.source = source.trimmed();
    entry.message = message;
    appendEntry(std::move(entry));
}

void LogBuffer::appendRemote(const QString& origin, const QString& category,
                             const QString& source, const QString& text,
                             QtMsgType severity)
{
    // A daemon reports stderr as a stream, and a single signal may contain a
    // complete startup report. Splitting here keeps each row independently
    // filterable and prevents one long report from hiding the useful tail.
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            append(severity, category, source, trimmed, origin);
    }
}

QVariantList LogBuffer::filteredEntries(const QString& severityFilter,
                                        const QString& textFilter) const
{
    const QString wantedSeverity = severityFilter.trimmed().toLower();
    const QString wantedText = textFilter.trimmed();
    const QVariantList all = entries();
    if (wantedSeverity.isEmpty() || wantedSeverity == QLatin1String("all")) {
        if (wantedText.isEmpty())
            return all;
    }

    QVariantList result;
    result.reserve(all.size());
    for (const QVariant& item : all) {
        const QVariantMap map = item.toMap();
        if (!wantedSeverity.isEmpty() && wantedSeverity != QLatin1String("all")
            && map.value(QStringLiteral("severity")).toString().toLower()
                   != wantedSeverity) {
            continue;
        }
        if (!wantedText.isEmpty()
            && !map.value(QStringLiteral("text")).toString().contains(
                wantedText, Qt::CaseInsensitive)) {
            continue;
        }
        result.push_back(map);
    }
    return result;
}

void LogBuffer::clear()
{
    bool changed = false;
    {
        QMutexLocker locker(&m_mutex);
        changed = !m_entries.isEmpty();
        m_entries.clear();
        m_totalBytes = 0;
    }
    if (changed)
        emit entriesChanged();
}

void LogBuffer::messageHandler(QtMsgType type,
                               const QMessageLogContext& context,
                               const QString& message)
{
    std::shared_ptr<HandlerLease> lease;
    QtMessageHandler previous = nullptr;
    {
        QMutexLocker locker(&s_handlerMutex);
        if (!s_handlerBuffers.isEmpty())
            lease = s_handlerBuffers.constLast();
        previous = s_previousHandler;
    }

    // The shared lease closes the gap between looking up a handler and calling
    // it. Destructor waits for this active call after removing the lease from
    // the registry; it never relies on a raw pointer surviving an unlock.
    LogBuffer* buffer = nullptr;
    if (lease && lease->acquire(&buffer)) {
        buffer->appendQtMessage(type, context, message);
        lease->release();
    }

    // Chaining is not optional: qInstallMessageHandler() replaces the existing
    // sink, and tests (and users' launchers) rely on that sink continuing to
    // receive warnings. Fatal messages go through the old sink before Qt's own
    // fatal handling runs, so the diagnostics window never changes termination
    // semantics or silently turns a fatal into an ordinary row.
    if (previous && previous != &LogBuffer::messageHandler) {
        previous(type, context, message);
        return;
    }

    // Qt represents its built-in stderr handler as a null function pointer.
    // Reproduce the small part we need when no caller supplied a handler; this
    // keeps ordinary warnings visible in a terminal and preserves fatal exit.
    const QByteArray rendered = qFormatLogMessage(type, context, message).toLocal8Bit();
    std::fputs(rendered.constData(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    if (type == QtFatalMsg)
        std::abort();
}

void LogBuffer::appendQtMessage(QtMsgType type,
                                const QMessageLogContext& context,
                                const QString& message)
{
    // Credential values never enter this method: AppController's password and
    // key-passphrase callbacks deliberately emit no Qt message, and the SSH
    // router/daemon streams carry only protocol diagnostics, never the supplied
    // secret. We therefore capture metadata and the message Qt itself emitted,
    // but never QML credential fields or callback arguments.
    const QString category = context.category ? QString::fromUtf8(context.category)
                                              : QString();
    append(type, category, messageSource(context), message,
           QStringLiteral("client"));
}

void LogBuffer::appendEntry(Entry entry)
{
    // Keep the timestamp and severity for every retained row. If metadata fills
    // the entire byte budget, the message is truncated rather than violating
    // the hard cap; if even the fixed fields cannot fit, dropping that one row
    // is the only honest bounded behaviour.
    const int fixedBytes = entryBytes(Entry{entry.timestamp, entry.severity,
                                            QString(), QString(), QString(),
                                            QString()});
    if (fixedBytes > m_maxBytes || m_maxEntries == 0)
        return;

    int remaining = m_maxBytes - fixedBytes;
    const auto fit = [&remaining](QString& value) {
        value = truncateUtf8(value, remaining);
        remaining -= value.toUtf8().size();
    };
    fit(entry.origin);
    fit(entry.category);
    fit(entry.source);
    fit(entry.message);

    const int bytes = entryBytes(entry);
    if (bytes > m_maxBytes)
        return;

    bool accepted = false;
    {
        QMutexLocker locker(&m_mutex);
        while (!m_entries.isEmpty()
               && (m_entries.size() >= m_maxEntries
                   || m_totalBytes + bytes > m_maxBytes)) {
            m_totalBytes -= m_entries.constFirst().bytes;
            m_entries.removeFirst();
        }
        if (m_entries.size() < m_maxEntries && m_totalBytes + bytes <= m_maxBytes) {
            m_entries.push_back(StoredEntry{std::move(entry), bytes});
            m_totalBytes += bytes;
            accepted = true;
        }
    }
    if (accepted)
        emit entriesChanged();
}

QString LogBuffer::truncateUtf8(const QString& value, qsizetype byteLimit)
{
    if (byteLimit <= 0)
        return QString();
    const QByteArray encoded = value.toUtf8();
    if (encoded.size() <= byteLimit)
        return value;

    // Do not hand QString::fromUtf8() a prefix ending halfway through a
    // multi-byte sequence: Qt replaces that incomplete sequence with U+FFFD,
    // whose three bytes can put the result back over the hard byte cap. Trim
    // the incomplete lead byte and its continuation bytes instead.
    qsizetype end = byteLimit;
    const auto isContinuation = [](unsigned char byte) {
        return (byte & 0xc0u) == 0x80u;
    };
    const auto sequenceLength = [](unsigned char lead) -> qsizetype {
        if (lead < 0x80u)
            return 1;
        if ((lead & 0xe0u) == 0xc0u)
            return 2;
        if ((lead & 0xf0u) == 0xe0u)
            return 3;
        if ((lead & 0xf8u) == 0xf0u)
            return 4;
        return 0;
    };

    qsizetype start = end;
    while (start > 0
           && isContinuation(static_cast<unsigned char>(encoded.at(start - 1)))) {
        --start;
    }
    if (start < end) {
        const unsigned char lead =
            static_cast<unsigned char>(encoded.at(start - 1));
        const qsizetype expected = sequenceLength(lead);
        if (expected == 0 || expected > end - (start - 1))
            end = start - 1;
    } else if (end > 0) {
        const unsigned char lead =
            static_cast<unsigned char>(encoded.at(end - 1));
        const qsizetype expected = sequenceLength(lead);
        if (expected > 1)
            end = end - 1;
    }
    return QString::fromUtf8(encoded.constData(), static_cast<int>(end));
}

int LogBuffer::entryBytes(const Entry& entry)
{
    const auto bytes = [](const QString& value) { return value.toUtf8().size(); };
    // Six separators are reserved so the byte count also covers the shape of a
    // serialized row used by the QML copy action. The exact delimiter is not a
    // public format; it is simply a conservative accounting unit.
    return bytes(entry.timestamp.toString(Qt::ISODateWithMs))
        + bytes(severityName(entry.severity)) + bytes(entry.origin)
        + bytes(entry.category) + bytes(entry.source) + bytes(entry.message) + 6;
}

QVariantMap LogBuffer::toMap(const Entry& entry)
{
    QVariantMap map;
    map.insert(QStringLiteral("timestamp"), entry.timestamp.toString(Qt::ISODateWithMs));
    map.insert(QStringLiteral("severity"), severityName(entry.severity));
    map.insert(QStringLiteral("severityValue"), severityValue(entry.severity));
    map.insert(QStringLiteral("origin"), entry.origin);
    map.insert(QStringLiteral("category"), entry.category);
    map.insert(QStringLiteral("source"), entry.source);
    map.insert(QStringLiteral("message"), entry.message);
    map.insert(QStringLiteral("text"), displayLine(entry));
    return map;
}

QString LogBuffer::displayLine(const Entry& entry)
{
    const QString origin = entry.origin.isEmpty() ? QStringLiteral("client")
                                                   : entry.origin;
    QString labels = QStringLiteral("[%1]").arg(origin);
    if (!entry.category.isEmpty())
        labels += QStringLiteral(" [%1]").arg(entry.category);
    if (!entry.source.isEmpty())
        labels += QStringLiteral(" %1").arg(entry.source);
    return QStringLiteral("%1 [%2] %3: %4")
        .arg(entry.timestamp.toString(Qt::ISODateWithMs), severityName(entry.severity),
             labels, entry.message);
}

} // namespace ch
