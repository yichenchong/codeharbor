#include "KnownHosts.h"

#include <QRegularExpression>
#include <QStringList>

namespace ch {

KnownHosts::Verdict KnownHosts::verify(const QString& host,
                                       const QString& keyType,
                                       const QByteArray& keyBlob) const
{
    bool sawType = false;
    for (const Entry& e : m_entries) {
        if (!e.supported || e.host != host || e.keyType != keyType)
            continue;
        sawType = true;
        if (e.key == keyBlob)
            return Verdict::Match;
    }
    // Same host+keyType seen but no blob matched: the key changed -> refuse.
    return sawType ? Verdict::Mismatch : Verdict::Unknown;
}

void KnownHosts::add(const QString& host, const QString& keyType,
                     const QByteArray& keyBlob)
{
    for (Entry& e : m_entries) {
        if (e.supported && e.host == host && e.keyType == keyType) {
            e.key = keyBlob;
            return;
        }
    }
    m_entries.append(Entry{host, keyType, keyBlob, true, QString()});
}

QByteArray KnownHosts::serialize() const
{
    QByteArray out;
    for (const Entry& e : m_entries) {
        out += e.host.toUtf8();
        out += ' ';
        out += e.keyType.toUtf8();
        out += ' ';
        out += e.key.toBase64();
        if (!e.comment.isEmpty()) {
            out += ' ';
            out += e.comment.toUtf8();
        }
        out += '\n';
    }
    return out;
}

KnownHosts KnownHosts::parse(const QString& text)
{
    KnownHosts store;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        QStringList fields =
            line.split(QRegularExpression(QStringLiteral("\\s+")),
                       Qt::SkipEmptyParts);
        int idx = 0;
        // Skip an optional leading marker (@cert-authority, @revoked, ...).
        if (idx < fields.size() && fields.at(idx).startsWith(QLatin1Char('@')))
            ++idx;
        if (fields.size() - idx < 3)
            continue;

        const QString hostField = fields.at(idx);
        const QString keyType = fields.at(idx + 1);
        const QByteArray key =
            QByteArray::fromBase64(fields.at(idx + 2).toUtf8());
        QString comment;
        for (int i = idx + 3; i < fields.size(); ++i) {
            if (!comment.isEmpty())
                comment += QLatin1Char(' ');
            comment += fields.at(i);
        }

        // Hashed host entries (|1|salt|hash) are opaque: recorded but never
        // matchable, so they still round-trip through serialize().
        if (hostField.startsWith(QLatin1Char('|'))) {
            store.m_entries.append(
                Entry{hostField, keyType, key, false, comment});
            continue;
        }

        // A single line may list several comma-separated hosts sharing one key.
        const QStringList hosts =
            hostField.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& host : hosts)
            store.m_entries.append(Entry{host, keyType, key, true, comment});
    }
    return store;
}

const QList<KnownHosts::Entry>& KnownHosts::entries() const
{
    return m_entries;
}

} // namespace ch
