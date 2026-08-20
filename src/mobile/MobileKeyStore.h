#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

namespace ch {

class SshConnectionPool;

// SSH credential handling for the Android/iOS client (SPEC 11.2, 12.1).
//
// ---------------------------------------------------------------------------
// WHY THIS CLASS EXISTS AT ALL: the desktop auth ladder has nothing to stand on
// ---------------------------------------------------------------------------
//
// SshConnectionPool climbs "ssh-agent and the configured/default keys -> the
// same keys with a passphrase -> password -> keyboard-interactive"
// (SshConnectionPool.h, AuthRung). Every rung above `password` is reached
// through ssh_userauth_publickey_auto(), and on a phone each of its credential
// sources is simply absent:
//
//   * NO SSH-AGENT. ssh_userauth_publickey_auto() offers the agent's identities
//     before it looks at any key file (see the comment on AuthRung::KeyFile),
//     and libssh finds the agent through SSH_AUTH_SOCK. Neither Android nor iOS
//     sets that variable, and neither has any process a client app could reach
//     an AF_UNIX socket to even if it did: on iOS a third-party app cannot spawn
//     a helper process at all, and nothing in the Android platform runs an
//     agent. That rung therefore contributes nothing rather than failing loudly.
//
//   * NO ~/.ssh. identityFileCandidates() falls back to scanning
//     QDir::home()/.ssh for id_ed25519/id_ecdsa/id_rsa
//     (SshConnectionPool.cpp). On Android QDir::homePath() is the app's own
//     sandbox directory and on iOS it is the app container; there is no ~/.ssh in
//     either, and no way for the user to put one there — the platform pickers
//     hand an app a document, not a directory it may enumerate.
//     ssh_options_parse_config() is likewise skipped because ~/.ssh/config does
//     not exist, so the IdentityFile route into the ladder is gone too.
//
//   * QStandardPaths RESOLVES INSIDE THE SANDBOX. AppDataLocation and
//     AppConfigLocation point at per-app private directories. That matters for
//     known_hosts, which SessionBootstrap already keeps in AppConfigLocation
//     (SessionBootstrap.cpp:114-116) and which therefore needs no mobile-specific
//     handling at all — and it is deliberately NOT used for key material here,
//     for the reason immediately below.
//
// ---------------------------------------------------------------------------
// THIS CLASS STORES NO KEY MATERIAL. AT ALL.
// ---------------------------------------------------------------------------
//
// SPEC 11.2 (docs/SPEC.md:1710-1729) is an EXHAUSTIVE allowlist — "The client
// may store only:" — of what the client may keep locally. Private-key bytes are
// not on it. The single credential entry is "SSH-agent or credential-store
// references" (docs/SPEC.md:1721), and the profile-store sentence that follows
// (docs/SPEC.md:1726-1729) permits "an optional local identity-file path" and
// forbids "private-key bytes" by name.
//
// So there are exactly TWO compliant credential paths, and this class implements
// both. There is no third one, and in particular there is no
// <AppDataLocation>/keys: not as a default, not as an opt-in, and not as a
// temporary file during a handshake — a temp file is still client-stored key
// bytes.
//
//   1. IN-MEMORY IMPORT (the default and the primary path).
//      importKeyFromText() validates a pasted key and holds its armoured bytes in
//      a member for the session. applyIdentityForConnect() hands them to
//      SshConnectionPool::setInMemoryIdentity(), which parses the armour directly
//      through ssh_pki_import_privkey_base64() — no file is involved anywhere.
//      forgetSession() wipes them.
//
//      The cost is real and is stated rather than hidden: the key has to be
//      pasted or re-picked on every launch. That is the SAME trade SPEC 12.1
//      already accepts for passphrases (docs/SPEC.md:1798-1805: "No credential
//      store is integrated. A password or key passphrase is prompted for, handed
//      straight to libssh for that one authentication attempt, and then
//      discarded... The requirement above is therefore met in its strong form —
//      no secret is persisted at all — at the cost of retyping it each
//      connection."). On mobile the same reasoning extends from the passphrase to
//      the key itself, because a phone has no ~/.ssh to have put it in.
//
//   2. A REFERENCE TO A USER-MANAGED KEY FILE (the durable path).
//      The key stays in the user's OWN storage — a document provider on Android,
//      a Files document on iOS, an ordinary path on the desktop builds of this
//      shell. addReferenceFromFile() turns a pick into a durable reference
//      (ch::keyref::makeDurableReference: an Android persistable-read grant, an
//      iOS security-scoped bookmark, or an absolute path), and the ONLY thing the
//      client persists is that reference string, in ch::ServerProfiles' EXISTING
//      identityFile field. No new profile field, no change to its whitelist, and
//      no key bytes in the profile — which is exactly the shape
//      docs/SPEC.md:1721 and :1727 allow.
//
//      The bytes are read on demand, into memory, at connect time
//      (applyIdentityForConnect) and never copied into app storage.
//
// PASSPHRASES ARE NEVER PERSISTED AND NEVER REUSED. armPassphrase() parks one
// secret for one attempt; takePassphrase() hands it over AND erases it in the
// same call, so a second attempt must ask again. There is no code path from a
// passphrase to QSettings, to a file, to a log line, or to a Q_PROPERTY a QML
// sheet could keep alive.
//
// NO KEYCHAIN OR KEYSTORE INTEGRATION, and none is faked. Qt 6.10 ships no
// keychain API, so it would mean per-platform JNI and Objective-C plus a
// biometric-unlock flow — and with nothing persisted there is nothing for a
// keychain to protect. What the durable path relies on instead is the platform's
// own permission model: an Android persistable URI grant and an iOS
// security-scoped bookmark are both per-app, revocable by the user, and useless
// to another app or device. The at-rest protection of the key itself is whatever
// the user chose for their own storage, which is where the key stays.
//
// ---------------------------------------------------------------------------
// THREADING
// ---------------------------------------------------------------------------
//
// UI-thread only, like ServerProfiles. Reference resolution is a bounded
// synchronous read of a few kilobytes, driven by a user action.
class MobileKeyStore : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList keyNames READ keyNames NOTIFY keyNamesChanged)
    Q_PROPERTY(QStringList referenceNames READ referenceNames NOTIFY keyNamesChanged)
    Q_PROPERTY(QStringList allKeyNames READ allKeyNames NOTIFY keyNamesChanged)
    Q_PROPERTY(bool hasKeys READ hasKeys NOTIFY keyNamesChanged)
    Q_PROPERTY(QString knownHostsPath READ knownHostsPath CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    // Empty `rootDirectory` -> the real per-app sandbox, whose only use here is
    // the trusted-host store at QStandardPaths::AppConfigLocation/known_hosts —
    // byte-for-byte the path SessionBootstrap already derives. A non-empty
    // rootDirectory puts it under that directory instead and exists for
    // tst_mobilekeystore, which asserts that NOTHING under it ever contains key
    // material.
    explicit MobileKeyStore(QString rootDirectory = QString(),
                            QObject* parent = nullptr);
    ~MobileKeyStore() override;

    // The pool applyIdentityForConnect() installs key material on. Null is
    // tolerated: without a pool there is nothing to authenticate, and
    // applyIdentityForConnect() says so through lastError() instead of
    // dereferencing nothing.
    Q_INVOKABLE void setConnectionPool(ch::SshConnectionPool* pool);

    // Names of keys imported IN MEMORY this session, sorted.
    QStringList keyNames() const { return m_keyNames; }
    // Names of user-managed key files this session knows a reference for, sorted.
    QStringList referenceNames() const { return m_referenceNames; }
    // Referenced keys first, then in-memory ones: what the connect page offers.
    // Referenced first because those are the ones that survived the last launch.
    QStringList allKeyNames() const;
    bool hasKeys() const
    {
        return !m_keyNames.isEmpty() || !m_referenceNames.isEmpty();
    }

    // The trusted-host store this client uses. NOT owned here and never written
    // here: SessionBootstrap owns the read-merge-write of known_hosts. This
    // accessor exists so main.cpp can hand the same path to
    // SessionBootstrap::setKnownHostsPath() and so knownHostInfo() below can
    // read it.
    QString knownHostsPath() const { return m_knownHostsPath; }

    // Why the last operation was refused, as one sentence for the user; empty
    // after a success. Never contains any part of a key or a passphrase.
    QString lastError() const { return m_lastError; }

    // ---- path 1: in-memory import ------------------------------------------

    // Validate `pem` and hold it in memory under `name` for this session. Nothing
    // is written: there is no filesystem call anywhere in this function, and no
    // fallback that writes the key "just for this attempt".
    //
    // Returns false with lastError() set for a name that cannot be used, a name
    // already taken, or text that is not a private key. Encrypted keys are
    // accepted and reported as such through keyInfo(), so the UI can ask for a
    // passphrase rather than failing at handshake time.
    Q_INVOKABLE bool importKeyFromText(QString name, QString pem);

    // Read a picked file and import its contents IN MEMORY, without keeping any
    // reference to where it came from. For a user who wants to pick a key once
    // per launch and leave no trace at all.
    Q_INVOKABLE bool importKeyFromFile(QUrl fileUrl, QString name);

    // ---- path 2: reference to a user-managed key file ----------------------

    // Turn a picked file into a DURABLE reference and remember it under `name`
    // for this session. The file is read once, now, so an unusable key is
    // reported while the user is still looking at the picker rather than at a
    // failed connection — but the bytes are dropped again immediately and are
    // re-read on demand at connect time.
    //
    // Returns the settled name, or an empty string with lastError() set.
    Q_INVOKABLE QString addReferenceFromFile(QUrl fileUrl, QString name);

    // After a FAILED addReferenceFromFile(): true only when the picked file was a
    // usable key that this platform could not turn into a DURABLE reference (a
    // document provider that refuses a persistable grant, a build with no
    // bookmark support). That is the one failure a session-only import of the
    // same file can still recover from, so it is the one failure the import sheet
    // may offer that fallback for; after any other refusal — unreadable, too
    // large, not a key, name already taken — the fallback would fail identically
    // and offering it is a dead end dressed up as a remedy.
    Q_INVOKABLE bool lastFailureAllowsSessionOnly() const
    {
        return m_lastFailureAllowsSessionOnly;
    }

    // Re-adopt a reference read back out of a saved profile's identityFile, so a
    // remembered server still offers its key after a relaunch. Idempotent, and a
    // no-op returning the existing name when this reference is already known.
    // Returns an empty string with lastError() set for a reference this platform
    // cannot resolve — a profile copied from another device, say — which is a
    // thing to explain rather than to fail on at connect time.
    Q_INVOKABLE QString registerReference(QString reference, QString name);

    // What to store in the profile's identityFile for `name`: the reference for a
    // referenced key, and deliberately EMPTY for an in-memory import, which has
    // nothing durable to record. This is the ONLY value that ever leaves this
    // class for persistence, and it is never key material.
    Q_INVOKABLE QString referenceFor(QString name) const;

    // ---- using and forgetting a key ----------------------------------------

    // Forget `name`: an in-memory key's bytes are overwritten, a reference is
    // dropped, an armed passphrase for it is dropped, and — when this is the key
    // whose material is currently installed on the pool — the pool's copy is
    // cleared too, so "forget" means forgotten everywhere rather than everywhere
    // except the one place that would re-authenticate with it. Returns false with
    // lastError() set for an unknown name.
    Q_INVOKABLE bool removeKey(QString name);

    // Install the credential for `name` on the pool for the NEXT connect attempt,
    // and return whether that succeeded:
    //   * in-memory name -> SshConnectionPool::setInMemoryIdentity(armoured PEM)
    //   * referenced name -> resolve the reference to bytes, then the same
    //   * empty name -> clearInMemoryIdentity(); the ladder falls back to
    //     password / keyboard-interactive
    // Always clears first, so a previous attempt's key can never leak into a
    // connection the user aimed at a different credential.
    Q_INVOKABLE bool applyIdentityForConnect(QString name);

    // Drop EVERY secret this object holds: in-memory keys, resolved reference
    // bytes, the armed passphrase, and the pool's in-memory identity. Called on
    // explicit disconnect and from the destructor. References themselves (which
    // are not secrets) survive, so reconnecting does not need a fresh pick.
    Q_INVOKABLE void forgetSession();

    // OpenSSH's own fingerprint of the key's PUBLIC half, "SHA256:<unpadded
    // base64>" — the exact string `ssh-keygen -lf` prints and the exact form
    // AppController shows for a host key, so a user can compare the two without
    // translating between spellings.
    //
    // Empty when the format does not carry the public half in the clear. That is
    // every legacy PEM key ("BEGIN RSA PRIVATE KEY" and friends): recovering
    // their public key means ASN.1 parsing, and for an encrypted one it is
    // impossible without the passphrase. An openssh-key-v1 key — what ssh-keygen
    // has produced by default for years — carries the public blob OUTSIDE the
    // encrypted section, so its fingerprint is available even while the key
    // itself stays locked. keyInfo()'s `fingerprintAvailable` says which case a
    // name is in, so the UI can leave the field out instead of showing a blank.
    Q_INVOKABLE QString keyFingerprint(QString name);

    // Everything the UI needs about a key, in one call:
    //   name                  as known
    //   referenced            true for a user-managed file, false for a paste
    //   reference             referenceFor() — empty for an in-memory import
    //   format                "openssh-key-v1" | "pem"
    //   keyType               "ssh-ed25519", "ecdsa-sha2-nistp256", ... ; empty
    //                         for a legacy PEM, whose type is not readable
    //                         without parsing the key material
    //   encrypted             true when a passphrase will be required
    //   kdf                   "bcrypt" for a modern encrypted key, the PEM
    //                         DEK-Info cipher for a legacy one, empty otherwise
    //   fingerprint           keyFingerprint()
    //   fingerprintAvailable  bool, see keyFingerprint()
    // Empty map for an unknown name, or for a reference that no longer resolves.
    Q_INVOKABLE QVariantMap keyInfo(QString name);

    // describeKeyText() as a QVariantMap, so KeyImportSheet can show what the
    // pasted text IS — format, key type, whether a passphrase will be needed,
    // fingerprint — or exactly why it was refused, WITHOUT importing it. Same
    // code the import runs, so a preview that says yes cannot be followed by an
    // import that says no. Keys: valid, error, format, keyType, encrypted, kdf,
    // fingerprint.
    Q_INVOKABLE QVariantMap describeText(QString text) const;

    // Has the user already approved a key for this endpoint? Answers
    //   known        bool
    //   lookupHost   the canonical known_hosts token that was looked up
    //   keyTypes     key types recorded for it, for display
    //   fingerprints "SHA256:..." per recorded key, for display
    // for the endpoint spelled the way SshConnectionPool::lookupHostFor() spells
    // it (bare host on port 22, "[host]:port" otherwise) — the pool hands the
    // BARE host to the host-key callback, so the port has to be re-applied here.
    //
    // `known` is decided by KnownHosts::verify() against a blob that cannot be
    // any real key, NOT by scanning entries: verify() is the authority and it
    // also matches hashed |1| and wildcard host entries, which a name comparison
    // cannot. Anything but Verdict::Unknown means the host is trusted with SOME
    // key. `keyTypes`/`fingerprints` are the plain-host entries only, because a
    // hashed entry's host is not recoverable for display.
    //
    // This is what lets HostTrustSheet tell "first contact with a new server"
    // apart from "we already trust this server and it just presented a DIFFERENT
    // key" — the second is a possible attack and must not look like routine
    // first use.
    Q_INVOKABLE QVariantMap knownHostInfo(QString host, int port) const;

    // ---- one-shot passphrases (SPEC 12.1) ----------------------------------
    Q_INVOKABLE void armPassphrase(QString name, QString passphrase);
    Q_INVOKABLE bool hasArmedPassphrase(QString name) const;
    // Returns the parked passphrase and erases the stored copy in the same call,
    // so a retry has to ask the user again by construction rather than by policy.
    Q_INVOKABLE QString takePassphrase(QString name);
    // Drop an armed passphrase unused, overwriting it.
    Q_INVOKABLE void forgetPassphrase();

    // ---- pure classifiers (testable without any filesystem) ----------------

    // What a candidate key text is, or why it is not a key at all.
    struct KeyDescription {
        bool valid = false;
        // One sentence for the user when !valid. Never quotes the input: a
        // rejected paste may still be key material, and a message is a thing
        // that gets logged.
        QString error;
        QString format;   // "openssh-key-v1" | "pem"
        QString keyType;  // SSH wire name, empty for a legacy PEM
        bool encrypted = false;
        QString kdf;
        QString fingerprint;  // "SHA256:..." or empty
        // The exact bytes to hand libssh: the BEGIN..END block with CRLF and
        // trailing whitespace normalised to LF-terminated lines, and anything the
        // user pasted around it dropped. Empty when !valid.
        QByteArray normalized;
    };

    // Recognises the OpenSSH container ("BEGIN OPENSSH PRIVATE KEY", which is
    // what ssh-keygen writes by default) and the legacy PEM spellings libssh
    // still loads ("BEGIN RSA/DSA/EC/ENCRYPTED PRIVATE KEY", "BEGIN PRIVATE
    // KEY"). A PUBLIC key — the .pub file, or "BEGIN PUBLIC KEY" — is the
    // mistake a user actually makes, so it is diagnosed by name instead of
    // falling through to a generic "not a private key".
    static KeyDescription describeKeyText(const QString& text);

    // A name that reads back to a human and can index the maps here: 1..64
    // characters of [A-Za-z0-9._-], not beginning with a dot. Refused at the
    // import boundary rather than sanitised, so the name the user typed is the
    // name they see.
    static bool isUsableKeyName(const QString& name);

    // Hard bound on an imported or referenced file, so a picker cannot be used to
    // read a gigabyte into memory. A 4096-bit RSA key in OpenSSH format is under
    // 4 KiB; 64 KiB leaves room for a certificate-bearing oddity and is still
    // nothing.
    static constexpr qint64 kMaxKeyBytes = 64 * 1024;

signals:
    void keyNamesChanged();
    void lastErrorChanged();
    // `referenced` distinguishes the two credential paths, so the import sheet can
    // say which one just happened without having to ask again.
    void keyImported(QString name, bool referenced);
    void keyRemoved(QString name);

private:
    void setLastError(const QString& message);
    // Read a picked file into `*textOut` and settle the key's name into
    // `*nameOut` (derived from the file when `requestedName` is blank). Shared by
    // both file entry points so the URL handling, the size bound and the name
    // derivation have exactly one spelling. Returns false with lastError() set.
    bool readPickedFile(const QUrl& fileUrl, QString requestedName,
                        QString* textOut, QString* nameOut);
    // The armoured key text for `name`: the in-memory copy, or a fresh read of
    // its reference. Empty for an unknown name or an unresolvable reference, with
    // lastError() set in the latter case. The one place that produces key
    // material, and the caller wipes what it gets.
    QByteArray keyMaterial(const QString& name);
    // Refuse a name that is malformed or already in use, with the message for it.
    bool claimName(const QString& name);

    QString m_knownHostsPath;
    // Name -> armoured PEM, in memory, this session only. Overwritten before
    // release.
    QHash<QString, QByteArray> m_memoryKeys;
    QStringList m_keyNames;
    // Name -> durable reference string. NOT a secret: it names a file, it carries
    // no key material, and it is what ends up in the profile's identityFile.
    QHash<QString, QString> m_references;
    QStringList m_referenceNames;
    QString m_lastError;
    // See lastFailureAllowsSessionOnly(). Reset by every file entry point, set in
    // exactly one place.
    bool m_lastFailureAllowsSessionOnly = false;
    // The key name whose material is currently installed on the pool, empty when
    // none is. NOT a secret and NOT persisted: it exists so removeKey() can tell
    // "the credential being deleted is the one the pool is holding" from "an
    // unrelated key is being tidied up", and disarm the pool only in the first
    // case.
    QString m_installedIdentityName;
    // The ONE parked passphrase and the key it belongs to.
    QString m_armedPassphraseName;
    QString m_armedPassphrase;
    // Not owned, and a QPointer rather than a raw pointer because the destructor
    // dereferences it to clear the pool's copy of the key: a pool destroyed first
    // must leave nothing to dereference, not a dangling address that happens to
    // work in the current declaration order. Only applyIdentityForConnect(),
    // forgetSession() and ~MobileKeyStore() touch it.
    QPointer<SshConnectionPool> m_pool;
};

}  // namespace ch
