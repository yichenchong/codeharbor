#include "MobileKeyStore.h"

#include "KnownHosts.h"
#include "MobileKeyReference.h"
#include "SshConnectionPool.h"

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVariantList>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#if CH_HAVE_LIBSSH
#include <libssh/libssh.h>
#endif

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

// Bounded DER framing for EncryptedPrivateKeyInfo. This checks the envelope,
// never decrypts it, and borrows the decoded bytes without copying key material.
bool readDerElement(QByteArrayView& input, quint8& tag, QByteArrayView& value)
{
    if (input.size() < 2)
        return false;
    tag = quint8(input[0]);
    if (tag == 0 || (tag & 0x1f) == 0x1f)
        return false;
    qsizetype header = 2;
    quint32 length = quint8(input[1]);
    if (length & 0x80) {
        const quint32 count = length & 0x7f;
        if (count == 0 || count > 4 || input.size() < 2 + count
            || input[2] == '\0')
            return false;
        length = 0;
        for (quint32 i = 0; i < count; ++i)
            length = (length << 8) | quint8(input[2 + i]);
        if (length < 128)
            return false;
        header += count;
    }
    if (quint64(length) > quint64(input.size() - header))
        return false;
    value = input.sliced(header, length);
    input = input.sliced(header + length);
    return true;
}

bool validDerOid(QByteArrayView oid)
{
    if (oid.isEmpty() || (quint8(oid.back()) & 0x80))
        return false;
    bool atStart = true;
    for (const char byte : oid) {
        if (atStart && quint8(byte) == 0x80)
            return false; // Non-minimal base-128 subidentifier.
        atStart = !(quint8(byte) & 0x80);
    }
    return true;
}

bool validDerContents(QByteArrayView input, int depth = 0)
{
    if (depth > 16)
        return false;
    while (!input.isEmpty()) {
        quint8 tag = 0;
        QByteArrayView value;
        if (!readDerElement(input, tag, value))
            return false;
        if ((tag & 0x20) && !validDerContents(value, depth + 1))
            return false;
        if (tag == 0x06 && !validDerOid(value))
            return false;
        if (tag == 0x05 && !value.isEmpty())
            return false;
        if (tag == 0x02
            && (value.isEmpty()
                || (value.size() > 1 && value[0] == '\0' && !(quint8(value[1]) & 0x80))
                || (value.size() > 1 && quint8(value[0]) == 0xff
                    && (quint8(value[1]) & 0x80))))
            return false;
    }
    return true;
}

bool readDerAlgorithm(QByteArrayView& input, QByteArrayView& oid,
                      quint8& parameterTag, QByteArrayView& parameters)
{
    quint8 tag = 0;
    QByteArrayView algorithm;
    if (!readDerElement(input, tag, algorithm) || tag != 0x30
        || !readDerElement(algorithm, tag, oid) || tag != 0x06 || !validDerOid(oid))
        return false;
    parameterTag = 0;
    parameters = {};
    if (algorithm.isEmpty())
        return true; // AlgorithmIdentifier permits absent parameters.
    const QByteArrayView encodedParameters = algorithm;
    return readDerElement(algorithm, parameterTag, parameters)
        && algorithm.isEmpty() && validDerContents(encodedParameters);
}

bool readPositiveDerInteger(QByteArrayView& input)
{
    quint8 tag = 0;
    QByteArrayView value;
    if (!readDerElement(input, tag, value) || tag != 0x02 || value.isEmpty()
        || (quint8(value[0]) & 0x80))
        return false;
    return std::any_of(value.begin(), value.end(), [](char byte) { return byte != 0; });
}

enum class EnvelopeStatus { Valid, Invalid, Unsupported };

EnvelopeStatus checkPbkdf2Parameters(QByteArrayView input)
{
    quint8 tag = 0;
    QByteArrayView salt;
    if (input.isEmpty())
        return EnvelopeStatus::Invalid;
    if (quint8(input[0]) == 0x30) {
        QByteArrayView saltAlgorithm;
        QByteArrayView saltParameters;
        if (!readDerAlgorithm(input, saltAlgorithm, tag, saltParameters))
            return EnvelopeStatus::Invalid;
        return EnvelopeStatus::Unsupported; // No external salt-source algorithm.
    }
    if (!readDerElement(input, tag, salt) || tag != 0x04
        || !readPositiveDerInteger(input))
        return EnvelopeStatus::Invalid;
    if (!input.isEmpty() && quint8(input[0]) == 0x02 && !readPositiveDerInteger(input))
        return EnvelopeStatus::Invalid;
    if (!input.isEmpty()) {
        QByteArrayView prf;
        QByteArrayView prfParameters;
        if (!readDerAlgorithm(input, prf, tag, prfParameters))
            return EnvelopeStatus::Invalid;
        // PKCS#5 HMAC-MD5 and HMAC-SHA1/SHA2 identifiers; parameters absent/NULL.
        if (prf.size() != 8
            || prf.first(7) != QByteArrayView("\x2a\x86\x48\x86\xf7\x0d\x02", 7)
            || quint8(prf[7]) < 6 || quint8(prf[7]) > 13)
            return EnvelopeStatus::Unsupported;
        if (tag != 0 && (tag != 0x05 || !prfParameters.isEmpty()))
            return EnvelopeStatus::Invalid;
    }
    return input.isEmpty() ? EnvelopeStatus::Valid : EnvelopeStatus::Invalid;
}

bool readPositiveDerUint64(QByteArrayView& input, quint64& result)
{
    quint8 tag = 0;
    QByteArrayView value;
    if (!readDerElement(input, tag, value) || tag != 0x02 || value.isEmpty()
        || (quint8(value[0]) & 0x80))
        return false;
    if (value[0] == '\0')
        value = value.sliced(1); // DER's sign padding for unsigned 64-bit values.
    if (value.isEmpty() || value.size() > 8)
        return false;
    result = 0;
    for (const char byte : value)
        result = (result << 8) | quint8(byte);
    return result > 0;
}

EnvelopeStatus checkScryptParameters(QByteArrayView input)
{
    quint8 tag = 0;
    QByteArrayView salt;
    quint64 cost = 0;
    quint64 blockSize = 0;
    quint64 parallelization = 0;
    if (!readDerElement(input, tag, salt) || tag != 0x04
        || !readPositiveDerUint64(input, cost)
        || !readPositiveDerUint64(input, blockSize)
        || !readPositiveDerUint64(input, parallelization))
        return EnvelopeStatus::Invalid;
    // RFC 7914: N > 1 is a power of two, N < 2^(16*r), and r*p < 2^30.
    if (cost <= 1 || (cost & (cost - 1)) != 0
        || blockSize >= (quint64(1) << 30)
        || parallelization > ((quint64(1) << 30) - 1) / blockSize
        || (blockSize < 4 && cost >= (quint64(1) << (16 * blockSize))))
        return EnvelopeStatus::Invalid;
    if (!input.isEmpty()) {
        quint64 keyLength = 0;
        if (!readPositiveDerUint64(input, keyLength))
            return EnvelopeStatus::Invalid;
    }
    return input.isEmpty() ? EnvelopeStatus::Valid : EnvelopeStatus::Invalid;
}

EnvelopeStatus checkEncryptedPkcs8(QByteArrayView input)
{
    quint8 tag = 0;
    quint8 parameterTag = 0;
    QByteArrayView sequence;
    QByteArrayView oid;
    QByteArrayView parameters;
    QByteArrayView ciphertext;
    if (!readDerElement(input, tag, sequence) || tag != 0x30 || !input.isEmpty()
        || !readDerAlgorithm(sequence, oid, parameterTag, parameters)
        || !readDerElement(sequence, tag, ciphertext) || tag != 0x04
        || ciphertext.isEmpty() || !sequence.isEmpty())
        return EnvelopeStatus::Invalid;
    if (oid == QByteArrayView("\x2a\x86\x48\x86\xf7\x0d\x01\x05\x0d", 9)) {
        if (parameterTag != 0x30)
            return EnvelopeStatus::Invalid;
        QByteArrayView kdfOid;
        QByteArrayView kdfParameters;
        QByteArrayView cipherOid;
        QByteArrayView cipherParameters;
        quint8 kdfParameterTag = 0;
        if (!readDerAlgorithm(parameters, kdfOid, kdfParameterTag, kdfParameters)
            || !readDerAlgorithm(parameters, cipherOid, tag, cipherParameters)
            || !parameters.isEmpty())
            return EnvelopeStatus::Invalid;
        const bool pbkdf2 =
            kdfOid == QByteArrayView("\x2a\x86\x48\x86\xf7\x0d\x01\x05\x0c", 9);
        const bool scrypt =
            kdfOid == QByteArrayView("\x2b\x06\x01\x04\x01\xda\x47\x04\x0b", 9);
        if (!pbkdf2 && !scrypt)
            return EnvelopeStatus::Unsupported;
        if (kdfParameterTag != 0x30)
            return EnvelopeStatus::Invalid;
        const auto kdfStatus = pbkdf2 ? checkPbkdf2Parameters(kdfParameters)
                                     : checkScryptParameters(kdfParameters);
        if (kdfStatus != EnvelopeStatus::Valid)
            return kdfStatus;
        int blockBytes = 0;
        if (cipherOid.size() == 9
            && cipherOid.first(8) == QByteArrayView("\x60\x86\x48\x01\x65\x03\x04\x01", 8)
            && (quint8(cipherOid[8]) == 2 || quint8(cipherOid[8]) == 22
                || quint8(cipherOid[8]) == 42))
            blockBytes = 16;
        else if (cipherOid == QByteArrayView("\x2a\x86\x48\x86\xf7\x0d\x03\x07", 8))
            blockBytes = 8;
        else
            return EnvelopeStatus::Unsupported;
        return tag == 0x04 && cipherParameters.size() == blockBytes
                && ciphertext.size() % blockBytes == 0
            ? EnvelopeStatus::Valid : EnvelopeStatus::Invalid;
    }
    // PBES1 (PKCS#5) and the PKCS#12 PBE families derive their IV from salt.
    const bool pkcs5 = oid.size() == 9
        && oid.first(8) == QByteArrayView("\x2a\x86\x48\x86\xf7\x0d\x01\x05", 8)
        && (quint8(oid[8]) == 1 || quint8(oid[8]) == 3 || quint8(oid[8]) == 4
            || quint8(oid[8]) == 6 || quint8(oid[8]) == 10 || quint8(oid[8]) == 11);
    const bool pkcs12 = oid.size() == 10
        && oid.first(9) == QByteArrayView("\x2a\x86\x48\x86\xf7\x0d\x01\x0c\x01", 9)
        && quint8(oid[9]) >= 1 && quint8(oid[9]) <= 6;
    if (!pkcs5 && !pkcs12)
        return EnvelopeStatus::Unsupported;
    QByteArrayView salt;
    if (parameterTag != 0x30
        || !readDerElement(parameters, tag, salt) || tag != 0x04
        || (pkcs5 && salt.size() != 8) || !readPositiveDerInteger(parameters)
        || !parameters.isEmpty())
        return EnvelopeStatus::Invalid;
    const int blockBytes = pkcs12 && quint8(oid[9]) <= 2 ? 1 : 8;
    return ciphertext.size() % blockBytes == 0
        ? EnvelopeStatus::Valid : EnvelopeStatus::Invalid;
}

// Strip the trailing carriage return and any leading/trailing blanks a paste or
// a mail client added. PEM cares about neither, and a `-----` marker only
// compares equal once they are gone.
QString tidyLine(QStringView line)
{
    return line.trimmed().toString();
}

// Best-effort overwrite of this value's writable buffer before release.
// Qt may detach shared storage, leaving other copies untouched; parser, QML,
// allocator and platform copies are not covered. This is not secure erasure.
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

#if CH_HAVE_LIBSSH
int declineKeyPassphrase(const char*, char*, size_t, int, int, void*)
{
    return -1;
}
#endif

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
    if (text.size() > kMaxKeyBytes) {
        out.error = tr("That text is far too large to be a private key.");
        return out;
    }

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
    int pemBlockBytes = 0;
    if (!openSsh) {
        bool hasProcType = false;
        bool hasDekInfo = false;
        while (bodyStart < endIndex) {
            const QString& line = lines.at(bodyStart);
            if (line.isEmpty())
                break;
            const qsizetype colon = line.indexOf(u':');
            if (colon <= 0)
                break;
            const QString field = line.left(colon);
            const QString value = line.sliced(colon + 1).trimmed();
            if (field == QLatin1String("Proc-Type")) {
                if (hasProcType || value != QLatin1String("4,ENCRYPTED")) {
                    out.error = tr("The PEM encryption headers are malformed.");
                    return out;
                }
                hasProcType = true;
                out.encrypted = true;
            } else if (field == QLatin1String("DEK-Info")) {
                static const QRegularExpression dekInfo(
                    QStringLiteral("^([A-Z0-9][A-Z0-9-]*),([0-9A-Fa-f]*)$"));
                const auto match = dekInfo.match(value);
                if (hasDekInfo || !match.hasMatch()) {
                    out.error = tr("The PEM encryption headers are malformed.");
                    return out;
                }
                hasDekInfo = true;
                out.kdf = match.captured(1);
                const qsizetype ivLength = match.capturedLength(2);
                int ivBytes = 0;
                static const QRegularExpression block128Cipher(
                    QStringLiteral("^(AES|CAMELLIA|ARIA)-(128|192|256)-"
                                   "(CBC|CFB|CFB1|CFB8|OFB|CTR)$"));
                static const QRegularExpression desCipher(
                    QStringLiteral("^DES-(EDE-|EDE3-)?(CBC|CFB|CFB1|CFB8|OFB)$"));
                if (block128Cipher.match(out.kdf).hasMatch()
                    || out.kdf == QLatin1String("SEED-CBC")
                    || out.kdf == QLatin1String("SM4-CBC"))
                    ivBytes = 16;
                else if (desCipher.match(out.kdf).hasMatch()
                         || out.kdf == QLatin1String("BF-CBC")
                         || out.kdf == QLatin1String("CAST5-CBC")
                         || out.kdf == QLatin1String("RC2-CBC")
                         || out.kdf == QLatin1String("RC2-40-CBC")
                         || out.kdf == QLatin1String("RC2-64-CBC")
                         || out.kdf == QLatin1String("IDEA-CBC"))
                    ivBytes = 8;
                else if (out.kdf != QLatin1String("RC4")
                         && out.kdf != QLatin1String("RC4-40")) {
                    out.error = tr("This PEM encryption algorithm is unsupported. "
                                   "Re-export the key as OpenSSH or AES-CBC PEM.");
                    return out;
                }
                const bool validIv = ivLength == 2 * ivBytes;
                if (!validIv) {
                    out.error = tr("The PEM encryption initialization vector is malformed.");
                    return out;
                }
                if (out.kdf.endsWith(QLatin1String("-CBC")))
                    pemBlockBytes = int(ivLength / 2);
                out.encrypted = true;
            }
            ++bodyStart;
        }
        if (hasProcType != hasDekInfo) {
            out.error = tr("The encrypted PEM key is missing an encryption "
                           "header. Copy the complete private key file.");
            return out;
        }
        if (hasProcType
            && (label == QLatin1String("ENCRYPTED PRIVATE KEY")
                || bodyStart == endIndex || !lines.at(bodyStart).isEmpty())) {
            out.error = tr("The PEM encryption headers are malformed.");
            return out;
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
    const auto envelope = label == QLatin1String("ENCRYPTED PRIVATE KEY")
        ? checkEncryptedPkcs8(decoded.decoded) : EnvelopeStatus::Valid;
    if (envelope == EnvelopeStatus::Unsupported) {
        out.error = tr("This encrypted PKCS#8 algorithm is unsupported. Re-export "
                       "the key as OpenSSH or PBKDF2 with AES-CBC.");
        return out;
    }
    if ((!openSsh && pemBlockBytes && decoded.decoded.size() % pemBlockBytes != 0)
        || envelope == EnvelopeStatus::Invalid) {
        out.error = tr("The encrypted private key container is malformed. "
                       "Copy the complete private key file.");
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
            || !readWireUint32(blob, offset, keyCount) || keyCount != 1
            || !readWireString(blob, offset, publicBlob)) {
            out.error = tr("The OpenSSH key container is malformed, so the file "
                           "was altered or truncated. Copy it again without "
                           "reformatting.");
            return out;
        }
        int blockBytes = 8;
        bool validEncryption = false;
        if (cipher == QByteArrayLiteral("none")) {
            validEncryption = kdfName == QByteArrayLiteral("none") && kdfOptions.isEmpty();
        } else {
            const bool aes = cipher == QByteArrayLiteral("aes128-cbc")
                || cipher == QByteArrayLiteral("aes192-cbc")
                || cipher == QByteArrayLiteral("aes256-cbc")
                || cipher == QByteArrayLiteral("aes128-ctr")
                || cipher == QByteArrayLiteral("aes192-ctr")
                || cipher == QByteArrayLiteral("aes256-ctr")
                || cipher == QByteArrayLiteral("aes128-gcm@openssh.com")
                || cipher == QByteArrayLiteral("aes256-gcm@openssh.com");
            const bool supportedCipher = aes || cipher == QByteArrayLiteral("3des-cbc")
                || cipher == QByteArrayLiteral("chacha20-poly1305@openssh.com");
            blockBytes = aes ? 16 : 8;
            qsizetype kdfOffset = 0;
            quint32 saltLength = 0;
            quint32 rounds = 0;
            if (supportedCipher && kdfName == QByteArrayLiteral("bcrypt")
                && readWireUint32(kdfOptions, kdfOffset, saltLength)
                && saltLength > 0
                && quint64(saltLength) <= quint64(kdfOptions.size() - kdfOffset)) {
                kdfOffset += saltLength;
                validEncryption = readWireUint32(kdfOptions, kdfOffset, rounds)
                    && rounds > 0 && kdfOffset == kdfOptions.size();
            }
        }
        if (!validEncryption) {
            out.error = tr("The OpenSSH key's encryption parameters are malformed "
                           "or unsupported. Choose a complete private key file.");
            return out;
        }
        // OpenSSH appends AEAD authentication data outside the private string's
        // declared length (sshkey_private_to_blob2 / cipher_authlen).
        const quint32 authenticationBytes =
            cipher == QByteArrayLiteral("aes128-gcm@openssh.com")
                    || cipher == QByteArrayLiteral("aes256-gcm@openssh.com")
                    || cipher == QByteArrayLiteral("chacha20-poly1305@openssh.com")
                ? 16 : 0;
        quint32 privateLength = 0;
        if (!readWireUint32(blob, offset, privateLength) || privateLength == 0
            || privateLength % blockBytes != 0
            || quint64(privateLength) + authenticationBytes
                != quint64(blob.size() - offset)) {
            out.error = tr("The OpenSSH key container has no complete private "
                           "key section, so the file was altered or truncated. "
                           "Copy the whole key again.");
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
#if CH_HAVE_LIBSSH
        ssh_key publicKey = nullptr;
        const QByteArray publicBase64 = publicBlob.toBase64();
        const int publicResult = ssh_pki_import_pubkey_base64(
            publicBase64.constData(), ssh_key_type_from_name(keyType.constData()),
            &publicKey);
        if (publicKey)
            ssh_key_free(publicKey);
        if (publicResult != SSH_OK) {
            out.error = tr("The OpenSSH key has a damaged or unsupported public "
                           "key section. Choose a complete private key file.");
            return out;
        }
#else
        out.error = tr("This build cannot validate private keys because SSH "
                       "support is unavailable.");
        return out;
#endif
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
    // Framing alone cannot validate a clear private key. Use the same parser as
    // authentication, with prompting disabled, before retaining or saving it.
    // Encrypted contents can only be validated once the user supplies a secret.
    if (!out.encrypted) {
#if CH_HAVE_LIBSSH
        ssh_key key = nullptr;
        const int result = ssh_pki_import_privkey_base64(
            normalized.constData(), nullptr, declineKeyPassphrase, nullptr, &key);
        bool matchesPublicKey = true;
        if (result == SSH_OK && key && openSsh) {
            char* publicText = nullptr;
            matchesPublicKey = ssh_pki_export_pubkey_base64(key, &publicText) == SSH_OK;
            if (matchesPublicKey) {
                matchesPublicKey = sha256Fingerprint(
                    QByteArray::fromBase64(QByteArray(publicText))) == out.fingerprint;
            }
            if (publicText)
                ssh_string_free_char(publicText);
        }
        if (key)
            ssh_key_free(key);
        if (result != SSH_OK || !matchesPublicKey) {
            out.error = tr("The private key is damaged or is not supported by "
                           "this build. Choose a complete private key file.");
            return out;
        }
#else
        out.error = tr("This build cannot validate private keys because SSH "
                       "support is unavailable.");
        return out;
#endif
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
    // The trusted-host store, at exactly the location SessionBootstrap's
    // constructor already derives, so handing this value to
    // setKnownHostsPath() changes nothing and documents everything.
    m_knownHostsPath =
        rootDirectory.isEmpty()
            ? QDir(QStandardPaths::writableLocation(
                       QStandardPaths::AppConfigLocation))
                  .filePath(QStringLiteral("known_hosts"))
            : QDir(rootDirectory).filePath(QStringLiteral("known_hosts"));

    // The saved-key directory (path 3). AppDataLocation and not
    // AppConfigLocation: this holds key material rather than configuration, and
    // the two are the same directory on Android anyway — the distinction that
    // matters is that both are private to this application. NOT created here;
    // ensureSavedKeyDirectory() creates it on the first save, so a user who never
    // saves a key leaves no keys directory behind at all.
    m_savedKeysDirectory =
        rootDirectory.isEmpty()
            ? QDir(QStandardPaths::writableLocation(
                       QStandardPaths::AppDataLocation))
                  .filePath(QStringLiteral("keys"))
            : QDir(rootDirectory).filePath(QStringLiteral("keys"));

    // Adopt what the last launch saved: NAMES only. The bytes are read when a
    // connect or a keyInfo() asks for them, so a store with ten saved keys holds
    // none of them in memory — and an entry whose name would not pass
    // isUsableKeyName() is ignored rather than offered, because this class never
    // wrote it and cannot build a path back to it through savedKeyPath().
    const QFileInfoList entries =
        QDir(m_savedKeysDirectory)
            .entryInfoList(QDir::Files | QDir::Hidden, QDir::Name);
    for (const QFileInfo& entry : entries) {
        const QString name = entry.fileName();
        if (isUsableKeyName(name))
            m_savedNames.append(name);
    }
    m_savedNames.sort();
}

MobileKeyStore::~MobileKeyStore()
{
    // Release held credentials with a best-effort overwrite of owned buffers.
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
    if (m_pool == pool)
        return;
    if (m_pool)
        m_pool->clearInMemoryIdentity();
    m_installedIdentityName.clear();
    m_pool = pool;
}

// ---------------------------------------------------------------------------
// listing and naming
// ---------------------------------------------------------------------------

QStringList MobileKeyStore::allKeyNames() const
{
    // Referenced first, then saved, then session-only: the credentials that
    // survived the last launch come before the one that has to be pasted again.
    QStringList names = m_referenceNames;
    names += m_savedNames;
    // A saved key is usually ALSO in m_keyNames — saveKeyOnDevice() keeps the
    // imported copy in memory — so the in-memory list is filtered rather than
    // appended wholesale. Two entries for one credential would let the connect
    // page offer the same key twice and let a "remove" act on the wrong one.
    for (const QString& name : m_keyNames) {
        if (!m_savedNames.contains(name))
            names += name;
    }
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
    if (m_memoryKeys.contains(name) || m_references.contains(name)
        || m_savedNames.contains(name)) {
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
    // Best-effort overwrite of the file-to-text conversion; the retained
    // normalized key and any parser/runtime copies have separate lifetimes.
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
    // App-private saved keys already have a stable name and deletion policy.
    // Re-adopting their path must not create a second, user-managed alias that
    // survives forgetting the saved copy.
    for (const QString& savedName : m_savedNames) {
        if (reference == savedKeyPath(savedName)) {
            setLastError(QString());
            return savedName;
        }
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
    // Names are shared by all three storage paths, including saved keys whose
    // bytes have not been loaded since launch. Keep suffixes within the same
    // 64-character bound as a name entered by the user.
    const auto nameTaken = [this](const QString& candidate) {
        return m_memoryKeys.contains(candidate) || m_references.contains(candidate)
               || m_savedNames.contains(candidate);
    };
    if (nameTaken(name)) {
        const QString base = name;
        for (int suffix = 2; suffix < 1000; ++suffix) {
            const QString tail = QStringLiteral("-") + QString::number(suffix);
            name = base.left(64 - tail.size()) + tail;
            if (!nameTaken(name))
                break;
        }
    }
    if (!claimName(name))
        return QString();

    m_references.insert(name, reference);
    m_referenceNames.append(name);
    m_referenceNames.sort();
    setLastError(QString());
    emit keyNamesChanged();
    return name;
}

QString MobileKeyStore::referenceFor(QString name) const
{
    name = name.trimmed();
    return m_savedNames.contains(name) ? savedKeyPath(name) : m_references.value(name);
}

// ---------------------------------------------------------------------------
// path 3: saved on this device (opt-in, per key)
// ---------------------------------------------------------------------------

QString MobileKeyStore::savedKeyPath(const QString& name) const
{
    // The name reaches the FILENAME verbatim, so validating it and building the
    // path are one act with one spelling. A name that fails here — "../evil",
    // "a/b", ".hidden", 400 characters — yields an empty string, and every caller
    // treats that as "refuse", so no unvalidated name can ever become a path.
    // QDir::filePath() would happily resolve "../evil" into a write outside the
    // app's own data directory.
    if (!isUsableKeyName(name))
        return {};
    return QDir(m_savedKeysDirectory).filePath(name);
}

bool MobileKeyStore::ensureSavedKeyDirectory()
{
    QDir directory(m_savedKeysDirectory);
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        setLastError(tr("Could not create the folder for saved keys, so the key "
                        "was not saved."));
        return false;
    }

    // mkpath() has no permissions argument and applies the process umask, so the
    // mode is set afterwards rather than at creation. That is a window, and it is
    // an acceptable one: the directory is empty until the write below, and the
    // FILE is what carries the key — its own permissions are set while it is
    // still the unnamed temporary QSaveFile holds.
    const QFile::Permissions ownerOnlyDirectory =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner;
    QFile::setPermissions(m_savedKeysDirectory, ownerOnlyDirectory);
#ifdef Q_OS_UNIX
    // VERIFIED, not assumed: setPermissions() can fail silently on a filesystem
    // that does not carry POSIX modes, and a directory anyone can list is not
    // where a private key goes. Only the group and other bits are checked, so a
    // platform that adds owner bits of its own is not treated as a failure.
    //
    // Unix only, and Android and iOS are both Unix. On Windows QFile::permissions()
    // synthesises the POSIX bits from an ACL and reports the group/other read bits
    // set for an ordinary user directory, so the same check there would refuse
    // every save on the desktop build this shell is developed against — while
    // saying nothing true about the ACL that actually governs access.
    const QFile::Permissions leaked =
        QFile::permissions(m_savedKeysDirectory)
        & ~(ownerOnlyDirectory | QFileDevice::ReadUser | QFileDevice::WriteUser
            | QFileDevice::ExeUser);
    if (leaked != QFile::Permissions()) {
        setLastError(tr("The folder for saved keys cannot be made private on "
                        "this device, so the key was not saved."));
        return false;
    }
#endif
    return true;
}

bool MobileKeyStore::saveKeyOnDevice(QString name)
{
    name = name.trimmed();
    // Validated BEFORE any filesystem call, and through the one function that
    // both validates and builds the path.
    const QString path = savedKeyPath(name);
    if (path.isEmpty()) {
        setLastError(tr("Give the key a name of 1 to 64 letters, digits, dots, "
                        "dashes or underscores."));
        return false;
    }
    if (m_savedNames.contains(name)) {
        setLastError(tr("\"%1\" is already saved on this device.").arg(name));
        return false;
    }
    if (!m_memoryKeys.contains(name)) {
        // A referenced key is refused rather than copied: path 2's whole promise
        // is that the key stays in the user's own storage, and duplicating it into
        // app storage behind a "save" switch would quietly break it.
        setLastError(m_references.contains(name)
                         ? tr("\"%1\" is a key file you manage yourself, which "
                              "is already remembered without copying it.")
                               .arg(name)
                         : tr("There is no loaded key by that name."));
        return false;
    }
    if (!ensureSavedKeyDirectory())
        return false;

    // QSaveFile: the bytes go to a temporary file in the same directory and are
    // renamed into place by commit(), so an interrupted save leaves no file
    // rather than half a key that fails to parse at the next launch.
    //
    // Permissions are set on the OPEN temporary file, before a single byte of key
    // material is written to it, and survive the rename — rather than on the
    // final path afterwards, which would leave the key readable for however long
    // the second call took.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setLastError(tr("Could not save the key (%1).").arg(out.errorString()));
        return false;
    }
    const QFile::Permissions ownerOnlyFile =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!out.setPermissions(ownerOnlyFile)) {
        out.cancelWriting();
        setLastError(tr("The saved key cannot be made private on this device, "
                        "so it was not saved."));
        return false;
    }

    // Borrow the retained bytes. Copying and then wiping would only detach and
    // zero a newly allocated buffer while the session's original stays live.
    const QByteArray& material = m_memoryKeys.constFind(name).value();
    const qint64 written = out.write(material);
    const bool complete = (written == qint64(material.size()));
    if (!complete || !out.commit()) {
        // commit() failed -> nothing was renamed into place. A partial write that
        // was never committed leaves the temporary file removed by QSaveFile
        // itself, so there is nothing here to clean up.
        out.cancelWriting();
        setLastError(tr("Could not save the key (%1).").arg(out.errorString()));
        return false;
    }

#ifdef Q_OS_UNIX
    // The same verification the directory gets, for the same reason and with the
    // same platform caveat: a key file the group or the world can read is worse
    // than a save the user has to be told did not happen, so a wider mode deletes
    // the file instead of leaving it there.
    const QFile::Permissions leaked =
        QFile::permissions(path)
        & ~(ownerOnlyFile | QFileDevice::ReadUser | QFileDevice::WriteUser);
    if (leaked != QFile::Permissions()) {
        QFile::remove(path);
        setLastError(tr("The saved key cannot be made private on this device, "
                        "so it was not saved."));
        return false;
    }
#endif

#ifdef Q_OS_IOS
    // iOS backs up the whole app container except <Caches> and <tmp>, so the file
    // has to opt out by name. A failure here deletes the file: a key that would
    // be copied into iCloud is not what the user agreed to keep on this device.
    QString backupError;
    if (!excludeFromDeviceBackup(path, &backupError)) {
        QFile::remove(path);
        setLastError(backupError.isEmpty()
                         ? tr("The key could not be kept out of the device "
                              "backup, so it was not saved.")
                         : backupError);
        return false;
    }
#endif

    m_savedNames.append(name);
    m_savedNames.sort();
    setLastError(QString());
    emit keyNamesChanged();
    return true;
}

bool MobileKeyStore::isSavedOnDevice(QString name) const
{
    return m_savedNames.contains(name.trimmed());
}

bool MobileKeyStore::deleteSavedKeyFile(const QString& name)
{
    if (!m_savedNames.contains(name))
        return false;
    // The name came out of m_savedNames, which only ever admits names that pass
    // isUsableKeyName(), so this cannot be empty — the check is here because a
    // path built from a name is the one thing in this class that must never be
    // done without it.
    const QString path = savedKeyPath(name);
    if (path.isEmpty())
        return false;
    if (!QFile::remove(path)) {
        // exists() also returns false when the parent cannot be searched.
        // Only a confirmed missing entry, not an access/IO error, is success.
#ifdef Q_OS_WIN
        const std::filesystem::path nativePath(path.toStdWString());
#else
        const std::filesystem::path nativePath(QFile::encodeName(path).constData());
#endif
        std::error_code error;
        const auto status = std::filesystem::symlink_status(nativePath, error);
        if (status.type() != std::filesystem::file_type::not_found
            || (error && error != std::errc::no_such_file_or_directory)) {
            return false;
        }
    }
    m_savedNames.removeAll(name);
    return true;
}

bool MobileKeyStore::forgetSavedKey(QString name)
{
    name = name.trimmed();
    if (!m_savedNames.contains(name)) {
        setLastError(tr("There is no key by that name saved on this device."));
        return false;
    }
    const bool deleted = deleteSavedKeyFile(name);

    // The in-memory copy goes too: "delete the key from this device" that left it
    // usable for the rest of the run would be a lie the user cannot see through.
    const auto inMemory = m_memoryKeys.find(name);
    if (inMemory != m_memoryKeys.end()) {
        wipeKeyMaterial(inMemory.value());
        m_memoryKeys.erase(inMemory);
        m_keyNames.removeAll(name);
    }
    // And the pool's copy, when this is the credential it is holding — same
    // reasoning as removeKey(), and the same restriction to the INSTALLED name so
    // deleting one key does not disarm the live session's other one.
    if (m_installedIdentityName == name) {
        if (m_pool)
            m_pool->clearInMemoryIdentity();
        m_installedIdentityName.clear();
    }
    if (m_armedPassphraseName == name)
        forgetPassphrase();

    if (!deleted) {
        setLastError(tr("The saved copy of \"%1\" could not be deleted from "
                        "this device.")
                         .arg(name));
        emit keyNamesChanged();
        return false;
    }
    setLastError(QString());
    emit keyNamesChanged();
    emit keyRemoved(name);
    return true;
}

// ---------------------------------------------------------------------------
// using and forgetting a key
// ---------------------------------------------------------------------------

bool MobileKeyStore::removeKey(QString name)
{
    name = name.trimmed();
    // A saved key is removed from the DEVICE, not just from the list: leaving the
    // file behind would resurrect at the next launch the very credential the user
    // just told this client to forget. The saved copy and the in-memory copy are
    // independent — saveKeyOnDevice() keeps both — so these are separate checks
    // rather than the either/or chain this function used to be.
    const bool wasSaved = m_savedNames.contains(name);
    const bool savedFileDeleted = wasSaved && deleteSavedKeyFile(name);
    bool known = wasSaved;

    const auto inMemory = m_memoryKeys.find(name);
    if (inMemory != m_memoryKeys.end()) {
        wipeKeyMaterial(inMemory.value());
        m_memoryKeys.erase(inMemory);
        m_keyNames.removeAll(name);
        known = true;
    } else if (m_references.remove(name) > 0) {
        m_referenceNames.removeAll(name);
        known = true;
    }
    if (!known) {
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
    // Everything above happened either way; the only thing that can still have
    // failed is the unlink, and that is the one failure the user has to know
    // about, because it is the one that leaves key material on the device.
    if (wasSaved && !savedFileDeleted) {
        setLastError(tr("The saved copy of \"%1\" could not be deleted from "
                        "this device.")
                         .arg(name));
        emit keyNamesChanged();
        return false;
    }
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

    // Not loaded, so the bytes come from whichever durable source this name has —
    // the user's own file through its reference, or the copy saved on this device.
    // Both are read HERE and nowhere else, and both are read afresh every time:
    // caching them would turn a durable credential into a session-long copy in
    // memory, which is the thing paths 2 and 3 are careful not to be.
    QByteArray raw;
    const auto reference = m_references.constFind(name);
    if (reference != m_references.constEnd()) {
        QString referenceError;
        raw = keyref::readReference(reference.value(), kMaxKeyBytes,
                                    &referenceError);
        if (raw.isEmpty()) {
            setLastError(referenceError.isEmpty()
                             ? tr("The referenced key file could not be read.")
                             : referenceError);
            return {};
        }
    } else if (m_savedNames.contains(name)) {
        const QString path = savedKeyPath(name);
        QFile in(path);
        if (path.isEmpty() || !in.open(QIODevice::ReadOnly)) {
            setLastError(tr("The key saved on this device could not be read, so "
                            "it cannot be used. Remove it and save it again."));
            return {};
        }
        // The same one-byte-past-the-bound read every other entry point does. The
        // file was written by this class, but the directory is not immutable and a
        // bound that is only enforced on input this class trusts is not a bound.
        raw = in.read(kMaxKeyBytes + 1);
        in.close();
        if (raw.size() > kMaxKeyBytes) {
            wipeKeyMaterial(raw);
            setLastError(tr("The key saved on this device is far too large to be "
                            "a private key, so it was not used."));
            return {};
        }
        if (raw.isEmpty()) {
            setLastError(tr("The key saved on this device is empty, so it cannot "
                            "be used. Remove it and save it again."));
            return {};
        }
    } else {
        return {};  // an unknown name: nothing to produce and nothing to explain
    }

    // Normalise through the same describeKeyText() the paste path uses, so a
    // source whose file has been replaced with something that is not a key is
    // caught here rather than inside libssh.
    //
    // Keep the UTF-16 conversion addressable for a best-effort overwrite after
    // parsing; this does not cover the parser's temporary copies.
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
    if (!m_memoryKeys.contains(name) && !m_references.contains(name)
        && !m_savedNames.contains(name)) {
        setLastError(tr("There is no loaded key by that name."));
        return false;
    }

    QByteArray material = keyMaterial(name);
    if (material.isEmpty())
        return false;  // keyMaterial() already said why
    m_pool->setInMemoryIdentity(material);
    // Release this temporary with a best-effort overwrite. The pool retains
    // its own identity for authentication/reconnect, and a pasted key remains
    // in m_memoryKeys until explicitly forgotten.
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
    // Two things survive, and neither is a session secret. References: dropping
    // them would force a fresh pick after every disconnect. Saved key FILES: the
    // user asked this device to keep those, and "disconnect" is not "delete my
    // key" — m_savedNames therefore stays as it is, and the names it holds are
    // still offered by allKeyNames() with their bytes still on disk and no longer
    // anywhere in memory.
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
    const bool saved = m_savedNames.contains(name);
    if (!referenced && !saved && !m_memoryKeys.contains(name))
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
        {QStringLiteral("saved"), saved},
        {QStringLiteral("reference"), referenceFor(name)},
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
    // Return an independent buffer so forgetting the parked value can overwrite
    // its storage without touching the result. Copies elsewhere are not covered.
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
