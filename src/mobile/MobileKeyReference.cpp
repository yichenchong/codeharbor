#include "MobileKeyReference.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace ch::keyref {

namespace {

// Read an already-openable location into memory, bounded. Shared by every
// platform: once a reference has been turned into something QFile can open, the
// rest is identical, and Qt's Android file engine opens a `content://` URI
// directly so there is nothing platform-specific left at this point.
QByteArray readOpenable(const QString& path, qint64 maxBytes, QString* errorOut)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                            "ch::keyref", "Could not read the key file (%1).")
                            .arg(in.errorString());
        }
        return {};
    }
    // One byte past the bound rather than trusting size(): a content provider may
    // not report a size at all.
    QByteArray contents = in.read(maxBytes + 1);
    in.close();
    if (contents.size() > maxBytes) {
        // Overwritten rather than merely dropped: it was read in the belief that
        // it might be a private key, and the caller that would normally wipe it
        // never receives it.
        contents.fill('\0');
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref",
                "That file is far too large to be a private key, so it was not "
                "read.");
        }
        return {};
    }
    if (contents.isEmpty() && errorOut) {
        *errorOut = QCoreApplication::translate("ch::keyref",
                                                "The key file is empty.");
    }
    return contents;
}

#ifdef Q_OS_ANDROID

// ContentResolver.takePersistableUriPermission(uri, FLAG_GRANT_READ_URI_PERMISSION).
//
// Without this the grant the picker handed us is scoped to this process: the
// stored reference would resolve to SecurityException on the next launch, and the
// user would be told their saved server has an unreadable key for no visible
// reason. Read-only on purpose — this client never writes to the document.
bool takePersistableReadPermission(const QString& uriString, QString* errorOut)
{
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref",
                "The Android application context was not available, so the key "
                "file cannot be remembered.");
        }
        return false;
    }
    const QJniObject resolver = context.callObjectMethod(
        "getContentResolver", "()Landroid/content/ContentResolver;");
    if (!resolver.isValid()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref", "The Android content resolver was not available.");
        }
        return false;
    }
    const QJniObject uri = QJniObject::callStaticObjectMethod(
        "android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;",
        QJniObject::fromString(uriString).object<jstring>());
    if (!uri.isValid()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref", "That location is not a readable document URI.");
        }
        return false;
    }
    // Intent.FLAG_GRANT_READ_URI_PERMISSION. Spelled as the literal rather than
    // read back through JNI: it is a public, frozen platform constant, and one
    // more reflective call here would be a second thing that can fail.
    constexpr jint kFlagGrantRead = 0x00000001;
    resolver.callMethod<void>("takePersistableUriPermission",
                             "(Landroid/net/Uri;I)V", uri.object<jobject>(),
                             kFlagGrantRead);
    // A refused grant arrives as a Java exception, not a return value.
    QJniEnvironment env;
    if (env.checkAndClearExceptions()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref",
                "Android refused to remember access to that file. Pick it again "
                "each time, or paste the key instead.");
        }
        return false;
    }
    return true;
}

#endif  // Q_OS_ANDROID

}  // namespace

bool isResolvableReference(const QString& reference)
{
    if (reference.isEmpty())
        return false;
    if (reference.startsWith(kBookmarkScheme)) {
        // An iOS bookmark is meaningless to any other platform, and saying so is
        // the whole point of this predicate.
#ifdef Q_OS_IOS
        return true;
#else
        return false;
#endif
    }
    if (reference.startsWith(QLatin1String("content://"))) {
#ifdef Q_OS_ANDROID
        return true;
#else
        return false;
#endif
    }
    // A plain path — the desktop builds of this shell, and anything an Android
    // document provider handed over as a real file. Existence and readability
    // ARE checked here, because this predicate's caller
    // (MobileKeyStore::registerReference) runs when a saved profile is loaded on
    // the connect page, and that is the moment at which a key file that is gone
    // can still be explained to the user. Deferring it to the read means the
    // profile silently offers a credential that cannot work, and the user finds
    // out as a failed handshake with no obvious cause.
    //
    // A path that is merely unavailable right now (an unmounted share) is
    // refused on the same footing, and the remedy is the same: pick the key
    // again. That is a worse answer than retrying would be, and a much better
    // one than a connection that fails for reasons the UI never names.
    const QFileInfo info(reference);
    return info.isFile() && info.isReadable();
}

QString makeDurableReference(const QUrl& pickedUrl, QString* errorOut)
{
    if (errorOut)
        errorOut->clear();
    if (pickedUrl.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate("ch::keyref",
                                                    "No file was chosen.");
        }
        return {};
    }

#ifdef Q_OS_IOS
    // A picked document lives outside the container; only a bookmark survives.
    return makeIosBookmark(pickedUrl, errorOut);
#endif

    if (pickedUrl.scheme() == QLatin1String("content")) {
#ifdef Q_OS_ANDROID
        const QString uriString = pickedUrl.toString();
        if (!takePersistableReadPermission(uriString, errorOut))
            return {};
        return uriString;
#else
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref",
                "This build cannot remember Android document locations.");
        }
        return {};
#endif
    }

    const QString path =
        pickedUrl.isLocalFile() ? pickedUrl.toLocalFile()
                                : (pickedUrl.scheme().isEmpty()
                                       ? pickedUrl.toString()
                                       : QString());
    if (path.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref",
                "That location cannot be referenced. Paste the key text "
                "instead.");
        }
        return {};
    }
    // An absolute path, so a reference stored from one working directory still
    // resolves from another.
    return QFileInfo(path).absoluteFilePath();
}

QByteArray readReference(const QString& reference, qint64 maxBytes,
                         QString* errorOut)
{
    if (errorOut)
        errorOut->clear();
    if (reference.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate("ch::keyref",
                                                    "No key file is referenced.");
        }
        return {};
    }

    if (reference.startsWith(kBookmarkScheme)) {
#ifdef Q_OS_IOS
        return readIosBookmark(reference, maxBytes, errorOut);
#else
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref",
                "That key reference was saved on iOS and cannot be resolved on "
                "this device. Choose the key file again.");
        }
        return {};
#endif
    }

    if (reference.startsWith(QLatin1String("content://"))) {
#ifndef Q_OS_ANDROID
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref",
                "That key reference was saved on Android and cannot be resolved "
                "on this device. Choose the key file again.");
        }
        return {};
#endif
        // On Android the URI IS the path: Qt's file engine opens it.
    }

    return readOpenable(reference, maxBytes, errorOut);
}

}  // namespace ch::keyref
