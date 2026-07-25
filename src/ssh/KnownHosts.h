#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace ch {

// Pure, network-free OpenSSH known_hosts store (SPEC 12.1).
//
// Parses the known_hosts text format (host, key-type, base64-encoded key blob),
// verifies presented host keys against the store, and serializes back to the
// same format. Key blobs are held as raw bytes (decoded from base64), matching
// what libssh exports via ssh_pki_export_pubkey_base64 so callers can compare
// directly. Hashed (|1|salt|hash) host entries are preserved on round-trip and
// participate in verification: their hostname is matched via HMAC-SHA1 over the
// salt, so a changed or @revoked key at a hashed host is refused (Mismatch).
class KnownHosts {
public:
    enum class Verdict {
        Unknown,   // host+keyType absent from the store
        Match,     // host+keyType present with the same key blob
        Mismatch,  // host+keyType present with a different key blob (refuse)
    };

    struct Entry {
        QString host;           // single hostname or address as stored
        QString keyType;        // e.g. "ssh-ed25519", "ssh-rsa"
        QByteArray key;         // raw key blob (base64-decoded)
        bool supported = true;  // false for hashed |1| and @marker entries
                                // (excluded from add()/replace; verify() still
                                // consults hashed hosts via HMAC-SHA1)
        QString comment;        // trailing comment, empty if none
        QString marker;         // "@cert-authority"/"@revoked", empty if none
    };

    // Verify a presented host key. Returns Match iff host+keyType exists with an
    // identical blob; Mismatch iff host+keyType exists with a different blob;
    // Unknown if the host+keyType pair is absent. Hashed |1| entries are matched
    // via their HMAC-SHA1 salted hostname hash, like plaintext entries.
    Verdict verify(const QString& host, const QString& keyType,
                   const QByteArray& keyBlob) const;

    // Record (or replace) a host key. A subsequent verify() with the same triple
    // returns Match. Distinct key types for one host are kept independently.
    void add(const QString& host, const QString& keyType,
             const QByteArray& keyBlob);

    // Serialize to known_hosts text: one "host keyType base64key [comment]" line
    // per entry, terminated by newlines.
    QByteArray serialize() const;

    // Parse known_hosts-format text. Blank lines and #-comments are ignored;
    // a leading @marker (@cert-authority, @revoked) is captured: a @revoked key
    // is refused (Mismatch) if presented and @cert-authority entries are opaque.
    static KnownHosts parse(const QString& text);

    const QList<Entry>& entries() const;

private:
    QList<Entry> m_entries;
};

} // namespace ch
