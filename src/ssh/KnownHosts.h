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
// Wildcard host fields (`*`, `?`, and `!` negation) are matched with OpenSSH's
// pattern rules, so a host covered by a pattern is a trusted host and a
// different key for it is refused rather than offered as first use.
class KnownHosts {
public:
    enum class Verdict {
        Unknown,   // the host has no trusted entry at all
        Match,     // host+keyType present with the same key blob
        Mismatch,  // the host IS trusted but not with this key (refuse)
    };

    struct Entry {
        QString host;           // single hostname or address as stored
        QString keyType;        // e.g. "ssh-ed25519", "ssh-rsa"
        QByteArray key;         // raw key blob (base64-decoded)
        bool supported = true;  // false for hashed |1|, wildcard/negated, and
                                // @marker entries (excluded from add()/replace;
                                // verify() still consults hashed hosts via
                                // HMAC-SHA1 and pattern hosts via glob match)
        QString comment;        // trailing comment, empty if none
        QString marker;         // "@cert-authority"/"@revoked", empty if none
    };

    // Verify a presented host key. Returns Match iff host+keyType exists with an
    // identical blob. Returns Mismatch if the host is trusted at all but this
    // exact blob is not among its entries — that covers both the classic
    // changed-key case (same keyType, different blob) AND a key of a DIFFERENT
    // type presented for a host we already trust, which is otherwise a
    // downgrade around the hard refusal: a MITM that cannot reproduce the
    // trusted ed25519 key would simply offer an RSA one and be greeted by the
    // friendly first-use prompt. @revoked and @cert-authority entries never
    // establish that trust, so they cannot turn an unrelated key into a
    // Mismatch. Unknown only when the host has no trusted entry at all. Hashed
    // |1| entries are matched via their HMAC-SHA1 salted hostname hash; wildcard
    // and negated entries via OpenSSH's pattern rules; plain names are compared
    // case-insensitively, again like OpenSSH.
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
    // A plain comma-separated host list expands to one entry per name, while a
    // list containing wildcards or a `!` negation is kept verbatim in one entry
    // so its negation keeps working.
    static KnownHosts parse(const QString& text);

    const QList<Entry>& entries() const;

private:
    QList<Entry> m_entries;
};

} // namespace ch
