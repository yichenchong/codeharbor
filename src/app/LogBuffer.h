#pragma once

#include <QDateTime>
#include <QLoggingCategory>
#include <QMutex>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <memory>

namespace ch {

// In-memory diagnostics shared by the client shell and its remote connection
// spine. The buffer intentionally owns no file or socket: keeping it transient
// means a password, passphrase or workspace content cannot be recovered from a
// log file after the process exits. The two limits are enforced before an entry
// is published, so even a noisy server cannot turn debugging into unbounded
// client memory use.
class LogBuffer final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(int count READ count NOTIFY entriesChanged)
    Q_PROPERTY(int totalBytes READ totalBytes NOTIFY entriesChanged)
    Q_PROPERTY(int maxEntries READ maxEntries CONSTANT)
    Q_PROPERTY(int maxBytes READ maxBytes CONSTANT)

public:
    static constexpr int kDefaultMaxEntries = 512;
    static constexpr int kDefaultMaxBytes = 256 * 1024;

    struct Entry {
        QDateTime timestamp;
        QtMsgType severity = QtInfoMsg;
        QString origin;
        QString category;
        QString source;
        QString message;
    };

    explicit LogBuffer(QObject* parent = nullptr);
    LogBuffer(int maxEntries, int maxBytes, QObject* parent = nullptr);
    ~LogBuffer() override;

    QVariantList entries() const;
    int count() const;
    int totalBytes() const;
    int maxEntries() const { return m_maxEntries; }
    int maxBytes() const { return m_maxBytes; }

    // Add one application message. `source` is deliberately separate from the
    // Qt logging category: the category is the logger's own label, while source
    // identifies the file/function that produced it.
    void append(QtMsgType severity, const QString& category,
                const QString& source, const QString& message,
                const QString& origin = QStringLiteral("client"));

    // Add remote output without routing it through Qt's global message handler.
    // Remote channels may carry several lines in one signal; each non-empty line
    // gets its own entry so following the tail remains useful.
    void appendRemote(const QString& origin, const QString& category,
                      const QString& source, const QString& text,
                      QtMsgType severity = QtInfoMsg);

    // The QML log pane calls this after changing either filter. Matching is
    // case-insensitive and preserves the buffer's oldest-to-newest order.
    Q_INVOKABLE QVariantList filteredEntries(
        const QString& severityFilter = QString(),
        const QString& textFilter = QString()) const;

    Q_INVOKABLE void clear();

    static QString severityName(QtMsgType severity);

signals:
    void entriesChanged();

private:
    struct StoredEntry {
        Entry value;
        int bytes = 0;
    };

    static void messageHandler(QtMsgType type, const QMessageLogContext& context,
                               const QString& message);
    void appendQtMessage(QtMsgType type, const QMessageLogContext& context,
                         const QString& message);
    void appendEntry(Entry entry);

    static QString truncateUtf8(const QString& value, qsizetype byteLimit);
    static int entryBytes(const Entry& entry);
    static QVariantMap toMap(const Entry& entry);
    static QString displayLine(const Entry& entry);

    struct HandlerLease;
    static QMutex s_handlerMutex;
    static QVector<std::shared_ptr<HandlerLease>> s_handlerBuffers;
    static QtMessageHandler s_previousHandler;

    std::shared_ptr<HandlerLease> m_handlerLease;
    mutable QMutex m_mutex;
    QVector<StoredEntry> m_entries;
    int m_totalBytes = 0;
    const int m_maxEntries;
    const int m_maxBytes;
};

} // namespace ch
