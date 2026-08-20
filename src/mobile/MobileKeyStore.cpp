#include "MobileKeyStore.h"

#include "KnownHosts.h"
#include "MobileKeyReference.h"
#include "SshConnectionPool.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVariantList>

#include <algorithm>
#include <utility>

namespace ch {

namespace {

// The OpenSSH private-key container's magic, NUL included (PROTOCOL.key).
const QByteArray& openSshMagic()
{
    static const QByteArray magic =
        QByteArrayLiteral("openssh-key-v1") + QByteArray(1, '\0');
    return magic;
}

// Every PEM label this client will accept. A whitelist rather than a suffix test
// on "PRIVATE KEY": libssh loads exactly these, and a label it does not know is
// a key that would fail at handshake time instead of at import time, which is
// the wrong place for the user to find out.
const QStringList& acceptedPemLabels()
{
    static const QStringList labels = {
        QStringLiteral("RSA PRIVATE KEY"),
        QStringLiteral("DSA PRIVATE KEY"),
        QStringLiteral("EC PRIVATE KEY"),
        QStringLiteral("PRIVATE KEY"),            // PKCS#8, unencrypted
        QStringLiteral("ENCRYPTED PRIVATE KEY"),  // PKCS#8, encrypted
    };
    return labels;
}

// OpenSSH's own fingerprint spelling, and deliberately the same one
// AppController::displayFingerprint() uses for a HOST key: base64(SHA-256(blob))
// with the padding dropped, behind a literal "SHA256:". A user comparing a key
// fingerprint here against `ssh-keygen -lf` output, or against the host-key
// prompt, must not have to translate between two spellings of one digest.
QString sha256Fingerprint(const QByteArray& blob)
{
    return QStringLiteral("SHA256:")
           + QString::fromLatin1(
               QCryptographicHash::hash(blob, QCryptographicHash::Sha256)
                   .toBase64(QByteArray::OmitTrailingEquals));
}

// One SSH wire string: uint32 big-endian length, then that many bytes. Returns
// false — rather than clamping — on any length that runs past the end, which is
// what a truncated or hostile container looks like.
bool readWireString(const QByteArray& in, qsizetype& offset, QByteArray& out)
{
    if (offset < 0 || in.size() - offset < 4)
        return false;
    const auto* raw = reinterpret_cast<const quint8*>(in.constData() + offset);
    const quint32 length = (quint32(raw[0]) << 24) | (quint32(raw[1]) << 16)
                           | (quint32(raw[2]) << 8) | quint32(raw[3]);
    offset += 4;
    if (length > quint32(in.size() - offset))
        return false;
    out = in.mid(offset, qsizetype(length));
    offset += qsizetype(length);
    return true;
}

bool readWireUint32(const QByteArray& in, qsizetype& offset, quint32& out)
{
    if (offset < 0 || in.size() - offset < 4)
        return false;
    const auto* raw = reinterpret_cast<const quint8*>(in.constData() + offset);
    out = (quint32(raw[0]) << 24) | (quint32(raw[1]) << 16)
          | (quint32(raw[2]) << 8) | quint32(raw[3]);
    offset += 4;
    return true;
}

// Strip the trailing carriage return and any leading/trailing blanks a paste or
// a mail client added. PEM cares about neither, and a `-----` marker only
// compares equal once they are gone.
QString tidyLine(QStringView line)
{
    return line.trimmed().toString();
}

// Overwrite a secret's characters before the buffer is released, so a passphrase
// does not linger in freed heap memory for the rest of the run (SPEC 12.3).
//
// Same limitation SshConnectionPool::wipeSecret() documents, and worth spelling
// out rather than implying: only the copy this object owns can be scrubbed.
// data() detaches it first, so the fill cannot corrupt a QString the caller
// still holds — and by the same token cannot scrub that one either.
void wipeSecret(QString& secret)
{
    if (!secret.isEmpty()) {
        QChar* raw = secret.data();
        std::fill(raw, raw + secret.size(), QChar(u'\0'));
    }
    secret.clear();
}

// The same for key material.
void wipeKeyMaterial(QByteArray& material)
{
    if (!material.isEmpty()) {
        material.detach();
        material.fill('\0');
    }
    material.clear();
}

}  // namespace

// ---------------------------------------------------------------------------
// pure classifiers
// ---------------------------------------------------------------------------

bool MobileKeyStore::isUsableKeyName(const QString& name)
{
    if (name.isEmpty() || name.size() > 64)
        return false;
    if (name.startsWith(QLatin1Char('.')))
        return false;
    for (const QChar c : name) {
        const bool ok = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')
                        || (c >= u'0' && c <= u'9') || c == u'.' || c == u'_'
                        || c == u'-';
        if (!ok)
            return false;
    }
    return true;
}

MobileKeyStore::KeyDescription MobileKeyStore::describeKeyText(const QString& text)
{
    KeyDescription out;

    const QList<QStringView> rawLines = QStringView(text).split(u'\n');
    QStringList lines;
    lines.reserve(rawLines.size());
    for (QStringView raw : rawLines)
        lines.append(tidyLine(raw));

    // A `.pub` file is THE mistake users make, so it gets its own sentence
    // instead of a generic "no private key found". Both spellings: the one-line
    // authorized_keys form and the PEM SubjectPublicKeyInfo block.
    const auto looksLikePublicKey = [](const QString& line) {
        return line.startsWith(QLatin1String("ssh-"))
               || line.startsWith(QLatin1String("ecdsa-sha2-"))
               || line.startsWith(QLatin1String("sk-ssh-"))
               || line.startsWith(QLatin1String("sk-ecdsa-"))
               || line == QLatin1String("-----BEGIN PUBLIC KEY-----");
    };

    static const QRegularExpression beginMarker(
        QStringLiteral("^-----BEGIN ([A-Z0-9 ]+)-----$"));

    qsizetype beginIndex = -1;
    QString label;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch match = beginMarker.match(lines.at(i));
        if (match.hasMatch()) {
            beginIndex = i;
            label = match.captured(1);
            break;
        }
        if (looksLikePublicKey(lines.at(i))) {
            out.error = tr("That is a PUBLIC key. Import the private key file — "
                           "the one WITHOUT the .pub suffix.");
            return out;
        }
    }
    if (beginIndex < 0) {
        out.error = tr("No private key found. The text must contain a "
                       "\"-----BEGIN ... PRIVATE KEY-----\" line.");
        return out;
    }
    if (label.endsWith(QLatin1String("PUBLIC KEY"))) {
        out.error = tr("That is a PUBLIC key. Import the private key file — the "
                       "one WITHOUT the .pub suffix.");
        return out;
    }

    const bool openSsh = (label == QLatin1String("OPENSSH PRIVATE KEY"));
    if (!openSsh && !acceptedPemLabels().contains(label)) {
        out.error = tr("That block is not a private key CodeHarbor can use. "
                       "Supported formats are OpenSSH (\"BEGIN OPENSSH PRIVATE "
                       "KEY\") and PEM RSA, DSA, EC or PKCS#8.");
        return out;
    }
    out.format = openSsh ? QStringLiteral("openssh-key-v1")
                         : QStringLiteral("pem");

    const QString endLine = QStringLiteral("-----END %1-----").arg(label);
    qsizetype endIndex = -1;
    for (qsizetype i = beginIndex + 1; i < lines.size(); ++i) {
        if (lines.at(i) == endLine) {
            endIndex = i;
            break;
        }
    }
    if (endIndex < 0) {
        out.error = tr("The key block has no matching \"-----END ...-----\" "
                       "line, so the file is incomplete. Copy the whole key, "
                       "including the last line.");
        return out;
    }

    // Legacy PEM encryption is announced by RFC 1421 headers ahead of the body:
    //   Proc-Type: 4,ENCRYPTED
    //   DEK-Info: AES-128-CBC,<iv>
    // followed by a blank line. The OpenSSH container never carries headers, so
    // they are only looked for in the PEM case — a "Proc-Type:" line inside an
    // OPENSSH block is base64 that happens to contain a colon and must not be
    // eaten as a header.
    qsizetype bodyStart = beginIndex + 1;
    if (!openSsh) {
        while (bodyStart < endIndex) {
            const QString& line = lines.at(bodyStart);
            if (line.isEmpty())
                break;
            const qsizetype colon = line.indexOf(u':');
            if (colon <= 0)
                break;
            const QString field = line.left(colon);
            const QString value = line.sliced(colon + 1).trimmed();
            if (field == QLatin1String("Proc-Type")
                && value.contains(QLatin1String("ENCRYPTED"))) {
                out.encrypted = true;
            } else if (field == QLatin1String("DEK-Info")) {
                out.kdf = value.section(u',', 0, 0).trimmed();
                // DEK-Info without Proc-Type is malformed per RFC 1421, but the
                // header only exists to describe an ENCRYPTION, so its presence
                // alone is enough to require a passphrase. Reporting such a key
                // as unencrypted would show the connect page no passphrase field
                // and cost the user a whole failed authentication round before
                // the ladder asked for one.
                out.encrypted = true;
            }
            ++bodyStart;
        }
        // PKCS#8's own encrypted spelling needs no headers at all.
        if (label == QLatin1String("ENCRYPTED PRIVATE KEY")) {
            out.encrypted = true;
            if (out.kdf.isEmpty())
                out.kdf = QStringLiteral("pkcs8");
        }
    }

    QString base64;
    for (qsizetype i = bodyStart; i < endIndex; ++i) {
        const QString& line = lines.at(i);
        if (line.isEmpty())
            continue;
        base64 += line;
    }
    if (base64.isEmpty()) {
        out.error = tr("The key block is empty. Copy the whole key, including "
                       "every line between BEGIN and END.");
        return out;
    }

    // Validate the alphabet and the length BEFORE decoding, so a body that is
    // plainly not base64 — an ellipsis a chat client inserted, say — is reported
    // as altered rather than as a malformed container after a lenient decode
    // quietly dropped the offending characters.
    static const QRegularExpression base64Body(
        QStringLiteral("^[A-Za-z0-9+/]+={0,2}$"));
    const QString truncatedMessage =
        tr("The key body is not valid base64, so the file was altered or "
           "truncated on its way here. Copy it again without reformatting.");
    if (!base64Body.match(base64).hasMatch() || (base64.size() % 4) != 0) {
        out.error = truncatedMessage;
        return out;
    }
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
        base64.toLatin1(), QByteArray::Base64Encoding
                               | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        out.error = truncatedMessage;
        return out;
    }

    if (openSsh) {
        const QByteArray& blob = *decoded;
        if (!blob.startsWith(openSshMagic())) {
            out.error = tr("The key says it is an OpenSSH key but does not "
                           "carry the OpenSSH key header, so it is not usable.");
            return out;
        }
        // openssh-key-v1: magic, cipher, kdfname, kdfoptions, key count, then
        // one PUBLIC key blob per key — in the CLEAR, outside the encrypted
        // section. That is what lets an encrypted key still show its own
        // fingerprint and key type here, before any passphrase is known.
        qsizetype offset = openSshMagic().size();
        QByteArray cipher;
        QByteArray kdfName;
        QByteArray kdfOptions;
        quint32 keyCount = 0;
        QByteArray publicBlob;
        if (!readWireString(blob, offset, cipher)
            || !readWireString(blob, offset, kdfName)
            || !readWireString(blob, offset, kdfOptions)
            || !readWireUint32(blob, offset, keyCount) || keyCount < 1
            || !readWireString(blob, offset, publicBlob)) {
            out.error = tr("The OpenSSH key container is malformed, so the file "
                           "was altered or truncated. Copy it again without "
                           "reformatting.");
            return out;
        }
        qsizetype publicOffset = 0;
        QByteArray keyType;
        if (!readWireString(publicBlob, publicOffset, keyType)
            || keyType.isEmpty()) {
            out.error = tr("The OpenSSH key container does not name a key type, "
                           "so the file was altered or truncated.");
            return out;
        }
        out.keyType = QString::fromLatin1(keyType);
        out.encrypted = (cipher != QByteArrayLiteral("none"));
        if (kdfName != QByteArrayLiteral("none"))
            out.kdf = QString::fromLatin1(kdfName);
        out.fingerprint = sha256Fingerprint(publicBlob);
    }

    // The bytes to hand libssh: the block and nothing else, one LF-terminated
    // line each. Anything the user pasted around it (a mail signature, a shell
    // prompt) is dropped, CRLF is normalised, and the blank line a legacy PEM
    // needs between its headers and its body survives because empty lines inside
    // the block are kept as empty lines.
    QByteArray normalized;
    for (qsizetype i = beginIndex; i <= endIndex; ++i) {
        normalized += lines.at(i).toLatin1();
        normalized += '\n';
    }
    out.normalized = normalized;
    out.valid = true;
    return out;
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

MobileKeyStore::MobileKeyStore(QString rootDirectory, QObject* parent)
    : QObject(parent)
{
    // The ONLY path this class derives, and it is not for key material: the
    // trusted-host store, at exactly the location SessionBootstrap's constructor
    // already derives, so handing this value to setKnownHostsPath() changes
    // nothing and documents everything. There is deliberately no keys directory
    // anywhere in this class — see the header for why.
    m_knownHostsPath =
        rootDirectory.isEmpty()
            ? QDir(QStandardPaths::writableLocation(
                       QStandardPaths::AppConfigLocation))
                  .filePath(QStringLiteral("known_hosts"))
            : QDir(rootDirectory).filePath(QStringLiteral("known_hosts"));
}

MobileKeyStore::~MobileKeyStore()
{
    // None of these emits, so all are safe from a destructor, and all are the
    // point of the class: no secret and no key material may outlive this object
    // inside freed heap memory.
    forgetPassphrase();
    for (auto it = m_memoryKeys.begin(); it != m_memoryKeys.end(); ++it)
        wipeKeyMaterial(it.value());
    m_memoryKeys.clear();
    // The pool holds its OWN copy of the last installed key (it detaches in
    // setInMemoryIdentity), and that copy is key material this object put there.
    // A pool that outlives this store — the destruction order in main.cpp — would
    // otherwise keep it until its own destructor ran. m_pool is a QPointer, so a
    // pool destroyed FIRST leaves nothing to dereference here.
    if (m_pool)
        m_pool->clearInMemoryIdentity();
}

void MobileKeyStore::setConnectionPool(SshConnectionPool* pool)
{
    m_pool = pool;
}

// ---------------------------------------------------------------------------
// listing and naming
// ---------------------------------------------------------------------------

QStringList MobileKeyStore::allKeyNames() const
{
    QStringList names = m_referenceNames;
    names += m_keyNames;
    return names;
}

void MobileKeyStore::setLastError(const QString& message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

bool MobileKeyStore::claimName(const QString& name)
{
    if (!isUsableKeyName(name)) {
        setLastError(tr("Give the key a name of 1 to 64 letters, digits, dots, "
                        "dashes or underscores."));
        return false;
    }
    if (m_memoryKeys.contains(name) || m_references.contains(name)) {
        setLastError(tr("A key named \"%1\" is already loaded. Remove it first "
                        "if you mean to replace it.")
                         .arg(name));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// path 1: in-memory import
// ---------------------------------------------------------------------------

bool MobileKeyStore::importKeyFromText(QString name, QString pem)
{
    name = name.trimmed();
    if (!claimName(name))
        return false;

    // Validated BEFORE anything is retained, and there is nothing to undo if it
    // fails: this function performs no I/O at all, which is the whole promise of
    // the in-memory path.
    const KeyDescription described = describeKeyText(pem);
    if (!described.valid) {
        setLastError(described.error);
        return false;
    }

    m_memoryKeys.insert(name, described.normalized);
    m_keyNames.append(name);
    m_keyNames.sort();
    setLastError(QString());
    emit keyNamesChanged();
    emit keyImported(name, false);
    return true;
}

bool MobileKeyStore::importKeyFromFile(QUrl fileUrl, QString name)
{
    QString text;
    QString settledName;
    m_lastFailureAllowsSessionOnly = false;
    if (!readPickedFile(fileUrl, name, &text, &settledName))
        return false;
    // Note what is NOT kept: no reference to where the file was. This entry point
    // exists for a user who wants to pick a key once per launch and leave nothing
    // behind at all.
    const bool imported = importKeyFromText(settledName, text);
    // `text` is the only copy of the key bytes left standing — importKeyFromText()
    // took a value parameter, but that copy died with the call, and what it
    // retained is the re-serialised `normalized` block, a different buffer. So
    // this wipe is not the no-op a wipe of a still-shared string would be: with
    // no other reference, data() does not detach and the fill really does erase
    // the characters instead of erasing a fresh copy of them.
    wipeSecret(text);
    return imported;
}

// ---------------------------------------------------------------------------
// path 2: reference to a user-managed key file
// ---------------------------------------------------------------------------

QString MobileKeyStore::addReferenceFromFile(QUrl fileUrl, QString name)
{
    QString text;
    QString settledName;
    // Only a reference that could not be made DURABLE leaves the picked file
    // usable for this session; every other failure below (unreadable, too large,
    // not a key, name taken) would fail the session-only import in exactly the
    // same way. The import sheet offers that fallback off this flag, so it must
    // start false and be set in one place only.
    m_lastFailureAllowsSessionOnly = false;
    if (!readPickedFile(fileUrl, name, &text, &settledName))
        return QString();
    if (!claimName(settledName)) {
        wipeSecret(text);
        return QString();
    }

    // Refuse a file that is not a key while the user is still looking at the
    // picker, rather than three screens later at a failed handshake. The bytes go
    // out of scope with `described` and are never retained here — a reference is
    // resolved afresh at connect time — and `text` is wiped rather than merely
    // dropped, because it is this function's own private copy.
    const KeyDescription described = describeKeyText(text);
    wipeSecret(text);
    if (!described.valid) {
        setLastError(described.error);
        return QString();
    }

    QString referenceError;
    const QString reference =
        keyref::makeDurableReference(fileUrl, &referenceError);
    if (reference.isEmpty()) {
        setLastError(referenceError.isEmpty()
                         ? tr("That file cannot be remembered. Paste the key "
                              "text instead to use it for this session.")
                         : referenceError);
        // The file IS a valid key: this is the one failure the session-only
        // import can still recover from.
        m_lastFailureAllowsSessionOnly = true;
        return QString();
    }

    m_references.insert(settledName, reference);
    m_referenceNames.append(settledName);
    m_referenceNames.sort();
    setLastError(QString());
    emit keyNamesChanged();
    emit keyImported(settledName, true);
    return settledName;
}

QString MobileKeyStore::registerReference(QString reference, QString name)
{
    reference = reference.trimmed();
    if (reference.isEmpty()) {
        setLastError(tr("There is no key reference to load."));
        return QString();
    }
    // Already known? Answer with the name it is known by. This is the ordinary
    // case: the connect page re-adopts the reference out of the saved profile
    // every time it loads it, and doing that must not accumulate duplicates.
    for (auto it = m_references.cbegin(); it != m_references.cend(); ++it) {
        if (it.value() == reference) {
            setLastError(QString());
            return it.key();
        }
    }
    if (!keyref::isResolvableReference(reference)) {
        setLastError(tr("This device cannot open the key file that server "
                        "profile refers to. Choose the key file again."));
        return QString();
    }

    name = name.trimmed();
    // A caller-supplied name is NOT trusted here. This entry point is reached
    // with whatever a saved profile carried, and unlike every other import path
    // it does not run claimName() on the happy path — so without this check a
    // name with a space, a leading dot or 400 characters would be inserted into
    // the key maps and offered in the QML picker, breaking the invariant
    // isUsableKeyName() exists to state. Derivation is the fallback rather than a
    // refusal, for the reason below.
    if (!isUsableKeyName(name)) {
        // Derive a name from the reference's last path segment. A content: URI or
        // a bookmark has no meaningful segment, so fall back to a fixed, valid
        // name rather than refusing: the user did not type this reference, they
        // saved a profile, and a nameless credential they cannot select is worse
        // than a generically named one.
        name = QFileInfo(reference).completeBaseName();
        if (!isUsableKeyName(name))
            name = QStringLiteral("saved-key");
    }
    // A derived name may already be taken by an unrelated key; make it unique
    // rather than refusing to load the profile's credential.
    if (m_memoryKeys.contains(name) || m_references.contains(name)) {
        const QString base = name;
        for (int suffix = 2; suffix < 1000; ++suffix) {
            name = base + QStringLiteral("-") + QString::number(suffix);
            if (!m_memoryKeys.contains(name) && !m_references.contains(name))
                break;
        }
        if (!claimName(name))
            return QString();
    }

    m_references.insert(name, reference);
    m_referenceNames.append(name);
    m_referenceNames.sort();
    setLastError(QString());
    emit keyNamesChanged();
    return name;
}

QString MobileKeyStore::referenceFor(QString name) const
{
    return m_references.value(name.trimmed());
}

// ---------------------------------------------------------------------------
// using and forgetting a key
// ---------------------------------------------------------------------------

bool MobileKeyStore::removeKey(QString name)
{
    name = name.trimmed();
    const auto inMemory = m_memoryKeys.find(name);
    if (inMemory != m_memoryKeys.end()) {
        wipeKeyMaterial(inMemory.value());
        m_memoryKeys.erase(inMemory);
        m_keyNames.removeAll(name);
    } else if (m_references.remove(name) > 0) {
        m_referenceNames.removeAll(name);
    } else {
        setLastError(tr("There is no loaded key by that name."));
        return false;
    }

    // The pool holds its OWN copy of whatever applyIdentityForConnect() last
    // installed, and removing a key is the user being told CodeHarbor will forget
    // it. Without this the bytes stay live in the pool for the rest of the run and
    // — worse — SessionBootstrap's reconnect ladder would re-authenticate with a
    // credential the user has just deleted. Only the INSTALLED name clears it:
    // removing an unrelated key must not disarm the credential the live session is
    // actually using, or the next reconnect would fail authentication.
    if (m_installedIdentityName == name) {
        if (m_pool)
            m_pool->clearInMemoryIdentity();
        m_installedIdentityName.clear();
    }

    // Drop an armed passphrase with it: a later key under the same name is a
    // DIFFERENT key, and sending it a passphrase that cannot unlock it spends an
    // authentication attempt for nothing.
    if (m_armedPassphraseName == name)
        forgetPassphrase();
    setLastError(QString());
    emit keyNamesChanged();
    emit keyRemoved(name);
    return true;
}

QByteArray MobileKeyStore::keyMaterial(const QString& name)
{
    const auto inMemory = m_memoryKeys.constFind(name);
    if (inMemory != m_memoryKeys.constEnd())
        return inMemory.value();

    const auto reference = m_references.constFind(name);
    if (reference == m_references.constEnd())
        return {};

    QString referenceError;
    QByteArray raw =
        keyref::readReference(reference.value(), kMaxKeyBytes, &referenceError);
    if (raw.isEmpty()) {
        setLastError(referenceError.isEmpty()
                         ? tr("The referenced key file could not be read.")
                         : referenceError);
        return {};
    }
    // Normalise through the same describeKeyText() the paste path uses, so a
    // reference whose file has been replaced with something that is not a key is
    // caught here rather than inside libssh.
    //
    // The UTF-16 conversion is a SECOND copy of the whole key, in a buffer this
    // function owns alone, so it is wiped explicitly: letting `QString::fromUtf8`
    // produce a temporary would leave the key's characters in freed heap memory
    // for the rest of the run, which is precisely what this class promises not to
    // do. describeKeyText() takes a const reference and keeps nothing, so the
    // wipe below cannot pull the ground out from under it.
    QString text = QString::fromUtf8(raw);
    wipeKeyMaterial(raw);
    const KeyDescription described = describeKeyText(text);
    wipeSecret(text);
    if (!described.valid) {
        setLastError(described.error);
        return {};
    }
    return described.normalized;
}

bool MobileKeyStore::applyIdentityForConnect(QString name)
{
    name = name.trimmed();
    if (!m_pool) {
        setLastError(tr("There is no SSH connection to hand a credential to."));
        return false;
    }

    // Always clear first: a previous attempt's key must never leak into a
    // connection the user aimed at a different credential. The name goes with it,
    // so nothing believes a credential is installed while the pool has none.
    m_pool->clearInMemoryIdentity();
    m_installedIdentityName.clear();
    if (name.isEmpty()) {
        setLastError(QString());
        return true;  // password / keyboard-interactive only, deliberately
    }
    if (!m_memoryKeys.contains(name) && !m_references.contains(name)) {
        setLastError(tr("There is no loaded key by that name."));
        return false;
    }

    QByteArray material = keyMaterial(name);
    if (material.isEmpty())
        return false;  // keyMaterial() already said why
    m_pool->setInMemoryIdentity(material);
    // For a REFERENCED key this is the only copy outside the pool (the pool
    // detaches its own), so the wipe erases the real characters. For an
    // in-memory key `material` still shares the buffer this object keeps under
    // that name, and wipeKeyMaterial() detaches before filling — deliberately:
    // scrubbing here would destroy the very key the session is meant to keep,
    // and forgetSession() is what erases that one.
    wipeKeyMaterial(material);
    // Remembered so removeKey() knows whether dropping a key also has to disarm
    // the pool. It is a NAME, not key material.
    m_installedIdentityName = name;
    setLastError(QString());
    return true;
}

void MobileKeyStore::forgetSession()
{
    forgetPassphrase();
    for (auto it = m_memoryKeys.begin(); it != m_memoryKeys.end(); ++it)
        wipeKeyMaterial(it.value());
    m_memoryKeys.clear();
    m_keyNames.clear();
    m_installedIdentityName.clear();
    if (m_pool)
        m_pool->clearInMemoryIdentity();
    // References survive: they are not secrets, and dropping them would force a
    // fresh pick after every disconnect.
    emit keyNamesChanged();
}

QString MobileKeyStore::keyFingerprint(QString name)
{
    return keyInfo(std::move(name))
        .value(QStringLiteral("fingerprint"))
        .toString();
}

QVariantMap MobileKeyStore::keyInfo(QString name)
{
    name = name.trimmed();
    const bool referenced = m_references.contains(name);
    if (!referenced && !m_memoryKeys.contains(name))
        return {};
    QByteArray material = keyMaterial(name);
    if (material.isEmpty())
        return {};
    // Same reason as in keyMaterial(): the UTF-16 conversion is a private second
    // copy of the key and is erased rather than dropped on the floor.
    QString text = QString::fromUtf8(material);
    wipeKeyMaterial(material);
    const KeyDescription described = describeKeyText(text);
    wipeSecret(text);
    if (!described.valid)
        return {};

    return QVariantMap{
        {QStringLiteral("name"), name},
        {QStringLiteral("referenced"), referenced},
        {QStringLiteral("reference"), m_references.value(name)},
        {QStringLiteral("format"), described.format},
        {QStringLiteral("keyType"), described.keyType},
        {QStringLiteral("encrypted"), described.encrypted},
        {QStringLiteral("kdf"), described.kdf},
        {QStringLiteral("fingerprint"), described.fingerprint},
        {QStringLiteral("fingerprintAvailable"), !described.fingerprint.isEmpty()},
    };
}

QVariantMap MobileKeyStore::describeText(QString text) const
{
    const KeyDescription described = describeKeyText(text);
    // The normalized BYTES are deliberately NOT in this map: it goes to QML, and
    // the one thing a preview must never do is hand the key back to the surface
    // that is about to clear it.
    return QVariantMap{
        {QStringLiteral("valid"), described.valid},
        {QStringLiteral("error"), described.error},
        {QStringLiteral("format"), described.format},
        {QStringLiteral("keyType"), described.keyType},
        {QStringLiteral("encrypted"), described.encrypted},
        {QStringLiteral("kdf"), described.kdf},
        {QStringLiteral("fingerprint"), described.fingerprint},
    };
}

// ---------------------------------------------------------------------------
// picked-file plumbing
// ---------------------------------------------------------------------------

bool MobileKeyStore::readPickedFile(const QUrl& fileUrl, QString requestedName,
                                    QString* textOut, QString* nameOut)
{
    QString path;
    if (fileUrl.isLocalFile()) {
        path = fileUrl.toLocalFile();
    } else if (fileUrl.scheme().isEmpty()) {
        // QML pickers hand back a URL, but a plain path is what a test or a
        // command line supplies; accept both rather than making the caller guess.
        path = fileUrl.toString();
    } else if (fileUrl.scheme() == QLatin1String("content")) {
        // Android's document picker returns a content: URI. Qt's Android file
        // engine opens those through QFile directly, so the URI IS the path here;
        // there is nothing to convert.
        path = fileUrl.toString();
    } else {
        setLastError(tr("That location cannot be read directly. Open the key "
                        "file and paste its text instead."));
        return false;
    }
    if (path.isEmpty()) {
        setLastError(tr("No file was chosen."));
        return false;
    }

    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        setLastError(tr("Could not read %1 (%2).").arg(path, in.errorString()));
        return false;
    }
    // Read one byte past the bound rather than trusting size(): a content: URI
    // may not report a size at all, and the picker can point at anything.
    QByteArray contents = in.read(kMaxKeyBytes + 1);
    in.close();
    if (contents.size() > kMaxKeyBytes) {
        // Not key material as far as anyone knows, but it was read on the chance
        // that it was, so it is erased on the same footing as the rest.
        wipeKeyMaterial(contents);
        setLastError(tr("That file is far too large to be a private key, so it "
                        "was not read."));
        return false;
    }

    QString name = requestedName.trimmed();
    if (name.isEmpty()) {
        // "id_ed25519" -> "id_ed25519". A content: URI's last segment is opaque,
        // so this can fail; the sheet has a name field for exactly that case and
        // the message points at it.
        name = QFileInfo(path).completeBaseName();
        if (!isUsableKeyName(name)) {
            wipeKeyMaterial(contents);
            setLastError(tr("Could not work out a name from that file. Type a "
                            "name for the key."));
            return false;
        }
    }

    // The UTF-16 conversion allocates a buffer of its own, so the file bytes can
    // — and must — be erased here rather than left in freed heap memory. The
    // caller owns the wipe of what it is handed.
    *textOut = QString::fromUtf8(contents);
    wipeKeyMaterial(contents);
    *nameOut = name;
    return true;
}

// ---------------------------------------------------------------------------
// trusted-host lookup
// ---------------------------------------------------------------------------

QVariantMap MobileKeyStore::knownHostInfo(QString host, int port) const
{
    host = host.trimmed();
    // The pool hands the BARE host to its host-key callback and canonicalises
    // only for the known_hosts lookup itself
    // (SshConnectionPool::applyHostKeyPolicy). Reuse ITS function rather than
    // respelling "[host]:port" here: a second spelling that drifted would report
    // "new server" for a host the user trusted on a non-default port, which is
    // exactly the case this method exists to catch.
    const quint16 effectivePort =
        (port > 0 && port <= 65535) ? quint16(port) : quint16(22);
    const QString lookupHost =
        SshConnectionPool::lookupHostFor(host, effectivePort);

    QVariantMap out{
        {QStringLiteral("known"), false},
        {QStringLiteral("lookupHost"), lookupHost},
        {QStringLiteral("keyTypes"), QVariantList()},
        {QStringLiteral("fingerprints"), QVariantList()},
    };
    if (host.isEmpty())
        return out;

    QFile store(m_knownHostsPath);
    if (!store.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;  // no store yet: nothing is trusted, which is the default
    const QByteArray contents = store.readAll();
    store.close();
    const KnownHosts hosts = KnownHosts::parse(QString::fromUtf8(contents));

    // "Is this host trusted with SOME key?" asked of the authority rather than by
    // comparing names: KnownHosts::verify() also resolves hashed |1| and
    // wildcard host entries, which no name comparison here could. The probe blob
    // claims a 4 GiB length in its first field, so it cannot be any real key and
    // can only ever produce Mismatch (host trusted, different key) or Unknown
    // (host not trusted at all).
    static const QByteArray probe = QByteArrayLiteral("\xff\xff\xff\xff")
                                    + QByteArrayLiteral("codeharbor-probe");
    out[QStringLiteral("known")] =
        hosts.verify(lookupHost, QStringLiteral("ssh-ed25519"), probe)
        != KnownHosts::Verdict::Unknown;

    // For display only, and plain entries only: a hashed entry's hostname is not
    // recoverable, so it can be verified against but never listed.
    QVariantList keyTypes;
    QVariantList fingerprints;
    for (const KnownHosts::Entry& entry : hosts.entries()) {
        if (!entry.supported || !entry.marker.isEmpty())
            continue;
        if (entry.host.compare(lookupHost, Qt::CaseInsensitive) != 0)
            continue;
        keyTypes.append(entry.keyType);
        fingerprints.append(sha256Fingerprint(entry.key));
    }
    out[QStringLiteral("keyTypes")] = keyTypes;
    out[QStringLiteral("fingerprints")] = fingerprints;
    return out;
}

// ---------------------------------------------------------------------------
// one-shot passphrases (SPEC 12.1)
// ---------------------------------------------------------------------------

void MobileKeyStore::armPassphrase(QString name, QString passphrase)
{
    // One parked secret at a time. Arming a second overwrites the first rather
    // than accumulating a per-key cache, which is what keeps "one attempt, then
    // discarded" true without any expiry policy.
    forgetPassphrase();
    name = name.trimmed();
    if (passphrase.isEmpty())
        return;
    m_armedPassphraseName = name;
    m_armedPassphrase = passphrase;
    // Force a private copy so takePassphrase()'s wipe cannot be turned into a
    // silent detach — or, worse, corrupt the caller's own string.
    m_armedPassphrase.data();
}

bool MobileKeyStore::hasArmedPassphrase(QString name) const
{
    return !m_armedPassphrase.isEmpty()
           && m_armedPassphraseName == name.trimmed();
}

QString MobileKeyStore::takePassphrase(QString name)
{
    if (!hasArmedPassphrase(name))
        return QString();
    // A DEEP copy, not an assignment: QString is reference-counted, so a plain
    // copy would share the buffer and forgetPassphrase()'s fill would then detach
    // and zero a fresh block while the real characters lived on in the one being
    // returned. This way the wipe below erases the only remaining copy besides
    // the caller's.
    const QString secret(m_armedPassphrase.constData(), m_armedPassphrase.size());
    forgetPassphrase();
    return secret;
}

void MobileKeyStore::forgetPassphrase()
{
    wipeSecret(m_armedPassphrase);
    m_armedPassphraseName.clear();
}

}  // namespace ch
