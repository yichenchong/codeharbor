#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <QtGlobal>

namespace ch::keyref {

// A REFERENCE to a private key the USER manages, in the user's own storage —
// never a copy of the key in the client's storage (SPEC 11.2, docs/SPEC.md:
// 1710-1729: the local-state allowlist admits "SSH-agent or credential-store
// references" and "an optional local identity-file path", and private-key bytes
// are deliberately absent from it).
//
// The three platforms need three different things to make a reference SURVIVE a
// relaunch, which is the only reason this file exists:
//
//   * DESKTOP (the buildable/testable mobile shell on Linux, macOS, Windows): an
//     ordinary absolute path. Nothing to encode and nothing to authorise.
//
//   * ANDROID: the document picker returns a `content://` URI whose read grant
//     dies with the process unless the app asks the ContentResolver to make it
//     persistable. takePersistableReadPermission() below is that one call. Qt's
//     file dialog does not make it for us — it has no way to know whether the
//     caller wants the grant to outlive the pick — so a reference stored without
//     it resolves to "permission denied" on the next launch. Requesting it does
//     not copy a single byte: the bytes stay in whatever provider the user chose.
//
//   * iOS: a picked document lives outside the app container and is reachable
//     only through a SECURITY-SCOPED URL, which is likewise process-scoped. The
//     durable form is a security-scoped BOOKMARK — opaque data the app may store
//     and later resolve back into a URL it is allowed to read. That is
//     credential-store-reference-shaped by construction: it names a file, it
//     carries no key material, and it is useless to anyone but this app on this
//     device.
//
// Encoded references are therefore one of:
//   "/abs/path"                     desktop, and any plain local path
//   "content://…"                   Android document URI (grant taken)
//   "chbookmark:<base64>"           iOS security-scoped bookmark
//
// All three are opaque strings as far as the rest of the client is concerned, so
// a reference travels in ch::ServerProfiles' EXISTING identityFile field with no
// new profile field and no change to its whitelist.
//
// ONE PLACE HAS TO TELL THEM APART FROM A PATH, and it is not this file. That
// same profile field is handed to SshConnectionPool::connectToHost() as an
// identity FILE, so the pool drops a value that is a URI rather than a path
// (SshConnectionPool::identityFilePathFor). Its rule is a SHAPE test — a scheme
// of two or more letters, then a colon — and it does NOT reference the constant
// below, deliberately: src/mobile links against src/ssh, so an include in the
// other direction would be a cycle in the build graph. The two definitions are
// independent on purpose and cannot drift, because the pool asks "is this a URI
// at all", not "is this one of the schemes the mobile client mints". Adding a
// third reference shape here therefore needs nothing changed there, as long as it
// is either a plain path or a genuinely URI-shaped string.
const QLatin1String kBookmarkScheme{"chbookmark:"};

// Ask the platform to make a picked document's read grant durable, so the
// reference stored now still resolves after a relaunch. Returns the reference to
// store, or an empty string when the pick cannot be turned into a durable
// reference at all (the caller then keeps the key in memory for this session
// only, which is always available). Never copies the file.
QString makeDurableReference(const QUrl& pickedUrl, QString* errorOut);

// Read the key bytes a reference points at, into memory. `maxBytes` bounds the
// read exactly as MobileKeyStore::kMaxKeyBytes does, because a reference can
// point at anything by the time it is resolved. Empty on failure with *errorOut
// set. The bytes are the caller's to wipe; nothing is cached here.
QByteArray readReference(const QString& reference, qint64 maxBytes,
                         QString* errorOut);

// True for a reference this build can resolve at all. Used by the UI to explain
// a stored profile whose reference belongs to another platform (a synced
// profile, an ini copied between machines) instead of failing at connect time.
bool isResolvableReference(const QString& reference);

#ifdef Q_OS_IOS
// The iOS half, implemented in MobileKeyReferenceIos.mm because it needs
// Foundation. Declared here so the portable code above can call it without an
// Objective-C include leaking into every translation unit.
//
// makeIosBookmark() returns "chbookmark:<base64 bookmark data>";
// readIosBookmark() resolves one, starts security-scoped access, reads, and stops
// access again — the start/stop pair is mandatory and unbalanced calls leak a
// sandbox extension for the life of the process.
QString makeIosBookmark(const QUrl& pickedUrl, QString* errorOut);
QByteArray readIosBookmark(const QString& reference, qint64 maxBytes,
                           QString* errorOut);
#endif

}  // namespace ch::keyref
