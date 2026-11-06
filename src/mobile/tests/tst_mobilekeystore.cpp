// Credential handling for the mobile client (SPEC 11.2, 12.1).
//
// Most of this file asserts a NEGATIVE, and that is why it exists: SPEC 11.2 is
// an exhaustive allowlist of what the client may store locally, and the only key
// material on it is "an opt-in, per-key copy in app-private storage, owner-only
// and excluded from OS backup" — a copy made by an explicit
// saveKeyOnDevice(name) and by nothing else. So every test below that imports,
// authenticates with, or forgets a key WITHOUT asking for it to be saved
// finishes by walking the store's whole storage root and proving that no file
// under it contains any part of the key or of the passphrase, and
// nothingReachesThePerAppSandboxEither() extends that walk to the per-app
// QStandardPaths directories — the place a mobile client would naturally put a
// keys directory, and a place a check confined to the store's own rootDirectory
// would never have looked.
//
// A positive-only suite would pass just as happily against an implementation
// that cached the key "just for this attempt" in a temp file, or that saved
// every imported key without being asked, which are exactly the compliance
// failures worth testing for.
//
// The saved-key cases in the last section are the mirror image: they use their
// OWN temporary app-data directory, precisely so that the file they expect to
// exist cannot make the leak scans elsewhere in this file — which share one
// m_root across the whole run — report it as a leak.
//
// The key material below is a throwaway ed25519 pair generated for this file
// alone; it authenticates nothing. Its fingerprint is `ssh-keygen -lf`'s own
// output, so the expectation is a known answer produced by OpenSSH rather than
// by the code under test.

#include "MobileKeyStore.h"
#include "ServerProfiles.h"

#include "SshConnectionPool.h"

#include <QtTest/QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

using ch::MobileKeyStore;
using ch::SshConnectionPool;

namespace {

// ssh-keygen -t ed25519 -N '' -C ch-test
constexpr auto kPlainKey = R"(-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW
QyNTUxOQAAACCFPLr0I6E8+7CRLJi8cjh4SciVWMEXlHzF/IIDXFBLIwAAAJDD6ACHw+gA
hwAAAAtzc2gtZWQyNTUxOQAAACCFPLr0I6E8+7CRLJi8cjh4SciVWMEXlHzF/IIDXFBLIw
AAAEB/HxJkLwbaZm48TCAx96WJuyrDUfAUyv9wZzQGfttws4U8uvQjoTz7sJEsmLxyOHhJ
yJVYwReUfMX8ggNcUEsjAAAAB2NoLXRlc3QBAgMEBQY=
-----END OPENSSH PRIVATE KEY-----
)";

// `ssh-keygen -lf` on the matching .pub, verbatim.
constexpr auto kPlainFingerprint =
    "SHA256:PitEpcPx5tdS93tsxqg3GiuFpvJrlWMle3KJ6R+lr1s";

// ssh-keygen -t ed25519 -N 'hunter2' -C ch-test-enc: same container, but the
// private half is sealed with bcrypt+aes256-ctr, and the PUBLIC blob still sits
// outside the encrypted section — which is why a fingerprint is available for a
// key nobody can use yet.
constexpr auto kEncryptedKey = R"(-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAACmFlczI1Ni1jdHIAAAAGYmNyeXB0AAAAGAAAABBTcp/M3c
nNn8Oi2dZiw75dAAAAGAAAAAEAAAAzAAAAC3NzaC1lZDI1NTE5AAAAIG6EvZuUuzjzQAl2
i6zHx+GGi4sIsm/e4YX+jRcSrliZAAAAkFIE5FM8xmyrcq/l69BabypTNlP65vuiNTz34Z
chBfcnKG4McfNNHZdw1TZeacl72vjmFrpAN63bN4HahCvsh+HgoDrgMHRMc39pEQT5EAAn
EqEzoQ7YsS3Tm6EYunq0XQL0Z8bTlNDGAnTWAZINad542hKZgGDVOkLtxEcWO9HhtJMzxN
oWZnUXt+al/XIEiA==
-----END OPENSSH PRIVATE KEY-----
)";

constexpr auto kEncryptedFingerprint =
    "SHA256:ttlpD1Nd/zTjn1PBsVuPFsk9VblszllpkxEskhumgFE";

constexpr auto kPublicKey =
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIIU8uvQjoTz7sJEsmLxyOHhJyJVYwReUfMX8"
    "ggNcUEsj ch-test\n";

constexpr auto kPassphrase = "correct horse battery staple";

// A distinctive slice of the key's base64 body. Searching for the whole armour
// would miss an implementation that stored the key re-encoded, so the needle is a
// run of body characters from ONE line of the armour — it survives a copy that
// re-wraps the block anywhere except inside these 35 characters.
QByteArray keyNeedle()
{
    return QByteArrayLiteral("yJVYwReUfMX8ggNcUEsjAAAAB2NoLXRlc3Q");
}

// Disposable 1024-bit RSA fixtures generated with OpenSSL, never authorized on a
// server. Keep real keys here: a base64 block is not necessarily a private key.
constexpr auto kPemBody =
    "MIICXAIBAAKBgQDhwW6hW+PU6JPZvQ5UBxW6GTH9aCqn9F71gM/+/KzzjJ4kqGdi\n"
    "xrGVKoiykxEdiClYPLyrW659kadjU11cBI6T2efbwQxXckd1lLqk23j7wL+lT4Dx\n"
    "792hDmVbmCdkiiyVT/0P/FW4l29aWrn8VrjYkSk89J7Q/UhaimhoupiCQQIDAQAB\n"
    "AoGAJYs4rccNXbQMGpLxVhITj+5uvNjc9y+Rtzxj2uEElkBVcH09LFUk5Uy8jQ4e\n"
    "M44Yah83tTu7ChD2CHOtvAbZ+vatnEZIMG1Fx1aQzDq3OrjY1gH6Oo/dC3tFeUYZ\n"
    "VOpZ7FaRX0A4DfYoymw778342K2+0UAeRd+XMQQAuR7WlJECQQD3FErLC6hr76nG\n"
    "kAnTz5d5VtfU+35rt8gQ+kOnJ8fwC4jXXuFbr5K8TapXN1QM5eqALVkpARQVJElx\n"
    "gSBrL7GtAkEA6egMmzuNbO94RcQhax/PVlW7xdHMoQY5YPyXYD+wDtK+5qTzeEnt\n"
    "nqeg0O5x2Cv7jurEUotwzr+XumpwUjwtZQJAWTUcY/8Lihz34Vc0FSOIfyjZuG4t\n"
    "ENtEAs05C+sTBxpDB++b0iysAAzSibbSQ+3nIuw8ms9m3tNBKRgECm8n6QJAIPI1\n"
    "bFwMBI8KYtFIX71gR+nVg/yUFQ46YMXTUJRH6R6YEwai4IuRM6qqxSMA2BEB4tSA\n"
    "iJvpbMWhynkZjwu43QJBANwITmt7F1DW1xCMHKYmK6pw0vbZa0VEVApVSTHO0DbA\n"
    "N3+BTUc/WOe2X1QaPJ6bY/SS4O2tbzldUfLqA8sU9tU=\n";

constexpr auto kEncryptedPemBody =
    "AU7sz4KSN25vYjFN7kIdN4BfBkZ9L52tefffE9Rjt+vsAv2Qm+WD3wQIjEz+ttOe\n"
    "o95q8B8PQOXbW/h7oPijDFCdyCrlLRj98pP7/v/Bm6j7Z/pP0QqIxjowWjSIEAyH\n"
    "weDo5DQSnXur0AEjJTzhK1bg3JCyTgJkANPxgR5cN9zHuSwNJ8k2Sjm2gr9dr0vb\n"
    "kQ0SammcaReSLyO05VusgW0R4fjfz7Fers7GqzSL3PJ3GGfdvjRvVcOFlBDgBfg+\n"
    "OsKlLha8VL9DGipguwEVoKwuPIxGBcPbcWSA2P1EmSOTueY7tpAwGMquUac8DFAG\n"
    "fDC1SSc81SZKDaAiXF/JfT50ljOQbr8MAQBvIvyFg0bvdst5Fv5tL266UyPvAiPg\n"
    "C8UNWLiatzSiWVLeC5O9+0sxoHyxhu4oHvw3I9VSqt3LkQjZ5GiRoek/2Jd+SQJB\n"
    "AwqoGOEflCTZCSWySldrynBfVjHOyem7EEVf68R9F2RqRg/NJsp6V8tP5G9HsVzp\n"
    "FBHAQwhdsIaBml2N5lFd8BLPm1vJKaNtNjKe6Mt94HtTjQlX4Kr8cx+49Y1yfQPU\n"
    "VaxwBEY7PVj455OP9Db/0JIK2AfDVIJE4OFtVEnL+PMhLNnXtl7+SygaCc2gI0zi\n"
    "wDKFonS2/qPT2DlAD1aNEWTwqRlV3eZvs5l3CNLQ1Fyrd2F/auDoNDsEx6KNx679\n"
    "Kj3zSzu8CVpFKLfFSNMdCUYsw4vylgUmTQ/eFSMCuuOKbr3orViQPOtVSdzye7V5\n"
    "0HXXqi4yu2sFbIAWCiOBYMo0j9K4BGr7Cd/kbSZE3DC088GABvXk5TF5yeANYPwf\n";

// OpenSSL pkcs8 -topk8 -v2 aes-256-cbc -iter 2048, password hunter2.
// A real disposable encrypted key, not an invented ASN.1/ciphertext envelope.
constexpr auto kEncryptedPkcs8 = R"(-----BEGIN ENCRYPTED PRIVATE KEY-----
MIIC5TBfBgkqhkiG9w0BBQ0wUjAxBgkqhkiG9w0BBQwwJAQQLmInhI54Abeq9hr8
JNLyKwICCAAwDAYIKoZIhvcNAgkFADAdBglghkgBZQMEASoEEGXs/IOm6jGqQZ+P
iRgxjgwEggKAkkvUJjxPq9prW4DfwyvAyk/AcLyUeoMKpa8w5sUIRKqz1X90oSvP
oni6pseZ4iKCGpVQXes8vheMeZgWDl5OuSQt61Jmm6WFKQGtVq3xmfU99vral1Zm
PKbvl083Ob7anOCsm3AtMWbdp2lM8kmKK/FdNQ+2u7ktIK/7njr7Vv0WeC7I63ZE
oS9I46pSYS22/azP2qOd2+YJRsa80tDqQxw+6V4G3kZjDZAzMX26RbEDx/ofnfFA
Vonom/Wf2TVtdDRsMJWefDQEtlyYc9RcY1U7y/sJ8nvd2ga2WgEw4YUac5f5Y4Sj
AaaIOn/8N24RlFoTYMqsZvcwwgVGggNNtxgacimz35EALahu9Zp3Y2glGwyqLbOz
tCmjIHbKReDR3hqiUdAnwPnY6kdukyUWrjdUCdC9qHv02gCA6qLOfRYRkr8ehPyx
m4REueK7/oMYQauVaM6eGGtYYsydFHF7RTxfohicey75eeOLCcoAYTJVSe7dVHeN
14f0nj0nhBCtsH077+UYrpMxQnlLIHVToF+/9pqbnbtUfKYT0Sj6Ol32HXMP2ERB
l6j/BzviH56ud4OOAW551vuqokVbR8vz+ocQs0JGFw/fldHfR5e+fqT/HLH+fAxg
062M23ZEcw3hAvs5l/rO7WdZvc8LOPKtmozJUqIYmCQMGfu5/4oZSt4g+jTRedHX
kjkA54pOnXDBAEJTvfUfuaotLtln3pveBn8vPamjdEXsutf4IGPyNV0q7sQIL8YC
JmpYAOQ2rvH1oecvcjE9w31fJ0JlGU+9PpTP4v0Ua/S6m5TCKg/2SO3bvO3+h4I0
j1RyFf9DVsYdPtSmKj0pTm1n/bmGxFGH6A==
-----END ENCRYPTED PRIVATE KEY-----
)";

// OpenSSL pkcs8 -topk8 -scrypt, generated from disposable RSA-2048 material.
constexpr auto kEncryptedScrypt = R"(-----BEGIN ENCRYPTED PRIVATE KEY-----
MIIFLTBXBgkqhkiG9w0BBQ0wSjApBgkrBgEEAdpHBAswHAQQXa8yFGCZXcPky1/g
1ixCLQICQAACAQgCAQEwHQYJYIZIAWUDBAEqBBAKZBkIKoKvbbF7SYax25N5BIIE
0KAcGs3sCRHBL+ZSrbabmiKm711mgC8zjqd0Z5Ef0xCdTlwvGWF8Pmn28Qa1UTm8
3iWlXWmYX8Qo0OgQ/ycGTmxJRkdj5bnINMhR3OxJyj/Stz9mNvkna9eMpo5OTW69
bnTELQCi2YdupJ7+8sLomOTHy14tgQXZdokku/yMkwTbirUaF+XqMTX8oI+eZkAU
VpiJbMV6VcRcsE93JQE6EzENi3Zgc+rC+XCO+lHEMC18AUQGRUbrsAuBRZDWao34
52XNrQS6RTqRtJh/2oIk6AMvswRj1xE52uOFFVqaqSB3Gd1FVdP1f6lkrXO58bCn
yiO8Uw268/HjauZHv4G3tWX8WfCHgWdQI1cuOcEzHMyQMhlPdrLI+sU94K+x3HZA
kLqSeC2qI6xM8k7PSQVqrc3jwS/4ecqnF2v0IFnH1/4zg60gY5JUQmxl0DQ4SaOv
zSrF8M5lHYUyzhxufNktrANFWk0tzAXUXDHITVkOwCY4LHktCqFEL0a8lyd7bhLK
FiyG8L2N9+73QEaUhMwW0oqQZBAsodjjQuf1OeQR6gyyvDbD9PLODH/GKfCFr4Tx
B4Vce2xPTLtPkD9V60sd62I65X3I+Vb1uRkXM0pVncULgNWaIS5ePSjPLVE0p7KP
r+K47zM4faC0k9yh+HEAYpT+HzpJ+KRpLvxDqOPjw36E1vAPxkuqxY9CFvLy8Zy3
RoEep3cy3o1qhh8/46EPdq6pL85HGnqJDo9DOm1e8k0RXWicqcqHsF6GDti5sc9J
T3QWZcs0636UXY/dw+G0W5dTXf08RSbXHP5uopxhkCfHKCiddlciyZT/wZXP0i3z
+780c13zdWCB4bN43m+XwhfegK9/j9BxlLXMcK5nkC4DIzQfM+TlwDr1CYCilEq+
Y+8czyVmuNnhRXyCzW+8idAsnnjABnThjBpRTpaijl7iXdzw5Vp/q18wAdTgdVfd
OeMf05F4ku2Q4W6spu91QzgunB9s2zo1xW+26ehhSqjWdNMKaRZK/QzMbcwJOdIS
jNMX/iwqKmN/qpkN7kO6K9cdZUX6iE2EuLEZ6wE38ys0oonMryrJcBY0VHZbqGTk
rbcldZVyNzsEJ0RXW0yUr6yI1qGq3MEtujVFB1RImlF/Ho79WOwtrm8gwNWsOksE
LSE205wyUJw1NZ6X4bzO4dv858vLrrYX/vmgyALLaei/qdsIXsY/IzsvAFHLKAhu
u+JraExwuZK3IZPCspyQUtpngpLDOOt+r6Fzd17i+yQRGRdN5JDWbGqDh7BL+fWz
zIQfC1r5OHkuBTPTS0TNVVqaHXCMg2ABPKHnJxae/SG3alCCfEB7q+6JnmdcUDGZ
kz/CXjBmXvKru4OWRO/+tYqh1lMJBjEuEea8ARdln8YQEhabze1G/9drU9nvNulg
H4CP119sGZRnjqNgLARm4Mv/rBrZYPBWy6fYLKzkAWLZfiV2Sa1QAf0skBjOiUq/
NjVl2wFF8RWMUooblh6JGY5bBXb1uIYLPIXZYf4Z4GPxS1Rz9us7eqzxAFEbbfur
ZYdO/w8rKa4YlW6h0P3XYsysPFhwpzdmZcdYNZTBURNM2JqnH6sMFakdThC2Rb6z
akG9sNhPuqsJYF2P9EEN1jUwbGYdFsKMyL/WUOnm0rYC
-----END ENCRYPTED PRIVATE KEY-----
)";

// OpenSSL-generated legacy PBES1 MD5/DES and PKCS#12 SHA1/3DES containers.
// Password hunter2; fixtures are never authorized on a server.
constexpr auto kEncryptedPbes1 = R"(-----BEGIN ENCRYPTED PRIVATE KEY-----
MIICoTAbBgkqhkiG9w0BBQMwDgQIHhrAcmqINV8CAggABIICgCwAaJGZev6oanhu
DNs7JW8S6mKpdjr7OPpBoHBZlcyPEr3WN2QaOA/5RKHdoTUB8cBWgf8QVPrv7Taa
MZgg4WaOswTjHdxre2BUCy/nCa+Lx4Y9u+MAqjmQtlh5rVj+5iKey4zhmnFhzcck
RCO91Mb5PrnsLketCe6swvBF073NSPeVMU7pU27QBfidBF1lYinAFDZM1+8D32C5
089dZu/gY7l3CWuI91YEtmPD3ilTjgA7CsyqeImKELkePU4AhX8xSoaz2evaZuRA
uCKzoL/yX8ktg38CdOZO2hdopdCXXhU0rWxZnDWvtk/fgFpyK0H9ZWibmzKL668L
56GB6feqxf7NHXXHrP14mSNGvzPfp2zbMk6lllA+/bl94k+AYz+OZYs91LgScalj
Uk+v7EP43QbHyrrMHJSwyhQud9daQDFdMnx9gkSlh92syXdHA8Jm/ZSfYmsXttNR
92aluLTqoS7QQsOLTm/PaQ1rkPvNXUIty1YjU8fsCNZM9j0AEdSulzTkSgtBvxDt
sQOuI1A7zT/1aS8cWxeqeoifuQTTcZJnEvfe+IjmI7jMFs8TZYEGx1OxpMM3XykO
g0kVOOSI1TAfddBBclbzl2fSmIiNEfN+wgNl53jxQlhJxD/9M4z2lnnx58cIlrR6
ASJsag67F/uZBbyqurXcRu1nFB8/qs9rAtlR5onRa9NGUr7wyFMuPKTR6WaUpPos
od6Y3lg/RqQyOKCVWbUla0f4iN1eZLPfO3/8foDVyknt9YkvCh3+/7WNKVeC/I/Y
DGBbcOKcMT2gkwlTaSHEJUinWdCNo55CBGj/BLXAMPEh/TZ8fHhpgJ74lEihqCOO
HgQ2NDQ=
-----END ENCRYPTED PRIVATE KEY-----
)";

constexpr auto kEncryptedPkcs12Pbe = R"(-----BEGIN ENCRYPTED PRIVATE KEY-----
MIICojAcBgoqhkiG9w0BDAEDMA4ECEsvrOgGU9czAgIIAASCAoD3Y7qsIhCAec8l
MFPg+UKIc8DA4ZpGBsXFyjgvIIA+LlZfmZXHRBk7+z3qELo2JE+8WKO9ZaNWqdIb
w+7q9vjgu7OIUJ3r5HxEBrJPtKxoV7VuZKsW+hxd6Xk2Fni5ii6kE/AnaZG5b311
cGt440/kVpgWcbrutKkdXs9DgInl/9g/PN/KKXWk8hqvOENuJNAkXd+cTGTqhul5
NT/L2jTCTAOJ0dyf+Nby2kMvJi+xl7sSsomnXXF6PcZzCwp/040/T6iuYYZcKKou
e9hvHR+G3H42DsBrWshF0TH3qXNGC6eo+bf8PKumrY62ak9nBKV5+VhGmqdjhGly
fKx0JOSiEPY/2C1mK26KqyiT7UMUJN1CzGh8Sa3aiLO8wiXjy0BqZnlXQdlqWkFR
BonA0wf67T7l5eCexwVX8owJmYItbAmT7JzD3gIWfxuB8b593y+tX7m4aSGcwQRU
Reu6m8Ajeig2YE76HQiZ49c/l6hREcAGO9unCos2Ra1roEJ0hN0BrdPvqdNIJ3me
JA44YZNqhfrjLnTEvx0b+TMapxp1GXV7tAJO2pbyNAS+fwJ8Yi6SJAkD5gRCufbF
GmNVUJMuhRo/w4UcOIf+0kFvbhBCI4HjQ48pX88TJv328r2WFnkH+Sj2LAbQ3zOV
4TjeLnT1x9t9vMrps0uCA/3oZMVQoC8ml/nDGl+7hnoD/OFd65eE9ifT+RJoRzp2
3OlRxhefMLW/P7cX95HQ9ZG8wXs9qVQMniy7x8+lJQ3hfIkcsJ3+/Bujjhg/YsYu
fBQ+UswpvVd4qmJwKteHwfug1t/BG220fxsL3nFeQGSyrG243VauyULdcItsz+iT
b/hscQGl
-----END ENCRYPTED PRIVATE KEY-----
)";

QString legacyPemKey(const QString &headers = QString())
{
    return QStringLiteral("-----BEGIN RSA PRIVATE KEY-----\n") + headers
           + QString::fromLatin1(headers.isEmpty() ? kPemBody : kEncryptedPemBody)
           + QStringLiteral("-----END RSA PRIVATE KEY-----\n");
}

// The per-app sandbox directories a mobile client could plausibly write to.
// tst_mobilekeystore runs with QStandardPaths test mode on, so these resolve
// inside the test's own temporary tree rather than the developer's home — which
// is what makes scanning them safe AND makes the scan meaningful: a store that
// derived a keys directory from QStandardPaths instead of from its rootDirectory
// would land here, where a leak check that only walked rootDirectory would never
// look.
QStringList sandboxRoots()
{
    QStringList roots;
    // TempLocation is deliberately NOT among these. QTemporaryDir puts this
    // test's own fixture — including the user-managed key file the reference path
    // is supposed to read — inside it, so scanning it would report the fixture as
    // a leak. That the store writes no temporary file is instead a structural
    // fact: it opens no file for writing anywhere, and the rootDirectory scans
    // elsewhere in this file cover the one directory it does derive.
    for (const QStandardPaths::StandardLocation location :
         {QStandardPaths::AppDataLocation, QStandardPaths::AppLocalDataLocation,
          QStandardPaths::AppConfigLocation, QStandardPaths::CacheLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty() && !roots.contains(path))
            roots.append(path);
    }
    return roots;
}

// Every regular file anywhere under `root`, so the leak check cannot be fooled
// by a subdirectory created on demand.
QStringList filesUnder(const QString &root)
{
    QStringList out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        out << it.next();
    out.sort();
    return out;
}

// Fails the calling test with the offending path when any file under `root`
// contains `needle`. Reads bytes, not text: a leak does not have to be UTF-8.
void assertNoFileContains(const QString &root, const QByteArray &needle,
                          const char *what)
{
    const QStringList files = filesUnder(root);
    for (const QString &path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray contents = file.readAll();
        if (contents.contains(needle)) {
            QFAIL(qPrintable(QStringLiteral("%1 reached disk at %2")
                                 .arg(QString::fromLatin1(what), path)));
        }
    }
}

// assertNoFileContains() over several roots, for the claim that has to hold
// everywhere the client could write rather than only under the store's own
// directory. QFAIL inside the callee still fails the calling test.
void assertNoFileContainsAnywhere(const QStringList &roots,
                                  const QByteArray &needle, const char *what)
{
    for (const QString &root : roots)
        assertNoFileContains(root, needle, what);
}

// The saved-key directory as SPEC 11.2 and MobileKeyStore.h document it:
// <app data>/keys, one file per key named exactly as the key is. Spelled out
// here rather than asked of the store, so a change to the layout has to be a
// deliberate change to this expectation too — a test that asked the store where
// it put the key could not catch it putting it somewhere else.
QString savedKeysDirectory(const QString &appData)
{
    return QDir(appData).filePath(QStringLiteral("keys"));
}

QString savedKeyPath(const QString &appData, const QString &name)
{
    return QDir(savedKeysDirectory(appData)).filePath(name);
}

// assertNoFileContains(), minus the files the key is ALLOWED to be in. Used by
// the saved-key cases to make the strong claim rather than the weak one: not
// merely "the key is in the saved file", but "the saved file is the ONLY place
// under this app's storage that the key reached" — which is what covers
// QSettings, a log, a cache and a stray temporary in one assertion.
void assertNoFileOutsideContains(const QString &root, const QString &allowedDir,
                                 const QByteArray &needle, const char *what)
{
    const QStringList files = filesUnder(root);
    for (const QString &path : files) {
        if (path.startsWith(allowedDir + QLatin1Char('/')))
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        if (file.readAll().contains(needle)) {
            QFAIL(qPrintable(QStringLiteral("%1 reached disk at %2")
                                 .arg(QString::fromLatin1(what), path)));
        }
    }
}

}  // namespace

class TestMobileKeyStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // Pure classifiers, no filesystem involved at all.
    void describesAnOpensshKey();
    void describesAnEncryptedKey();
    void diagnosesAPublicKeyByName();
    void rejectsTextThatIsNotAKey();
    void rejectsUnusableNames();
    void refusesATruncatedOpenSshPrivateSection();
    void importsAuthenticatedCipherKeys();
    void refusesAnOversizeKey();

    // Path 1: in-memory import.
    void importsAKeyWithoutTouchingDisk();
    void refusesADuplicateName();
    void reportsAnEncryptedKeyAsSuch();
    void referenceForAnInMemoryKeyIsEmpty();
    void forgetsAnImportedKey();

    // Path 2: a reference to a user-managed file.
    void referencesAUserManagedFileWithoutCopyingIt();
    void reAdoptsAReferenceFromAProfile();
    void refusesAReferenceThisDeviceCannotResolve();
    void referencedKeyMaterialIsReadOnDemandOnly();

    // Using and forgetting a credential.
    void installsAnInMemoryIdentityOnThePool();
    void installsAReferencedIdentityOnThePool();
    void anEmptyNameClearsThePoolIdentity();
    void removingTheInstalledKeyDisarmsThePool();
    void changingPoolsDisarmsThePreviousIdentity();
    void forgetSessionDropsEveryStoredSecret();
    void aReferenceIsNeverTreatedAsAnIdentityFilePath();

    // One-shot passphrases (SPEC 12.1).
    void takingAPassphraseErasesIt();
    void forgetPassphraseDropsItUnused();
    void noPassphraseEverReachesDisk();

    // Formats and damaged input.
    void describesALegacyPemKey();
    void reportsAnEncryptedLegacyPemKey();
    void describesARealEncryptedPkcs8Key();
    void rejectsMalformedEncryptedPemContainers();
    void rejectsMalformedPbkdf2Parameters();
    void rejectsMalformedOrUnsupportedPbeEnvelopes();
    void rejectsMalformedScryptParameters();
    void rejectsInconsistentOpenSshEncryptionParameters();
    void rejectsIncompletePemEncryptionHeaders();
    void rejectsInvalidClearPrivateKeys();
    void refusesOversizePastedText();
    void normalisesCrlfAndSurroundingText();
    void rejectsAnAlteredBase64Body();
    void acceptsAFileExactlyAtTheSizeCap();

    // References that are not what they claim.
    void refusesADirectoryReference();
    void registerReferenceRepairsAnUnusableName();
    void registerReferenceDoesNotShadowASavedKey();
    void savedKeyReferenceRoundTripsWithoutAnAlias();
    void savedKeyReferenceRefusesAMissingFile();
    void registerReferenceSuffixFitsAMaximumLengthName();
    void registerReferenceRefusesAnEmptyReference();
    void refusesAReferenceWhoseFileStoppedBeingAKey();

    // What the UI is allowed to learn.
    void describeTextNeverReturnsKeyBytes();
    void keyInfoForAnUnknownNameIsEmpty();
    void sessionOnlyFallbackIsOfferedOnlyForADurabilityFailure();
    void nothingReachesThePerAppSandboxEither();

    // Path 3: saved on this device, opt-in per key. Each of these uses its own
    // temporary app-data directory, so a file that is SUPPOSED to exist is never
    // seen by the leak scans above.
    void savesAPastedKeyForTheNextLaunch();
    void aSavedKeyIsOwnerOnlyAndSoIsItsFolder();
    void savingIsNeverImplicit();
    void refusesToSaveAKeyItWouldHaveToBuildAPathFor();
    void refusesToSaveAReferencedKey();
    void forgetSessionKeepsASavedKeyAndWipesTheSessionOne();
    void forgetSavedKeyDeletesTheFileFromDisk();
    void removeKeyDeletesTheSavedFileToo();
    void aFailedSavedKeyDeletionCanBeRetried();
    void forgettingAnAlreadyRemovedSavedKeySucceeds();
    void anUnsearchableSavedKeyDirectoryDoesNotImplyDeletion();
    void savingAKeyPutsNothingInQSettings();

private:
    // Storage root the store is confined to, so the leak checks have a closed
    // world to walk. The store writes nothing here by design; the directory
    // exists precisely so "nothing" is a checkable claim.
    QTemporaryDir m_root;
    // A key file OUTSIDE the store's root, standing in for a document the user
    // manages themselves.
    QTemporaryDir m_userStorage;
    QString m_userKeyPath;

    QString root() const { return m_root.path(); }
};

// QStandardPaths test mode redirects AppDataLocation and friends into the test's
// own temporary tree. Two things depend on it: the store's default constructor
// (used by no test here, but a future one must not scribble a known_hosts file
// into the developer's real config directory), and the sandbox scan below, which
// walks those very directories looking for key material and must not walk the
// user's actual home.
void TestMobileKeyStore::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void TestMobileKeyStore::init()
{
    QVERIFY(m_root.isValid());
    QVERIFY(m_userStorage.isValid());

    m_userKeyPath = m_userStorage.filePath(QStringLiteral("id_ed25519"));
    QFile key(m_userKeyPath);
    QVERIFY(key.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(key.write(QByteArray(kPlainKey)),
             qint64(qstrlen(kPlainKey)));
    key.close();
}

void TestMobileKeyStore::describesAnOpensshKey()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    const auto description =
        MobileKeyStore::describeKeyText(QString::fromLatin1(kPlainKey));
    QVERIFY(description.valid);
    QVERIFY(description.error.isEmpty());
    QCOMPARE(description.format, QStringLiteral("openssh-key-v1"));
    QCOMPARE(description.keyType, QStringLiteral("ssh-ed25519"));
    QVERIFY(!description.encrypted);
    QVERIFY(description.kdf.isEmpty());
    QCOMPARE(description.fingerprint, QString::fromLatin1(kPlainFingerprint));
    // The normalised bytes are what libssh is handed: the block itself, ending
    // in a newline, with nothing the user pasted around it.
    QVERIFY(description.normalized.startsWith("-----BEGIN OPENSSH PRIVATE KEY-----\n"));
    QVERIFY(description.normalized.endsWith("-----END OPENSSH PRIVATE KEY-----\n"));
}

void TestMobileKeyStore::describesAnEncryptedKey()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    const auto description =
        MobileKeyStore::describeKeyText(QString::fromLatin1(kEncryptedKey));
    QVERIFY(description.valid);
    QVERIFY(description.encrypted);
    QCOMPARE(description.kdf, QStringLiteral("bcrypt"));
    // The public half lives outside the sealed section, so this is knowable
    // while the key itself is still locked — that is what lets the import sheet
    // show a fingerprint before asking for the passphrase.
    QCOMPARE(description.keyType, QStringLiteral("ssh-ed25519"));
    QCOMPARE(description.fingerprint,
             QString::fromLatin1(kEncryptedFingerprint));
}

void TestMobileKeyStore::diagnosesAPublicKeyByName()
{
    // Handing over the .pub file is the mistake users actually make, so it must
    // be named rather than swept into a generic refusal.
    const auto description =
        MobileKeyStore::describeKeyText(QString::fromLatin1(kPublicKey));
    QVERIFY(!description.valid);
    QVERIFY(description.error.contains(QStringLiteral("public"),
                                       Qt::CaseInsensitive));
    QVERIFY(description.normalized.isEmpty());
}

void TestMobileKeyStore::rejectsTextThatIsNotAKey()
{
    for (const QString &text : {QStringLiteral(""),
                                QStringLiteral("   \n\t\n"),
                                QStringLiteral("hunter2"),
                                QStringLiteral("-----BEGIN OPENSSH PRIVATE KEY-----\n")}) {
        const auto description = MobileKeyStore::describeKeyText(text);
        QVERIFY2(!description.valid, qPrintable(text));
        QVERIFY(!description.error.isEmpty());
        // A refused paste may still be key material, and an error message is a
        // thing that gets logged: it must never quote the input.
        QVERIFY(!description.error.contains(QStringLiteral("hunter2")));
        QVERIFY(description.normalized.isEmpty());
    }
}

void TestMobileKeyStore::rejectsUnusableNames()
{
    QVERIFY(MobileKeyStore::isUsableKeyName(QStringLiteral("work-laptop")));
    QVERIFY(MobileKeyStore::isUsableKeyName(QStringLiteral("id_ed25519")));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QString()));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QStringLiteral(".hidden")));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QStringLiteral("has space")));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QStringLiteral("../escape")));
    QVERIFY(!MobileKeyStore::isUsableKeyName(QString(65, QLatin1Char('a'))));
}

void TestMobileKeyStore::refusesAnOversizeKey()
{
    // A picker must not be usable to pull an arbitrary file into memory.
    MobileKeyStore store(root());
    const QString fat = m_userStorage.filePath(QStringLiteral("fat.key"));
    QFile file(fat);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QByteArray(MobileKeyStore::kMaxKeyBytes + 1, 'a')) > 0);
    file.close();

    QVERIFY(!store.importKeyFromFile(QUrl::fromLocalFile(fat),
                                     QStringLiteral("fat")));
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store.keyNames().isEmpty());
}

void TestMobileKeyStore::refusesATruncatedOpenSshPrivateSection()
{
    QByteArray encoded;
    for (const QByteArray &line : QByteArray(kPlainKey).split('\n')) {
        if (!line.startsWith("-----"))
            encoded += line.trimmed();
    }
    const QByteArray container = QByteArray::fromBase64(encoded);
    qsizetype offset = QByteArrayLiteral("openssh-key-v1").size() + 1;
    for (int field = 0; field < 3; ++field)
        offset += 4 + qFromBigEndian<quint32>(container.constData() + offset);
    offset += 4; // key count
    offset += 4 + qFromBigEndian<quint32>(container.constData() + offset);

    const QList<QByteArray> damaged{
        container.first(offset), // public half only, no private length
        container.chopped(1), // private bytes shorter than their declared length
        container.first(offset) + QByteArray(4, '\0'), // empty private half
        container + QByteArrayLiteral("trailing bytes"),
    };
    MobileKeyStore store(root());
    for (const QByteArray &blob : damaged) {
        const QString text = QStringLiteral("-----BEGIN OPENSSH PRIVATE KEY-----\n")
            + QString::fromLatin1(blob.toBase64())
            + QStringLiteral("\n-----END OPENSSH PRIVATE KEY-----\n");
        QVERIFY(!store.importKeyFromText(QStringLiteral("damaged"), text));
        QVERIFY(!store.lastError().isEmpty());
        QVERIFY(store.keyNames().isEmpty());
    }
}

void TestMobileKeyStore::importsAuthenticatedCipherKeys()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // Real ssh-keygen -t ed25519 -N mc-regression -Z <cipher> keys. OpenSSH
    // successfully decrypted each fixture; these are throwaway test identities.
    const char *keys[] = {
        R"(-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAAFmFlczI1Ni1nY21Ab3BlbnNzaC5jb20AAAAGYmNyeXB0AA
AAGAAAABAgQGJZpu/Ig1BKOcwUfX7rAAAAGAAAAAEAAAAzAAAAC3NzaC1lZDI1NTE5AAAA
ICkUg4ayjm5iaaR7moNjdJYjjp6atDZSps61y7Xhi/MAAAAAkLzVqriHuK0zedC+rDbKEr
JZrCyTIYlJOsInFp7rfjgOyt3p8zpGY7iYKM9DUpSTU+rOGpsWLaLDIF5wpUl+zYDQ0vX4
eJrrmFh+30DtL2P9+KaJCaR5sVv3lSnZgc3cdXNnXfjvvM5em9YZsUEeY/z0jUfHlWYdy4
LiLm44/w7KdfUVSQKpfufd0VTNXsTDqqYgWzCw76ImDdXk3pLBvhY=
-----END OPENSSH PRIVATE KEY-----
)",
        R"(-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAAFmFlczEyOC1nY21Ab3BlbnNzaC5jb20AAAAGYmNyeXB0AA
AAGAAAABDor/u+hOaBAB+e0vcj+QHtAAAAGAAAAAEAAAAzAAAAC3NzaC1lZDI1NTE5AAAA
IIyJmlk7BgC3H96lYLFDQ+pE/HHlhLeoaJWuYgCzEnmRAAAAkOq/sknzhZiX7rJEoDET4L
NMeefjX2z8ajDlMQ6gPJpBlLUh0lQdputm3oDulWgadlGJyjdhrlLZhUs6TZOq3agd2LyY
zif9OQ8k0gwPR/niwp7CB/iNyI2Mvcm1QTT6KgQamRt3dO+okjBhzvaVabaFjzOq3ljvWq
rDVh/QU2nPMi9lFqVucD9vGBow/WamCpY99k+MkxGO1hsq+p0xV00=
-----END OPENSSH PRIVATE KEY-----
)",
        R"(-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAAHWNoYWNoYTIwLXBvbHkxMzA1QG9wZW5zc2guY29tAAAABm
JjcnlwdAAAABgAAAAQ+7kDnht8ZeDRa8m4sZgbLwAAABgAAAABAAAAMwAAAAtzc2gtZWQy
NTUxOQAAACDC0oSRYEffsgjPSEKhdiBHiS8CVzdgz0iLWlwDnmVx+QAAAJDkf7LF24ISPF
ukJ1dlCpQNeuO3SLg0+mmueSqBj2/D1BPCQJla3FmBRZeFtL3MUY85lgEdT13C3B7PNlLC
67a8usBw4XGuVq6cZ9TeNc8MumnJQUxEDbsx6pz7k1ZnTnZRwIn+20rFNauQdHyMoPNpSv
B110slVf6dCaidJpml/hFbfOs9vz3h2h3h5NrPXO0hD9EEwbpEgUR1RPURqYHc
-----END OPENSSH PRIVATE KEY-----
)",
    };
    MobileKeyStore store(root());
    int index = 0;
    for (const char *key : keys) {
        const QString name = QStringLiteral("aead-%1").arg(++index);
        QVERIFY2(store.importKeyFromText(name, QString::fromLatin1(key)),
                 qPrintable(store.lastError()));
        const QVariantMap info = store.keyInfo(name);
        QVERIFY(info.value(QStringLiteral("encrypted")).toBool());
        QVERIFY(info.value(QStringLiteral("fingerprintAvailable")).toBool());

        QByteArray encoded;
        for (const QByteArray &line : QByteArray(key).split('\n')) {
            if (!line.startsWith("-----"))
                encoded += line.trimmed();
        }
        const QByteArray container = QByteArray::fromBase64(encoded);
        for (const QByteArray &damaged :
             {container.chopped(1), container.chopped(16),
              container + QByteArrayLiteral("trailing bytes")}) {
            const QString text = QStringLiteral("-----BEGIN OPENSSH PRIVATE KEY-----\n")
                + QString::fromLatin1(damaged.toBase64())
                + QStringLiteral("\n-----END OPENSSH PRIVATE KEY-----\n");
            QVERIFY(!store.importKeyFromText(QStringLiteral("damaged"), text));
            QVERIFY(!store.keyNames().contains(QStringLiteral("damaged")));
        }
    }
}

void TestMobileKeyStore::importsAKeyWithoutTouchingDisk()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    MobileKeyStore store(root());
    const QStringList before = filesUnder(root());

    QSignalSpy imported(&store, &MobileKeyStore::keyImported);
    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QCOMPARE(store.keyNames(), QStringList{QStringLiteral("pasted")});
    QVERIFY(store.hasKeys());
    QCOMPARE(imported.count(), 1);
    QCOMPARE(imported.at(0).at(1).toBool(), false);  // not a reference
    QCOMPARE(store.keyFingerprint(QStringLiteral("pasted")),
             QString::fromLatin1(kPlainFingerprint));

    // The whole point: an in-memory import performs no filesystem work.
    QCOMPARE(filesUnder(root()), before);
    assertNoFileContains(root(), keyNeedle(), "an imported key");
}

void TestMobileKeyStore::refusesADuplicateName()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("dup"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(!store.importKeyFromText(QStringLiteral("dup"),
                                     QString::fromLatin1(kEncryptedKey)));
    QVERIFY(!store.lastError().isEmpty());
    // The first key is still the one under that name.
    QCOMPARE(store.keyFingerprint(QStringLiteral("dup")),
             QString::fromLatin1(kPlainFingerprint));
    QCOMPARE(store.keyNames().count(), 1);
}

void TestMobileKeyStore::reportsAnEncryptedKeyAsSuch()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // An encrypted key must import: the UI needs it in hand to know a
    // passphrase is required, rather than discovering that at handshake time.
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("locked"),
                                    QString::fromLatin1(kEncryptedKey)));

    const QVariantMap info = store.keyInfo(QStringLiteral("locked"));
    QCOMPARE(info.value(QStringLiteral("encrypted")).toBool(), true);
    QCOMPARE(info.value(QStringLiteral("kdf")).toString(),
             QStringLiteral("bcrypt"));
    QCOMPARE(info.value(QStringLiteral("referenced")).toBool(), false);
    QCOMPARE(info.value(QStringLiteral("fingerprintAvailable")).toBool(), true);
    QCOMPARE(info.value(QStringLiteral("fingerprint")).toString(),
             QString::fromLatin1(kEncryptedFingerprint));
}

void TestMobileKeyStore::referenceForAnInMemoryKeyIsEmpty()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // referenceFor() is the only value that leaves this class for persistence.
    // A pasted key has nothing durable to record, and an empty string is what
    // keeps key material out of the profile.
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(store.referenceFor(QStringLiteral("pasted")).isEmpty());
}

void TestMobileKeyStore::forgetsAnImportedKey()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    MobileKeyStore store(root());
    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));

    QSignalSpy removed(&store, &MobileKeyStore::keyRemoved);
    QVERIFY(store.removeKey(QStringLiteral("pasted")));
    QCOMPARE(removed.count(), 1);
    QVERIFY(store.keyNames().isEmpty());
    QVERIFY(!store.hasKeys());
    QVERIFY(store.keyFingerprint(QStringLiteral("pasted")).isEmpty());

    QVERIFY(!store.removeKey(QStringLiteral("pasted")));
    QVERIFY(!store.lastError().isEmpty());
}

void TestMobileKeyStore::referencesAUserManagedFileWithoutCopyingIt()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    MobileKeyStore store(root());
    const QStringList before = filesUnder(root());

    const QString name =
        store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                   QStringLiteral("mine"));
    QCOMPARE(name, QStringLiteral("mine"));
    QCOMPARE(store.referenceNames(), QStringList{QStringLiteral("mine")});
    // The reference names a file; it is not key material, and it is exactly what
    // the profile's existing identityFile field will carry.
    QCOMPARE(store.referenceFor(QStringLiteral("mine")), m_userKeyPath);
    QCOMPARE(store.keyFingerprint(QStringLiteral("mine")),
             QString::fromLatin1(kPlainFingerprint));

    const QVariantMap info = store.keyInfo(QStringLiteral("mine"));
    QCOMPARE(info.value(QStringLiteral("referenced")).toBool(), true);

    // The user's file stays the only copy.
    QCOMPARE(filesUnder(root()), before);
    assertNoFileContains(root(), keyNeedle(), "a referenced key");
}

void TestMobileKeyStore::reAdoptsAReferenceFromAProfile()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // A remembered server still offers its key after a relaunch, which is the
    // whole reason the durable path exists.
    MobileKeyStore store(root());
    const QString name = store.registerReference(m_userKeyPath,
                                                 QStringLiteral("saved"));
    QCOMPARE(name, QStringLiteral("saved"));
    QCOMPARE(store.referenceFor(QStringLiteral("saved")), m_userKeyPath);

    // Idempotent: re-adopting the same reference is a no-op, not a second entry.
    QCOMPARE(store.registerReference(m_userKeyPath, QStringLiteral("other")),
             QStringLiteral("saved"));
    QCOMPARE(store.referenceNames().count(), 1);
}

void TestMobileKeyStore::refusesAReferenceThisDeviceCannotResolve()
{
    // A profile copied from another device names a file that is not here. That
    // is a thing to explain while the user is looking at the connect page, not
    // a connection that fails later for no visible reason.
    MobileKeyStore store(root());
    const QString missing =
        m_userStorage.filePath(QStringLiteral("not-here/id_ed25519"));
    QVERIFY(store.registerReference(missing, QStringLiteral("ghost")).isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store.referenceNames().isEmpty());
}

void TestMobileKeyStore::referencedKeyMaterialIsReadOnDemandOnly()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // The reference is read once at pick time to report an unusable key early,
    // then dropped: deleting the user's file must make the credential
    // unavailable rather than silently using a cached copy.
    MobileKeyStore store(root());
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));

    QVERIFY(QFile::remove(m_userKeyPath));

    ch::SshConnectionPool pool;
    store.setConnectionPool(&pool);
    QVERIFY(!store.applyIdentityForConnect(QStringLiteral("mine")));
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(!pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::installsAnInMemoryIdentityOnThePool()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(!pool.hasInMemoryIdentity());
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::changingPoolsDisarmsThePreviousIdentity()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    SshConnectionPool first;
    SshConnectionPool second;
    MobileKeyStore store(root());
    store.setConnectionPool(&first);
    QVERIFY(store.importKeyFromText(QStringLiteral("work"),
                                   QString::fromLatin1(kPlainKey)));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("work")));
    QVERIFY(first.hasInMemoryIdentity());
    store.setConnectionPool(&first);
    QVERIFY(first.hasInMemoryIdentity());

    store.setConnectionPool(&second);
    QVERIFY(!first.hasInMemoryIdentity());
    QVERIFY(!second.hasInMemoryIdentity());
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("work")));
    QVERIFY(second.hasInMemoryIdentity());
    store.setConnectionPool(nullptr);
    QVERIFY(!second.hasInMemoryIdentity());
}

void TestMobileKeyStore::installsAReferencedIdentityOnThePool()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("mine")));
    QVERIFY(pool.hasInMemoryIdentity());

    // Resolving a reference must not have left a copy behind.
    assertNoFileContains(root(), keyNeedle(), "a resolved reference");
}

void TestMobileKeyStore::anEmptyNameClearsThePoolIdentity()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // Switching to password auth must not leave the previous attempt's key
    // installed, or a connection aimed at one credential would be made with
    // another.
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(pool.hasInMemoryIdentity());

    QVERIFY(store.applyIdentityForConnect(QString()));
    QVERIFY(!pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::removingTheInstalledKeyDisarmsThePool()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // "Remove" tells the user CodeHarbor will forget the key. The pool holds its
    // own copy of whatever was last installed, so forgetting has to reach in
    // there too — otherwise the bytes stay live for the rest of the run and
    // SessionBootstrap's reconnect ladder would re-authenticate with a credential
    // the user just deleted.
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(pool.hasInMemoryIdentity());

    // Tidying up an UNRELATED key must not disarm the credential the live session
    // is using, or the next reconnect would fail authentication.
    QVERIFY(store.removeKey(QStringLiteral("mine")));
    QVERIFY(pool.hasInMemoryIdentity());

    QVERIFY(store.removeKey(QStringLiteral("pasted")));
    QVERIFY(!pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::forgetSessionDropsEveryStoredSecret()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));
    store.armPassphrase(QStringLiteral("pasted"),
                        QString::fromLatin1(kPassphrase));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));

    store.forgetSession();

    // Secrets are gone: the in-memory key, the armed passphrase and the pool's
    // installed identity.
    QVERIFY(store.keyNames().isEmpty());
    QVERIFY(!store.hasArmedPassphrase(QStringLiteral("pasted")));
    QVERIFY(!pool.hasInMemoryIdentity());
    // The reference is not a secret, so reconnecting needs no fresh pick.
    QCOMPARE(store.referenceNames(), QStringList{QStringLiteral("mine")});

    assertNoFileContains(root(), keyNeedle(), "a forgotten key");
}

void TestMobileKeyStore::aReferenceIsNeverTreatedAsAnIdentityFilePath()
{
    // The profile field a mobile durable reference lives in is the same field
    // SshConnectionPool::connectToHost() takes as an identity FILE, and the
    // reference has to stay in the profile verbatim so the connect page can read
    // it back after a relaunch. So the pool drops it at the point where the
    // stored string would become a path, and the ladder then runs on the
    // in-memory identity alone — which is the real credential on mobile anyway.
    QString scheme;

    // Both reference shapes the mobile client mints: ignored, and named.
    QVERIFY(SshConnectionPool::identityFilePathFor(
                QStringLiteral("content://com.android.providers.downloads/42"),
                &scheme)
                .isEmpty());
    QCOMPARE(scheme, QStringLiteral("content"));
    QVERIFY(SshConnectionPool::identityFilePathFor(
                QStringLiteral("chbookmark:Ym9va21hcmsA"), &scheme)
                .isEmpty());
    QCOMPARE(scheme, QStringLiteral("chbookmark"));

    // And everything that IS a path keeps working exactly as before, with an
    // empty `scheme` to say nothing was dropped.
    QVERIFY(SshConnectionPool::identityFilePathFor(QString(), &scheme).isEmpty());
    QVERIFY(scheme.isEmpty());
    QCOMPARE(SshConnectionPool::identityFilePathFor(
                 QStringLiteral("/home/me/.ssh/id_ed25519"), &scheme),
             QStringLiteral("/home/me/.ssh/id_ed25519"));
    QVERIFY(scheme.isEmpty());
    // Tilde expansion is this client's own doing — libssh does not do it — so it
    // has to survive the new guard.
    QCOMPARE(SshConnectionPool::identityFilePathFor(QStringLiteral("~/.ssh/id_rsa"),
                                                    &scheme),
             QDir::home().filePath(QStringLiteral(".ssh/id_rsa")));
    QVERIFY(scheme.isEmpty());
    QCOMPARE(SshConnectionPool::identityFilePathFor(QStringLiteral("~")),
             QDir::homePath());

    // A Windows drive letter is a ONE-character prefix and must never read as a
    // URI scheme: this is the case that would silently ignore every explicitly
    // configured key on Windows.
    QString windows = SshConnectionPool::identityFilePathFor(
        QStringLiteral("C:\\Users\\me\\.ssh\\id_ed25519"), &scheme);
    QVERIFY(scheme.isEmpty());
    QVERIFY2(windows.startsWith(QStringLiteral("C:")), qPrintable(windows));
    // A colon further along a relative path is not a scheme either.
    QCOMPARE(SshConnectionPool::identityFilePathFor(
                 QStringLiteral("keys/od:d/id_ed25519"), &scheme),
             QStringLiteral("keys/od:d/id_ed25519"));
    QVERIFY(scheme.isEmpty());
}

void TestMobileKeyStore::takingAPassphraseErasesIt()
{
    // SPEC 12.1: one secret, one attempt. A retry has to ask again by
    // construction, not by whoever writes the retry remembering to.
    MobileKeyStore store(root());

    store.armPassphrase(QStringLiteral("locked"),
                        QString::fromLatin1(kPassphrase));
    QVERIFY(store.hasArmedPassphrase(QStringLiteral("locked")));
    QCOMPARE(store.takePassphrase(QStringLiteral("locked")),
             QString::fromLatin1(kPassphrase));

    QVERIFY(!store.hasArmedPassphrase(QStringLiteral("locked")));
    QVERIFY(store.takePassphrase(QStringLiteral("locked")).isEmpty());
}

void TestMobileKeyStore::forgetPassphraseDropsItUnused()
{
    MobileKeyStore store(root());
    store.armPassphrase(QStringLiteral("locked"),
                        QString::fromLatin1(kPassphrase));
    store.forgetPassphrase();
    QVERIFY(!store.hasArmedPassphrase(QStringLiteral("locked")));
    QVERIFY(store.takePassphrase(QStringLiteral("locked")).isEmpty());
}

void TestMobileKeyStore::noPassphraseEverReachesDisk()
{
    MobileKeyStore store(root());

    store.armPassphrase(QStringLiteral("locked"),
                        QString::fromLatin1(kPassphrase));
    QCOMPARE(store.takePassphrase(QStringLiteral("locked")),
             QString::fromLatin1(kPassphrase));
    store.forgetSession();

    assertNoFileContains(root(), QByteArray(kPassphrase), "a passphrase");
}

// ---------------------------------------------------------------------------
// formats and damaged input
// ---------------------------------------------------------------------------

void TestMobileKeyStore::describesALegacyPemKey()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // libssh still loads these, so the client must too — and must be honest that
    // it cannot show a fingerprint for one: the format carries no public half in
    // the clear, so there is nothing to digest without parsing the private key.
    const auto description = MobileKeyStore::describeKeyText(legacyPemKey());
    QVERIFY2(description.valid, qPrintable(description.error));
    QCOMPARE(description.format, QStringLiteral("pem"));
    QVERIFY(description.keyType.isEmpty());
    QVERIFY(!description.encrypted);
    QVERIFY(description.fingerprint.isEmpty());
    QVERIFY(description.normalized.startsWith("-----BEGIN RSA PRIVATE KEY-----\n"));
    QVERIFY(description.normalized.endsWith("-----END RSA PRIVATE KEY-----\n"));
}

void TestMobileKeyStore::reportsAnEncryptedLegacyPemKey()
{
    // RFC 1421 headers, and the blank line between them and the body has to
    // SURVIVE normalisation: libssh needs it to find the body at all.
    const auto description = MobileKeyStore::describeKeyText(
        legacyPemKey(QStringLiteral("Proc-Type: 4,ENCRYPTED\n"
                                    "DEK-Info: AES-128-CBC,44E66F5602241BEE4639C0EAAD722FE4\n"
                                    "\n")));
    QVERIFY2(description.valid, qPrintable(description.error));
    QVERIFY(description.encrypted);
    QCOMPARE(description.kdf, QStringLiteral("AES-128-CBC"));
    QVERIFY(description.normalized.contains("\nProc-Type: 4,ENCRYPTED\n"));
    QVERIFY(description.normalized.contains("\n\n"));
}

void TestMobileKeyStore::describesARealEncryptedPkcs8Key()
{
    for (const char *key :
         {kEncryptedPkcs8, kEncryptedScrypt, kEncryptedPbes1, kEncryptedPkcs12Pbe}) {
        const auto description =
            MobileKeyStore::describeKeyText(QString::fromLatin1(key));
        QVERIFY2(description.valid, qPrintable(description.error));
        QVERIFY(description.encrypted);
        QCOMPARE(description.kdf, QStringLiteral("pkcs8"));
    }
}

void TestMobileKeyStore::rejectsMalformedEncryptedPemContainers()
{
    MobileKeyStore store(root());
    const QString header = QStringLiteral(
        "-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\n");
    const QString footer = QStringLiteral("-----END RSA PRIVATE KEY-----\n");
    for (const QString &text : {
             header + QStringLiteral("DEK-Info: AES-128-CBC,not-an-IV\n\n")
                 + QString::fromLatin1(kEncryptedPemBody) + footer,
             header + QStringLiteral("DEK-Info: AES-128-CBC,0123456789ABCDEF\n\n")
                 + QString::fromLatin1(kEncryptedPemBody) + footer,
             header + QStringLiteral(
                 "DEK-Info: AES-128-CBC,44E66F5602241BEE4639C0EAAD722FE4\n\nAAAA\n")
                 + footer,
             QStringLiteral("-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n"
                            "-----END ENCRYPTED PRIVATE KEY-----\n")}) {
        QVERIFY(!store.importKeyFromText(QStringLiteral("broken"), text));
        QVERIFY(!store.lastError().isEmpty());
    }

    QByteArray encoded;
    for (const QByteArray &line : QByteArray(kEncryptedPkcs8).split('\n')) {
        if (!line.startsWith("-----"))
            encoded += line.trimmed();
    }
    const QByteArray container = QByteArray::fromBase64(encoded);
    QByteArray wrongAlgorithm = container;
    const qsizetype oid = wrongAlgorithm.indexOf(
        QByteArray::fromHex("06092a864886f70d01050d"));
    QVERIFY(oid >= 0);
    wrongAlgorithm[oid] = '\x04'; // An OCTET STRING is not an algorithm OID.
    for (const QByteArray &damaged : {container.chopped(1),
             container + QByteArrayLiteral("trailing bytes"), wrongAlgorithm}) {
        const QString text = QStringLiteral("-----BEGIN ENCRYPTED PRIVATE KEY-----\n")
            + QString::fromLatin1(damaged.toBase64())
            + QStringLiteral("\n-----END ENCRYPTED PRIVATE KEY-----\n");
        QVERIFY(!store.importKeyFromText(QStringLiteral("broken"), text));
    }
    QVERIFY(store.allKeyNames().isEmpty());
}

void TestMobileKeyStore::rejectsMalformedPbkdf2Parameters()
{
    // Deliberately invalid public envelopes, never positive fake-key fixtures.
    const auto der = [](char tag, const QByteArray &body) {
        return QByteArray(1, tag) + QByteArray(1, char(body.size())) + body;
    };
    const QByteArray salt = der('\x04', QByteArrayLiteral("salt"));
    const QByteArray iteration = der('\x02', QByteArray(1, '\x01'));
    const QByteArray cipher = der('\x30',
        QByteArray::fromHex("060960864801650304012a")
            + der('\x04', QByteArray(16, 'i')));
    MobileKeyStore store(root());
    for (const QByteArray &parameters : {
             QByteArray(), salt, iteration,
             salt + der('\x02', QByteArray(1, '\0')),
             salt + der('\x02', QByteArray(1, '\xff')),
             salt + iteration + der('\x02', QByteArray(1, '\0')),
             salt + iteration + der('\x30', QByteArray())}) {
        const QByteArray kdf = der('\x30',
            QByteArray::fromHex("06092a864886f70d01050c") + der('\x30', parameters));
        const QByteArray algorithm = der('\x30',
            QByteArray::fromHex("06092a864886f70d01050d") + der('\x30', kdf + cipher));
        const QByteArray blob = der('\x30', algorithm + der('\x04', QByteArray(16, 'c')));
        const QString text = QStringLiteral("-----BEGIN ENCRYPTED PRIVATE KEY-----\n")
            + QString::fromLatin1(blob.toBase64())
            + QStringLiteral("\n-----END ENCRYPTED PRIVATE KEY-----\n");
        QVERIFY(!store.importKeyFromText(QStringLiteral("broken"), text));
    }
    QVERIFY(store.allKeyNames().isEmpty());
}

void TestMobileKeyStore::rejectsMalformedOrUnsupportedPbeEnvelopes()
{
    const auto der = [](char tag, const QByteArray &body) {
        return QByteArray(1, tag) + QByteArray(1, char(body.size())) + body;
    };
    const auto armor = [](const QByteArray &blob) {
        return QStringLiteral("-----BEGIN ENCRYPTED PRIVATE KEY-----\n")
            + QString::fromLatin1(blob.toBase64())
            + QStringLiteral("\n-----END ENCRYPTED PRIVATE KEY-----\n");
    };
    const QByteArray salt = der('\x04', QByteArrayLiteral("12345678"));
    const QByteArray iteration = der('\x02', QByteArray(1, '\x01'));
    const QByteArray pbes1 = QByteArray::fromHex("06092a864886f70d010503");
    const QByteArray pkcs12 = QByteArray::fromHex("060a2a864886f70d010c0103");
    MobileKeyStore store(root());
    for (const QByteArray &parameters : {
             QByteArray(), salt,
             der('\x04', QByteArrayLiteral("short")) + iteration,
             salt + der('\x02', QByteArray(1, '\0')),
             salt + iteration + der('\x05', QByteArray())}) {
        const QByteArray algorithm = der('\x30', pbes1 + der('\x30', parameters));
        QVERIFY(!store.importKeyFromText(QStringLiteral("broken"),
            armor(der('\x30', algorithm + der('\x04', QByteArray(16, 'c'))))));
    }
    QVERIFY(!store.importKeyFromText(QStringLiteral("broken"), armor(der('\x30',
        der('\x30', pkcs12 + der('\x30', QByteArray()))
            + der('\x04', QByteArray(16, 'c'))))));

    const QByteArray unknown = QByteArray::fromHex("06032a0304");
    const QByteArray pbkdf2 = QByteArray::fromHex("06092a864886f70d01050c");
    const QByteArray pbes2 = QByteArray::fromHex("06092a864886f70d01050d");
    const QByteArray cipher = der('\x30',
        QByteArray::fromHex("060960864801650304012a") + der('\x04', QByteArray(16, 'i')));
    const QByteArray kdf = der('\x30', pbkdf2 + der('\x30', salt + iteration));
    for (const QByteArray &algorithm : {
             der('\x30', unknown + der('\x30', QByteArray())),
             der('\x30', pbes2 + der('\x30',
                 der('\x30', unknown + der('\x30', salt + iteration)) + cipher)),
             der('\x30', pbes2 + der('\x30',
                 kdf + der('\x30', unknown + der('\x04', QByteArray(16, 'i'))))),
             der('\x30', pbes2 + der('\x30',
                 der('\x30', pbkdf2 + der('\x30', salt + iteration
                     + der('\x30', unknown + der('\x05', QByteArray())))) + cipher))}) {
        QVERIFY(!store.importKeyFromText(QStringLiteral("unsupported"),
            armor(der('\x30', algorithm + der('\x04', QByteArray(16, 'c'))))));
        QVERIFY(store.lastError().contains(QStringLiteral("unsupported"), Qt::CaseInsensitive));
    }
    QVERIFY(!store.importKeyFromText(QStringLiteral("unsupported"),
        legacyPemKey(QStringLiteral("Proc-Type: 4,ENCRYPTED\n"
            "DEK-Info: FUTURE-CIPHER,44E66F5602241BEE4639C0EAAD722FE4\n\n"))));
    QVERIFY(store.lastError().contains(QStringLiteral("unsupported"), Qt::CaseInsensitive));
    QVERIFY(store.allKeyNames().isEmpty());
}

void TestMobileKeyStore::rejectsMalformedScryptParameters()
{
    const auto der = [](char tag, const QByteArray &body) {
        return QByteArray(1, tag) + QByteArray(1, char(body.size())) + body;
    };
    const QByteArray salt = der('\x04', QByteArrayLiteral("salt"));
    const QByteArray zero = der('\x02', QByteArray(1, '\0'));
    const QByteArray one = der('\x02', QByteArray(1, '\x01'));
    const QByteArray two = der('\x02', QByteArray(1, '\x02'));
    const QByteArray negative = der('\x02', QByteArray(1, '\xff'));
    const QByteArray limit = der('\x02', QByteArray::fromHex("40000000"));
    const QByteArray oversized = der('\x02', QByteArray::fromHex("010000000000000000"));
    const QByteArray tooMuchCost = der('\x02', QByteArray::fromHex("010000"));
    const QByteArray cipher = der('\x30',
        QByteArray::fromHex("060960864801650304012a")
            + der('\x04', QByteArray(16, 'i')));
    MobileKeyStore store(root());
    for (const QByteArray &parameters : {
             QByteArray(), salt, salt + two + one,
             salt + zero + one + one, salt + one + one + one,
             salt + der('\x02', QByteArray(1, '\x03')) + one + one,
             salt + negative + one + one, salt + oversized + one + one,
             salt + two + zero + one, salt + two + one + zero,
             salt + two + limit + one, salt + two + one + limit,
             salt + tooMuchCost + one + one,
             salt + two + one + one + zero,
             salt + two + one + one + negative,
             salt + two + one + one + one + one,
             der('\x05', QByteArray()) + two + one + one}) {
        const QByteArray kdf = der('\x30',
            QByteArray::fromHex("06092b06010401da47040b") + der('\x30', parameters));
        const QByteArray algorithm = der('\x30',
            QByteArray::fromHex("06092a864886f70d01050d") + der('\x30', kdf + cipher));
        const QByteArray container = der('\x30',
            algorithm + der('\x04', QByteArray(16, 'c')));
        const QString text = QStringLiteral("-----BEGIN ENCRYPTED PRIVATE KEY-----\n")
            + QString::fromLatin1(container.toBase64())
            + QStringLiteral("\n-----END ENCRYPTED PRIVATE KEY-----\n");
        QVERIFY(!store.importKeyFromText(QStringLiteral("broken"), text));
    }
    QVERIFY(store.allKeyNames().isEmpty());
}

void TestMobileKeyStore::rejectsInconsistentOpenSshEncryptionParameters()
{
    QByteArray encoded;
    for (const QByteArray &line : QByteArray(kEncryptedKey).split('\n')) {
        if (!line.startsWith("-----"))
            encoded += line.trimmed();
    }
    const QByteArray container = QByteArray::fromBase64(encoded);
    qsizetype offset = QByteArrayLiteral("openssh-key-v1").size() + 1;
    const qsizetype cipherStart = offset + 4;
    offset += 4 + qFromBigEndian<quint32>(container.constData() + offset);
    const qsizetype kdfLengthOffset = offset;
    const qsizetype kdfStart = offset + 4;
    const quint32 kdfLength = qFromBigEndian<quint32>(container.constData() + offset);
    offset += 4 + kdfLength;
    const qsizetype optionsStart = offset + 4;
    offset += 4 + qFromBigEndian<quint32>(container.constData() + offset);
    offset += 4; // key count
    offset += 4 + qFromBigEndian<quint32>(container.constData() + offset);
    const qsizetype privateLengthOffset = offset;
    const quint32 privateLength = qFromBigEndian<quint32>(container.constData() + offset);

    QByteArray noKdf = container;
    noKdf.replace(kdfStart, kdfLength, QByteArrayLiteral("none"));
    qToBigEndian<quint32>(4, noKdf.data() + kdfLengthOffset);
    QByteArray emptySalt = container;
    qToBigEndian<quint32>(0, emptySalt.data() + optionsStart);
    QByteArray zeroRounds = container;
    const quint32 saltLength =
        qFromBigEndian<quint32>(container.constData() + optionsStart);
    qToBigEndian<quint32>(0, zeroRounds.data() + optionsStart + 4 + saltLength);
    QByteArray unknownCipher = container;
    unknownCipher.replace(cipherStart, 3, QByteArrayLiteral("bad"));
    QByteArray partialBlock = container.chopped(1);
    qToBigEndian<quint32>(privateLength - 1, partialBlock.data() + privateLengthOffset);
    MobileKeyStore store(root());
    for (const QByteArray &damaged :
         {noKdf, emptySalt, zeroRounds, unknownCipher, partialBlock}) {
        const QString text = QStringLiteral("-----BEGIN OPENSSH PRIVATE KEY-----\n")
            + QString::fromLatin1(damaged.toBase64())
            + QStringLiteral("\n-----END OPENSSH PRIVATE KEY-----\n");
        QVERIFY(!store.importKeyFromText(QStringLiteral("broken"), text));
    }
    QVERIFY(store.allKeyNames().isEmpty());
}

void TestMobileKeyStore::rejectsIncompletePemEncryptionHeaders()
{
    for (const QString &headers :
         {QStringLiteral("DEK-Info: AES-128-CBC,44E66F5602241BEE4639C0EAAD722FE4\n\n"),
          QStringLiteral("Proc-Type: 4,ENCRYPTED\n\n")}) {
        const auto description = MobileKeyStore::describeKeyText(legacyPemKey(headers));
        QVERIFY(!description.valid);
        QVERIFY(!description.error.isEmpty());
        QVERIFY(description.normalized.isEmpty());
    }
}

void TestMobileKeyStore::rejectsInvalidClearPrivateKeys()
{
    MobileKeyStore store(root());
    // Well-framed base64 containing a truncated RSA DER sequence is not a key.
    const QString truncatedPem = QStringLiteral(
        "-----BEGIN RSA PRIVATE KEY-----\n"
        "MIIBOgIBAAJBAKj34GkxFhD90vcNLYLInFEX6Ppy1tPf9Cnzj4p4WGeKLs1Pt8Qu\n"
        "-----END RSA PRIVATE KEY-----\n");
    QVERIFY(!store.importKeyFromText(QStringLiteral("broken-pem"), truncatedPem));

    QByteArray encoded;
    for (const QByteArray &line : QByteArray(kPlainKey).split('\n')) {
        if (!line.startsWith("-----"))
            encoded += line.trimmed();
    }
    QByteArray container = QByteArray::fromBase64(encoded);
    qsizetype offset = QByteArrayLiteral("openssh-key-v1").size() + 1;
    for (int field = 0; field < 3; ++field)
        offset += 4 + qFromBigEndian<quint32>(container.constData() + offset);
    offset += 4;
    offset += 4 + qFromBigEndian<quint32>(container.constData() + offset);
    QByteArray wrongPublicKey = container;
    // Still a valid ed25519 public blob, but it does not belong to this secret.
    wrongPublicKey[offset - 1] = char(wrongPublicKey.at(offset - 1) ^ 1);
    offset += 4; // private section length
    // Break the paired check integers without changing any container lengths.
    container[offset] = char(container.at(offset) ^ 1);
    for (const QByteArray &damaged : {container, wrongPublicKey}) {
        const QString brokenOpenSsh =
            QStringLiteral("-----BEGIN OPENSSH PRIVATE KEY-----\n")
            + QString::fromLatin1(damaged.toBase64())
            + QStringLiteral("\n-----END OPENSSH PRIVATE KEY-----\n");
        QVERIFY(!store.importKeyFromText(QStringLiteral("broken-openssh"), brokenOpenSsh));
    }
    QVERIFY(store.allKeyNames().isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    assertNoFileContains(root(), keyNeedle(), "a rejected private key");
}

void TestMobileKeyStore::refusesOversizePastedText()
{
    MobileKeyStore store(root());
    QString text = QString::fromLatin1(kPlainKey);
    text += QString(MobileKeyStore::kMaxKeyBytes, u'\n');
    QVERIFY(!store.importKeyFromText(QStringLiteral("oversize"), text));
    QVERIFY(store.allKeyNames().isEmpty());
    QVERIFY(!store.lastError().isEmpty());
}

void TestMobileKeyStore::normalisesCrlfAndSurroundingText()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // What a user actually pastes: a key that came through a mail client, with
    // CRLF line endings, an indented first line and a signature after the end
    // marker. All of it has to come out as the exact bytes libssh is handed.
    QString crlf = QString::fromLatin1(kPlainKey);
    crlf.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    const QString pasted = QStringLiteral("Here is the key:\r\n\r\n   ") + crlf
                           + QStringLiteral("-- \r\nSent from my phone\r\n");

    const auto description = MobileKeyStore::describeKeyText(pasted);
    QVERIFY2(description.valid, qPrintable(description.error));
    QCOMPARE(description.fingerprint, QString::fromLatin1(kPlainFingerprint));
    // Byte for byte the original armour: no CR anywhere, nothing from around the
    // block, and the trailing newline libssh expects.
    QCOMPARE(description.normalized, QByteArray(kPlainKey));
    QVERIFY(!description.normalized.contains('\r'));
}

void TestMobileKeyStore::rejectsAnAlteredBase64Body()
{
    // A chat client that inserted an ellipsis, or a diff that lost a chunk. The
    // alphabet is checked BEFORE decoding, so this is reported as damage rather
    // than as a malformed container after a lenient decoder silently dropped the
    // offending characters.
    QString altered = QString::fromLatin1(kPlainKey);
    altered.replace(QStringLiteral("hwAAAAtzc2gtZWQyNTUxOQ"),
                    QStringLiteral("hwAAAAt...c2gtZWQyNTUxOQ"));
    QVERIFY(altered != QString::fromLatin1(kPlainKey));  // the replace matched

    const auto description = MobileKeyStore::describeKeyText(altered);
    QVERIFY(!description.valid);
    QVERIFY(description.error.contains(QStringLiteral("altered")));
    QVERIFY(description.normalized.isEmpty());
}

void TestMobileKeyStore::acceptsAFileExactlyAtTheSizeCap()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // The bound is inclusive, and it is enforced on what was READ rather than on
    // what the file claims: exactly kMaxKeyBytes goes through, one byte more does
    // not. Padding after the END marker is dropped by normalisation, which is what
    // lets a real key be padded up to the boundary at all.
    MobileKeyStore store(root());

    QByteArray padded(kPlainKey);
    QVERIFY(padded.size() < MobileKeyStore::kMaxKeyBytes);
    padded.append(QByteArray(qsizetype(MobileKeyStore::kMaxKeyBytes)
                                 - padded.size(),
                             '\n'));
    QCOMPARE(qint64(padded.size()), MobileKeyStore::kMaxKeyBytes);

    const QString atCap = m_userStorage.filePath(QStringLiteral("at-cap.key"));
    QFile atCapFile(atCap);
    QVERIFY(atCapFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(atCapFile.write(padded), qint64(padded.size()));
    atCapFile.close();

    QVERIFY(store.importKeyFromFile(QUrl::fromLocalFile(atCap),
                                    QStringLiteral("at-cap")));
    QCOMPARE(store.keyFingerprint(QStringLiteral("at-cap")),
             QString::fromLatin1(kPlainFingerprint));

    const QString overCap = m_userStorage.filePath(QStringLiteral("over-cap.key"));
    QFile overCapFile(overCap);
    QVERIFY(overCapFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(overCapFile.write(padded + '\n'), qint64(padded.size() + 1));
    overCapFile.close();

    QVERIFY(!store.importKeyFromFile(QUrl::fromLocalFile(overCap),
                                     QStringLiteral("over-cap")));
    QVERIFY(!store.lastError().isEmpty());
    QCOMPARE(store.keyNames(), QStringList{QStringLiteral("at-cap")});
}

// ---------------------------------------------------------------------------
// references that are not what they claim
// ---------------------------------------------------------------------------

void TestMobileKeyStore::refusesADirectoryReference()
{
    // A picker can hand back a directory, and a stored reference can end up
    // naming one. Whichever way it arrives it must be refused with something to
    // read, not accepted into the key list to fail at connect time.
    MobileKeyStore store(root());
    const QString directory = m_userStorage.path();

    QVERIFY(store.registerReference(directory, QStringLiteral("dir")).isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store
                .addReferenceFromFile(QUrl::fromLocalFile(directory),
                                      QStringLiteral("dir"))
                .isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store.referenceNames().isEmpty());
    QVERIFY(!store.hasKeys());
}

void TestMobileKeyStore::registerReferenceRepairsAnUnusableName()
{
    // This is the one import path whose name does not go through claimName() on
    // the happy path, and the name it is handed comes from a saved profile rather
    // than from a field the user just typed. An unusable one must not end up in
    // the key maps and in the QML picker; the credential is too useful to refuse
    // over it, so the name is repaired instead.
    MobileKeyStore store(root());
    const QString name =
        store.registerReference(m_userKeyPath, QStringLiteral("not a name"));
    QVERIFY(!name.isEmpty());
    QVERIFY2(MobileKeyStore::isUsableKeyName(name), qPrintable(name));
    QCOMPARE(store.referenceNames(), QStringList{name});
    QCOMPARE(store.referenceFor(name), m_userKeyPath);
    if (CH_HAVE_LIBSSH)
        QCOMPARE(store.keyFingerprint(name), QString::fromLatin1(kPlainFingerprint));
}

void TestMobileKeyStore::registerReferenceDoesNotShadowASavedKey()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    {
        MobileKeyStore store(appData.path());
        QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                       QString::fromLatin1(kPlainKey)));
        QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
                 qPrintable(store.lastError()));
    }

    MobileKeyStore reopened(appData.path());
    QVERIFY(reopened.keyNames().isEmpty());
    const QString adopted =
        reopened.registerReference(m_userKeyPath, QStringLiteral("kept"));
    QCOMPARE(adopted, QStringLiteral("kept-2"));
    QCOMPARE(reopened.allKeyNames(),
             (QStringList{QStringLiteral("kept-2"), QStringLiteral("kept")}));
    QCOMPARE(reopened.referenceFor(QStringLiteral("kept")),
             savedKeyPath(appData.path(), QStringLiteral("kept")));
    QVERIFY(reopened.removeKey(adopted));
    QVERIFY(QFile::exists(m_userKeyPath));
    QVERIFY(reopened.isSavedOnDevice(QStringLiteral("kept")));
    QVERIFY(QFile::exists(savedKeyPath(appData.path(), QStringLiteral("kept"))));
    QCOMPARE(reopened.keyFingerprint(QStringLiteral("kept")),
             QString::fromLatin1(kPlainFingerprint));
}

void TestMobileKeyStore::savedKeyReferenceRoundTripsWithoutAnAlias()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    QString reference;
    QString profileId;
    const QString iniPath = appData.filePath(QStringLiteral("profiles.ini"));
    {
        MobileKeyStore store(appData.path());
        QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                       QString::fromLatin1(kPlainKey)));
        QVERIFY(store.referenceFor(QStringLiteral("kept")).isEmpty());
        QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
                 qPrintable(store.lastError()));
        reference = store.referenceFor(QStringLiteral("kept"));
        QVERIFY(!reference.isEmpty());
        QCOMPARE(store.keyInfo(QStringLiteral("kept"))
                     .value(QStringLiteral("reference")).toString(), reference);
        ch::ServerProfiles profiles(iniPath);
        profileId = profiles.addProfile({
            {QStringLiteral("host"), QStringLiteral("test.invalid")},
            {QStringLiteral("user"), QStringLiteral("test-user")},
            {QStringLiteral("identityFile"), reference},
        });
        QVERIFY(!profileId.isEmpty());
    }

    SshConnectionPool pool;
    MobileKeyStore reopened(appData.path());
    reopened.setConnectionPool(&pool);
    ch::ServerProfiles profiles(iniPath);
    const QString persistedReference = profiles.profile(profileId)
                                           .value(QStringLiteral("identityFile")).toString();
    const QString selected = reopened.registerReference(persistedReference, QString());
    QCOMPARE(selected, QStringLiteral("kept"));
    QCOMPARE(reopened.registerReference(reference, QStringLiteral("different")), selected);
    QCOMPARE(reopened.allKeyNames(), QStringList{selected});
    QVERIFY(reopened.referenceNames().isEmpty());
    QVERIFY2(reopened.applyIdentityForConnect(selected), qPrintable(reopened.lastError()));
    QVERIFY(pool.hasInMemoryIdentity());

    QVERIFY(reopened.forgetSavedKey(selected));
    QVERIFY(!pool.hasInMemoryIdentity());
    QVERIFY(reopened.allKeyNames().isEmpty());
    QVERIFY(!QFile::exists(reference));
    QVERIFY(reopened.registerReference(reference, QString()).isEmpty());
}

void TestMobileKeyStore::savedKeyReferenceRefusesAMissingFile()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    {
        MobileKeyStore store(appData.path());
        QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                       QString::fromLatin1(kPlainKey)));
        QVERIFY(store.saveKeyOnDevice(QStringLiteral("kept")));
    }

    MobileKeyStore reopened(appData.path());
    const QString reference = reopened.referenceFor(QStringLiteral("kept"));
    QVERIFY(!reference.isEmpty());
    QVERIFY(QFile::remove(reference));
    QVERIFY(reopened.registerReference(reference, QString()).isEmpty());
    QVERIFY(!reopened.lastError().isEmpty());
    QVERIFY(reopened.referenceNames().isEmpty());
}

void TestMobileKeyStore::registerReferenceSuffixFitsAMaximumLengthName()
{
    const QString secondPath = m_userStorage.filePath(QStringLiteral("second-key"));
    QVERIFY(QFile::copy(m_userKeyPath, secondPath));
    const QString name(64, QLatin1Char('k'));
    for (bool inMemory : {false, true}) {
        if (inMemory && !CH_HAVE_LIBSSH)
            continue; // The reference-only collision needs no native parser.
        MobileKeyStore store(root());
        if (inMemory) {
            QVERIFY(store.importKeyFromText(name, QString::fromLatin1(kPlainKey)));
        } else {
            QCOMPARE(store.registerReference(m_userKeyPath, name), name);
        }
        const QString adopted = store.registerReference(secondPath, name);
        QCOMPARE(adopted, name.left(62) + QStringLiteral("-2"));
        QVERIFY(MobileKeyStore::isUsableKeyName(adopted));
        QCOMPARE(store.referenceFor(adopted), secondPath);
        QVERIFY(store.referenceNames().contains(adopted));
        if (inMemory)
            QCOMPARE(store.keyNames(), QStringList{name});
        else
            QVERIFY(store.referenceNames().contains(name));
    }
}

void TestMobileKeyStore::registerReferenceRefusesAnEmptyReference()
{
    // A password-only profile stores an empty identityFile. Asking the store to
    // adopt that is a caller mistake and is reported as one — which is why
    // ConnectPage only calls this when the profile actually carries a reference,
    // rather than showing the user an error for a profile that is simply
    // password-only.
    MobileKeyStore store(root());
    QVERIFY(store.registerReference(QString(), QString()).isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(store.registerReference(QStringLiteral("   "), QString()).isEmpty());
    QVERIFY(store.referenceNames().isEmpty());
}

void TestMobileKeyStore::refusesAReferenceWhoseFileStoppedBeingAKey()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // The resolvable check at pick time and the read at connect time are separated
    // by however long the user's session lasts, and the file belongs to the user:
    // it can be replaced with anything in between. The read validates again rather
    // than trusting the earlier answer.
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));

    QFile replaced(m_userKeyPath);
    QVERIFY(replaced.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(replaced.write("this is not a key any more\n") > 0);
    replaced.close();

    QVERIFY(!store.applyIdentityForConnect(QStringLiteral("mine")));
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(!pool.hasInMemoryIdentity());
    // And the UI is told there is nothing to describe rather than shown a stale
    // fingerprint from the file that used to be there.
    QVERIFY(store.keyInfo(QStringLiteral("mine")).isEmpty());
}

// ---------------------------------------------------------------------------
// what the UI is allowed to learn
// ---------------------------------------------------------------------------

void TestMobileKeyStore::describeTextNeverReturnsKeyBytes()
{
    // describeText() is the preview the import sheet renders. It goes to QML, so
    // anything in it can end up in a visible item, in a debug dump or in a log
    // line: it must carry the DESCRIPTION and never the key.
    MobileKeyStore store(root());
    const QVariantMap described =
        store.describeText(QString::fromLatin1(kPlainKey));
    QCOMPARE(described.value(QStringLiteral("valid")).toBool(), CH_HAVE_LIBSSH != 0);
    if (CH_HAVE_LIBSSH) {
        QCOMPARE(described.value(QStringLiteral("fingerprint")).toString(),
                 QString::fromLatin1(kPlainFingerprint));
    }
    for (auto it = described.cbegin(); it != described.cend(); ++it) {
        QVERIFY2(!it.value().toString().toLatin1().contains(keyNeedle()),
                 qPrintable(it.key()));
    }

    // A refusal must not quote the input either: the text that was refused may
    // still be key material, and an error message is a thing that gets logged.
    const QVariantMap refused = store.describeText(
        QStringLiteral("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5 someone@host"));
    QCOMPARE(refused.value(QStringLiteral("valid")).toBool(), false);
    QVERIFY(!refused.value(QStringLiteral("error"))
                 .toString()
                 .contains(QStringLiteral("AAAAC3")));
}

void TestMobileKeyStore::keyInfoForAnUnknownNameIsEmpty()
{
    MobileKeyStore store(root());
    QVERIFY(store.keyInfo(QStringLiteral("never-loaded")).isEmpty());
    QVERIFY(store.keyFingerprint(QStringLiteral("never-loaded")).isEmpty());
    QVERIFY(store.referenceFor(QStringLiteral("never-loaded")).isEmpty());
}

void TestMobileKeyStore::sessionOnlyFallbackIsOfferedOnlyForADurabilityFailure()
{
    // KeyImportSheet offers "use that file just for this session" off this flag.
    // A file that is not a key, or one too large to read, would fail the
    // session-only import in exactly the same way, so offering the fallback for
    // those is a button whose only outcome is the identical refusal.
    MobileKeyStore store(root());

    const QString publicKey = m_userStorage.filePath(QStringLiteral("id.pub"));
    QFile publicFile(publicKey);
    QVERIFY(publicFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(publicFile.write(QByteArray(kPublicKey)) > 0);
    publicFile.close();

    QVERIFY(store.addReferenceFromFile(QUrl::fromLocalFile(publicKey),
                                       QStringLiteral("pub"))
                .isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(!store.lastFailureAllowsSessionOnly());

    // A durable reference to a plain path always succeeds on this platform, so the
    // flag stays false after a SUCCESS too — it describes a failure, not a state.
    if (CH_HAVE_LIBSSH) {
        QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                            QStringLiteral("mine")),
                 QStringLiteral("mine"));
        QVERIFY(!store.lastFailureAllowsSessionOnly());
    }
}

void TestMobileKeyStore::nothingReachesThePerAppSandboxEither()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // The leak checks elsewhere walk the store's own rootDirectory, which proves
    // nothing about an implementation that derived a keys directory from
    // QStandardPaths instead — the one place a mobile app would naturally put it.
    // initTestCase() puts QStandardPaths in test mode, so these paths are inside
    // the test's temporary tree and can be scanned safely.
    ch::SshConnectionPool pool;
    MobileKeyStore store(root());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    QCOMPARE(store.addReferenceFromFile(QUrl::fromLocalFile(m_userKeyPath),
                                        QStringLiteral("mine")),
             QStringLiteral("mine"));
    store.armPassphrase(QStringLiteral("pasted"),
                        QString::fromLatin1(kPassphrase));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("mine")));
    QVERIFY(!store.keyInfo(QStringLiteral("mine")).isEmpty());
    store.forgetSession();

    const QStringList roots = sandboxRoots();
    QVERIFY(!roots.isEmpty());
    assertNoFileContainsAnywhere(roots, keyNeedle(), "an imported key");
    assertNoFileContainsAnywhere(roots, QByteArray(kPassphrase), "a passphrase");
    assertNoFileContains(root(), keyNeedle(), "an imported key");
}

// ---------------------------------------------------------------------------
// path 3: saved on this device (opt-in, per key)
// ---------------------------------------------------------------------------

void TestMobileKeyStore::savesAPastedKeyForTheNextLaunch()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // The whole point of the third path: a key that was PASTED once is offered
    // again by a store built from scratch over the same app-data directory —
    // which is what a relaunch is — and can still authenticate.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());

    {
        MobileKeyStore store(appData.path());
        QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                        QString::fromLatin1(kPlainKey)));
        QVERIFY(!store.isSavedOnDevice(QStringLiteral("kept")));

        QSignalSpy changed(&store, &MobileKeyStore::keyNamesChanged);
        QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
                 qPrintable(store.lastError()));
        QVERIFY(store.lastError().isEmpty());
        QCOMPARE(changed.count(), 1);
        QVERIFY(store.isSavedOnDevice(QStringLiteral("kept")));
        // The in-memory copy is untouched: saving adds durability to the key the
        // session is already using rather than replacing it.
        QCOMPARE(store.keyNames(), QStringList{QStringLiteral("kept")});
        // And it is ONE entry in the list the connect page renders, not two.
        QCOMPARE(store.allKeyNames(), QStringList{QStringLiteral("kept")});
        QVERIFY(QFile::exists(savedKeyPath(appData.path(),
                                           QStringLiteral("kept"))));
    }

    // A FRESH store over the same directory: this is the relaunch.
    ch::SshConnectionPool pool;
    MobileKeyStore reopened(appData.path());
    reopened.setConnectionPool(&pool);
    QCOMPARE(reopened.savedKeyNames(), QStringList{QStringLiteral("kept")});
    QCOMPARE(reopened.allKeyNames(), QStringList{QStringLiteral("kept")});
    QVERIFY(reopened.hasKeys());
    QVERIFY(reopened.isSavedOnDevice(QStringLiteral("kept")));
    // Nothing is loaded in memory: the bytes are read when they are needed.
    QVERIFY(reopened.keyNames().isEmpty());

    const QVariantMap info = reopened.keyInfo(QStringLiteral("kept"));
    QCOMPARE(info.value(QStringLiteral("saved")).toBool(), true);
    QCOMPARE(info.value(QStringLiteral("referenced")).toBool(), false);
    QCOMPARE(info.value(QStringLiteral("fingerprint")).toString(),
             QString::fromLatin1(kPlainFingerprint));
    // The saved bytes are the same key, byte for byte, so it still authenticates.
    QVERIFY2(reopened.applyIdentityForConnect(QStringLiteral("kept")),
             qPrintable(reopened.lastError()));
    QVERIFY(pool.hasInMemoryIdentity());
}

void TestMobileKeyStore::aSavedKeyIsOwnerOnlyAndSoIsItsFolder()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
#ifndef Q_OS_UNIX
    QSKIP("POSIX modes only: QFile::permissions() synthesises these bits from an "
          "ACL on Windows, so the same assertion would describe nothing real.");
#else
    // A private key readable by the group or by the world is the failure this
    // path could most easily ship with and nobody would notice, because
    // everything else about it would still work. The directory matters as much as
    // the file: a listable keys directory tells another process which keys exist
    // and under what names.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    MobileKeyStore store(appData.path());
    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));

    const QFile::Permissions others =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther
        | QFileDevice::ExeOther;

    const QFile::Permissions file =
        QFile::permissions(savedKeyPath(appData.path(), QStringLiteral("kept")));
    QVERIFY(file.testFlag(QFileDevice::ReadOwner));
    QVERIFY(file.testFlag(QFileDevice::WriteOwner));
    QCOMPARE(file & others, QFile::Permissions());

    const QFile::Permissions directory =
        QFile::permissions(savedKeysDirectory(appData.path()));
    QVERIFY(directory.testFlag(QFileDevice::ReadOwner));
    QVERIFY(directory.testFlag(QFileDevice::WriteOwner));
    QVERIFY(directory.testFlag(QFileDevice::ExeOwner));
    QCOMPARE(directory & others, QFile::Permissions());
#endif
}

void TestMobileKeyStore::savingIsNeverImplicit()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // SPEC 11.2 admits the copy only for a key the user explicitly asked to keep.
    // So an ordinary import — the default path, and the one nearly every user
    // takes — must leave the device with no keys directory at all, never mind a
    // key in it.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    ch::SshConnectionPool pool;
    MobileKeyStore store(appData.path());
    store.setConnectionPool(&pool);
    QVERIFY(store.importKeyFromText(QStringLiteral("pasted"),
                                    QString::fromLatin1(kPlainKey)));
    // Using the key for a connection is not asking for it to be kept, so a whole
    // authenticate cycle must still leave nothing on the device.
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("pasted")));
    QVERIFY(pool.hasInMemoryIdentity());
    QVERIFY(!store.isSavedOnDevice(QStringLiteral("pasted")));
    QVERIFY(store.savedKeyNames().isEmpty());
    QVERIFY(!QFile::exists(savedKeysDirectory(appData.path())));
    assertNoFileContains(appData.path(), keyNeedle(), "an unsaved key");

    // Same for a store constructed over a directory that has never seen a save:
    // construction reads, it does not create.
    MobileKeyStore reopened(appData.path());
    QVERIFY(reopened.savedKeyNames().isEmpty());
    QVERIFY(!reopened.hasKeys());
    QVERIFY(!QFile::exists(savedKeysDirectory(appData.path())));
}

void TestMobileKeyStore::refusesToSaveAKeyItWouldHaveToBuildAPathFor()
{
    // The name reaches the FILENAME, so an unvalidated one is a write outside the
    // app's own data directory. "../evil" is the case that matters: it is a name
    // no import path would accept, but saveKeyOnDevice() is reachable from QML
    // with whatever string a caller passes, so it has to refuse it itself — and
    // refuse it BEFORE any filesystem call, which is what "no file was created"
    // below actually checks.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    MobileKeyStore store(appData.path());
    if (CH_HAVE_LIBSSH) {
        QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                       QString::fromLatin1(kPlainKey)));
    }

    for (const QString &name : {QStringLiteral("../evil"),
                                QStringLiteral("../../evil"),
                                QStringLiteral("sub/evil"),
                                QStringLiteral(".hidden"),
                                QStringLiteral("has space"),
                                QString(65, QLatin1Char('a')),
                                QString()}) {
        QVERIFY2(!store.saveKeyOnDevice(name), qPrintable(name));
        QVERIFY(!store.lastError().isEmpty());
        QVERIFY2(!store.isSavedOnDevice(name), qPrintable(name));
    }
    QVERIFY(store.savedKeyNames().isEmpty());

    // Nothing was created anywhere: not the keys directory, not the traversal
    // target one level up from it, and no file under the app-data root at all.
    QVERIFY(!QFile::exists(savedKeysDirectory(appData.path())));
    QVERIFY(!QFile::exists(QDir(appData.path()).filePath(QStringLiteral("evil"))));
    QVERIFY(filesUnder(appData.path()).isEmpty());

    // The refusal is about the NAME, not about the key: the same store still
    // saves the key it does have.
    if (CH_HAVE_LIBSSH) {
        QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
                 qPrintable(store.lastError()));
    }
}

void TestMobileKeyStore::refusesToSaveAReferencedKey()
{
    // Path 2's promise is that the key stays in the user's own storage. Copying a
    // referenced key into app storage behind the same switch would quietly break
    // it, so this is refused with something to read rather than silently doing
    // the more surprising of the two possible things.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    MobileKeyStore store(appData.path());
    QCOMPARE(store.registerReference(m_userKeyPath, QStringLiteral("mine")),
             QStringLiteral("mine"));

    QVERIFY(!store.saveKeyOnDevice(QStringLiteral("mine")));
    QVERIFY(!store.lastError().isEmpty());
    QVERIFY(!store.isSavedOnDevice(QStringLiteral("mine")));
    QVERIFY(!QFile::exists(savedKeysDirectory(appData.path())));
    // An unknown name is refused too, and neither refusal creates anything.
    QVERIFY(!store.saveKeyOnDevice(QStringLiteral("never-loaded")));
    QVERIFY(filesUnder(appData.path()).isEmpty());
}

void TestMobileKeyStore::forgetSessionKeepsASavedKeyAndWipesTheSessionOne()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // "Disconnect" is not "delete my key". A saved key survives it — that is what
    // saving means — while the session-only key beside it is wiped, which is what
    // NOT saving means.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    ch::SshConnectionPool pool;
    MobileKeyStore store(appData.path());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY(store.importKeyFromText(QStringLiteral("session-only"),
                                    QString::fromLatin1(kEncryptedKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    store.armPassphrase(QStringLiteral("session-only"),
                        QString::fromLatin1(kPassphrase));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("kept")));

    store.forgetSession();

    // The session secrets are gone, including the loaded copy of the saved key.
    QVERIFY(store.keyNames().isEmpty());
    QVERIFY(!store.hasArmedPassphrase(QStringLiteral("session-only")));
    QVERIFY(!pool.hasInMemoryIdentity());
    // The saved key is still there, still listed, and still usable without a
    // fresh paste — which is the only reason the user saved it.
    QCOMPARE(store.savedKeyNames(), QStringList{QStringLiteral("kept")});
    QCOMPARE(store.allKeyNames(), QStringList{QStringLiteral("kept")});
    QVERIFY(QFile::exists(savedKeyPath(appData.path(), QStringLiteral("kept"))));
    QVERIFY2(store.applyIdentityForConnect(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QVERIFY(pool.hasInMemoryIdentity());
    // The one that was not saved is gone from the device's point of view too.
    QVERIFY(!store.applyIdentityForConnect(QStringLiteral("session-only")));
    QVERIFY(store.keyInfo(QStringLiteral("session-only")).isEmpty());
    // And the encrypted key's passphrase never reached the disk, saved key or no
    // saved key.
    assertNoFileContains(appData.path(), QByteArray(kPassphrase),
                         "a passphrase");
}

void TestMobileKeyStore::forgetSavedKeyDeletesTheFileFromDisk()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // The surface that offers to keep a key also offers to delete it, and
    // "delete" has to mean the file is gone — not merely dropped from a list that
    // the next launch's directory scan would repopulate.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    ch::SshConnectionPool pool;
    MobileKeyStore store(appData.path());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("kept")));
    QVERIFY(pool.hasInMemoryIdentity());

    const QString path = savedKeyPath(appData.path(), QStringLiteral("kept"));
    QVERIFY(QFile::exists(path));

    QSignalSpy removed(&store, &MobileKeyStore::keyRemoved);
    QVERIFY2(store.forgetSavedKey(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QCOMPARE(removed.count(), 1);

    // Gone from disk, from the lists, from memory, and from the pool.
    QVERIFY(!QFile::exists(path));
    QVERIFY(store.savedKeyNames().isEmpty());
    QVERIFY(store.keyNames().isEmpty());
    QVERIFY(store.allKeyNames().isEmpty());
    QVERIFY(!store.hasKeys());
    QVERIFY(!store.isSavedOnDevice(QStringLiteral("kept")));
    QVERIFY(!pool.hasInMemoryIdentity());
    assertNoFileContains(appData.path(), keyNeedle(), "a deleted saved key");

    // And it stays gone across a relaunch, which is the claim a list-only
    // deletion would have failed.
    MobileKeyStore reopened(appData.path());
    QVERIFY(reopened.savedKeyNames().isEmpty());
    QVERIFY(!reopened.hasKeys());

    // A second deletion is refused rather than silently reported as a success.
    QVERIFY(!store.forgetSavedKey(QStringLiteral("kept")));
    QVERIFY(!store.lastError().isEmpty());
}

void TestMobileKeyStore::removeKeyDeletesTheSavedFileToo()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // removeKey() is the general "forget this key". Leaving the saved file behind
    // would resurrect at the next launch exactly the credential the user just
    // removed, which is the worst possible reading of "forget".
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    MobileKeyStore store(appData.path());
    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));

    const QString path = savedKeyPath(appData.path(), QStringLiteral("kept"));
    QVERIFY(QFile::exists(path));
    QVERIFY2(store.removeKey(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QVERIFY(!QFile::exists(path));
    QVERIFY(store.savedKeyNames().isEmpty());

    MobileKeyStore reopened(appData.path());
    QVERIFY(reopened.savedKeyNames().isEmpty());
}

void TestMobileKeyStore::savingAKeyPutsNothingInQSettings()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    // SPEC 11.2 permits the copy in ONE place: a file in app-private storage that
    // the user asked for. A profile still carries a reference or nothing, and no
    // key byte may reach the settings store, a cache or a stray temporary. So the
    // assertion is the strong one — the saved file is the ONLY file under this
    // app's storage that contains any part of the key — which covers every one of
    // those at once.
    QTemporaryDir appData;
    QVERIFY(appData.isValid());
    ch::SshConnectionPool pool;
    MobileKeyStore store(appData.path());
    store.setConnectionPool(&pool);

    QVERIFY(store.importKeyFromText(QStringLiteral("kept"),
                                    QString::fromLatin1(kPlainKey)));
    QVERIFY2(store.saveKeyOnDevice(QStringLiteral("kept")),
             qPrintable(store.lastError()));
    QVERIFY(store.applyIdentityForConnect(QStringLiteral("kept")));
    QVERIFY(!store.keyInfo(QStringLiteral("kept")).isEmpty());
    // Profiles may carry the saved file's path, but never its private bytes.
    QCOMPARE(store.referenceFor(QStringLiteral("kept")),
             savedKeyPath(appData.path(), QStringLiteral("kept")));
    store.forgetSession();

    const QString keysDirectory = savedKeysDirectory(appData.path());
    assertNoFileOutsideContains(appData.path(), keysDirectory, keyNeedle(),
                                "a saved key");
    // The per-app sandbox as well, which is where QSettings actually writes: the
    // store under test was pointed at its own temporary directory, so nothing
    // here is exempt at all.
    assertNoFileContainsAnywhere(sandboxRoots(), keyNeedle(), "a saved key");

    // And the settings file by name, with the exact constructor the client uses,
    // so this does not depend on the scan above having found it. QStandardPaths
    // test mode puts it inside the test's own tree.
    QSettings settings(QStringLiteral("CodeHarbor"), QStringLiteral("CodeHarbor"));
    settings.sync();
    QFile ini(settings.fileName());
    if (ini.open(QIODevice::ReadOnly))
        QVERIFY(!ini.readAll().contains(keyNeedle()));

    // The saved file, meanwhile, does hold the key — otherwise the assertions
    // above would pass against a save that wrote nothing at all.
    QFile saved(savedKeyPath(appData.path(), QStringLiteral("kept")));
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QVERIFY(saved.readAll().contains(keyNeedle()));
}

void TestMobileKeyStore::aFailedSavedKeyDeletionCanBeRetried()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    for (bool remove : {false, true}) {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        MobileKeyStore store(data.path());
        const QString name = QStringLiteral("work");
        QVERIFY(store.importKeyFromText(name, QString::fromLatin1(kPlainKey)));
        QVERIFY(store.saveKeyOnDevice(name));
        QSignalSpy removed(&store, &MobileKeyStore::keyRemoved);
        const QString path = savedKeyPath(data.path(), name);
        // A directory at the file's path makes QFile::remove fail even for an
        // elevated test process; no platform-specific permission assumption.
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir().mkdir(path));
        QVERIFY(!(remove ? store.removeKey(name) : store.forgetSavedKey(name)));
        QVERIFY(store.isSavedOnDevice(name));
        QVERIFY(store.allKeyNames().contains(name));
        QVERIFY(!store.lastError().isEmpty());
        QVERIFY(store.keyNames().isEmpty());
        QCOMPARE(removed.count(), 0);

        QVERIFY(QDir().rmdir(path));
        QFile restored(path);
        QVERIFY(restored.open(QIODevice::WriteOnly));
        QCOMPARE(restored.write(QByteArray(kPlainKey)), qint64(qstrlen(kPlainKey)));
        restored.close();
        QVERIFY(remove ? store.removeKey(name) : store.forgetSavedKey(name));
        QVERIFY(!store.isSavedOnDevice(name));
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(removed.count(), 1);
    }
}

void TestMobileKeyStore::forgettingAnAlreadyRemovedSavedKeySucceeds()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
    QTemporaryDir data;
    QVERIFY(data.isValid());
    MobileKeyStore store(data.path());
    const QString name = QStringLiteral("work");
    QVERIFY(store.importKeyFromText(name, QString::fromLatin1(kPlainKey)));
    QVERIFY(store.saveKeyOnDevice(name));
    QVERIFY(QFile::remove(savedKeyPath(data.path(), name)));
    QVERIFY(store.forgetSavedKey(name));
    QVERIFY(!store.isSavedOnDevice(name));
    QVERIFY(!store.hasKeys());
}

void TestMobileKeyStore::anUnsearchableSavedKeyDirectoryDoesNotImplyDeletion()
{
    if (!CH_HAVE_LIBSSH)
        QSKIP("This case requires the native SSH key parser.");
#ifndef Q_OS_UNIX
    QSKIP("POSIX directory search permissions are required.");
#else
    QTemporaryDir data;
    QVERIFY(data.isValid());
    MobileKeyStore store(data.path());
    const QString name = QStringLiteral("work");
    QVERIFY(store.importKeyFromText(name, QString::fromLatin1(kPlainKey)));
    QVERIFY(store.saveKeyOnDevice(name));
    const QString directory = savedKeysDirectory(data.path());
    const QString path = savedKeyPath(data.path(), name);
    const auto permissions = QFile::permissions(directory);
    QVERIFY(QFile::setPermissions(directory,
                                 QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QFile probe(path);
    if (probe.open(QIODevice::ReadOnly)) {
        probe.close();
        QVERIFY(QFile::setPermissions(directory, permissions));
        QSKIP("This process can bypass directory search permissions.");
    }
    QSignalSpy removed(&store, &MobileKeyStore::keyRemoved);
    const bool deleted = store.forgetSavedKey(name);
    const bool stillListed = store.isSavedOnDevice(name);
    const bool reportedError = !store.lastError().isEmpty();
    const int removalSignals = removed.count();
    // Restore before assertions so even a failing regression can clean up.
    QVERIFY(QFile::setPermissions(directory, permissions));
    QVERIFY(!deleted);
    QVERIFY(stillListed);
    QVERIFY(reportedError);
    QCOMPARE(removalSignals, 0);
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(store.forgetSavedKey(name));
    QVERIFY(!QFileInfo::exists(path));
#endif
}

// Guiless: nothing here draws, and a credential test must not depend on a
// display being reachable to prove that a key never reached disk.
QTEST_GUILESS_MAIN(TestMobileKeyStore)
#include "tst_mobilekeystore.moc"
