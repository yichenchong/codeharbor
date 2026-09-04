#include "MobileKeyStore.h"

#include <QCoreApplication>

#import <Foundation/Foundation.h>

// The iOS half of the SAVED KEY (path 3 in MobileKeyStore.h): keeping a file the
// user asked this device to hold out of the device's backups. Compiled only for
// iOS; MobileKeyStore.cpp calls it under Q_OS_IOS through the declaration in the
// header.
//
// WHY THIS FILE EXISTS AT ALL, when Android needs no code for the same
// guarantee. Android has one switch for the whole sandbox —
// android:allowBackup="false" in packaging/android/AndroidManifest.xml, already
// set — so nothing in the app's private storage is ever copied off the device by
// the platform. iOS has no such switch: everything in the app container except
// <Library/Caches> and <tmp> is included in iCloud and iTunes/Finder backups by
// default, and the only opt-out is per URL, set AFTER the file exists.
//
// Neither of the two directories iOS excludes by itself is a place for this file.
// <Caches> is reclaimable at the system's discretion — a key that vanishes when
// the device is low on storage is not a key the user asked to keep — and <tmp>
// does not survive at all, which is the opposite of the point.
//
// WHAT THE EXCLUSION IS AND IS NOT. It stops the key being copied into a backup
// and thereby onto any other device restored from it. It is not encryption and
// not an access control: anyone who can read this app's container on an unlocked
// device can read the file, exactly as MobileKeyStore.h says.

namespace ch {

bool excludeFromDeviceBackup(const QString& path, QString* errorOut)
{
    if (errorOut)
        errorOut->clear();

    // @autoreleasepool for the same reason MobileKeyReferenceIos.mm gives: the
    // NSURL and the NSError below come back autoreleased, so without a pool of
    // our own they live until whatever pool the caller happens to be inside is
    // drained — the run loop's if this really was called from the UI thread, and
    // NOBODY'S if a future caller ever runs it off one.
    @autoreleasepool {
        // fileURLWithPath: and not QUrl::toNSURL() on a QUrl built from a path:
        // this is a plain filesystem path that this class composed itself, and
        // the round trip through a URL string would put percent-encoding between
        // the path that was written and the path being marked.
        NSURL* url = [NSURL fileURLWithPath:path.toNSString() isDirectory:NO];
        if (!url) {
            if (errorOut) {
                *errorOut = QCoreApplication::translate(
                    "ch::MobileKeyStore",
                    "The saved key could not be kept out of the device backup, "
                    "so it was not saved.");
            }
            return false;
        }

        NSError* error = nil;
        // The value is set on the FILE, so this call has to come after the write
        // — an exclusion set on a path that does not exist yet fails, and the
        // caller deletes the file when it does.
        const BOOL ok = [url setResourceValue:@YES
                                       forKey:NSURLIsExcludedFromBackupKey
                                        error:&error];
        if (!ok) {
            if (errorOut) {
                const QString reason =
                    error ? QString::fromNSString(error.localizedDescription)
                          : QStringLiteral("unknown error");
                *errorOut =
                    QCoreApplication::translate(
                        "ch::MobileKeyStore",
                        "The saved key could not be kept out of the device "
                        "backup (%1), so it was not saved. Use the key for this "
                        "session instead.")
                        .arg(reason);
            }
            return false;
        }
        return true;
    }
}

}  // namespace ch
