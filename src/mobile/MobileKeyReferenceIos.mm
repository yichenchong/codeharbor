#include "MobileKeyReference.h"

#include <QCoreApplication>
#include <QFile>

#import <Foundation/Foundation.h>

// The iOS half of the key-file REFERENCE (SPEC 11.2). Compiled only for iOS; the
// portable half lives in MobileKeyReference.cpp and calls the two functions
// below through the declarations in the header.
//
// WHY A BOOKMARK AND NOT A PATH. A document the user picks with UIDocumentPicker
// lives outside the app container. The URL the picker hands back carries a
// process-scoped sandbox extension: it is readable now and unreadable after the
// next launch, and the path inside it is not a path this app may open on its own.
// The durable form Apple provides is a security-scoped BOOKMARK — opaque data the
// app may persist and later resolve back into a URL it is allowed to read. That
// is precisely a "credential-store reference" in the sense of the local-state
// allowlist: it names a file the USER manages, it contains no key material, and
// it is useless to any other app or device.
//
// NOTHING IS COPIED. readIosBookmark() resolves, starts security-scoped access,
// reads the bytes into memory, and stops access again. The key never lands in the
// app container.

namespace ch::keyref {

// Both functions wrap their Objective-C work in @autoreleasepool. Every
// Foundation object below (the bookmark NSData, the resolved NSURL, the NSError)
// comes back autoreleased, so without a pool of our own they live until whatever
// pool the caller happens to be inside is drained — the iOS run loop's, if the
// call really did come from the UI thread as MobileKeyStore documents, and
// NOBODY'S if a future caller ever runs this off it. A pool here makes that
// question stop mattering and frees a multi-kilobyte bookmark at once rather
// than at the next run-loop turn.
QString makeIosBookmark(const QUrl& pickedUrl, QString* errorOut)
{
    if (errorOut)
        errorOut->clear();

    @autoreleasepool {
        NSURL* url = pickedUrl.toNSURL();
        if (!url) {
            if (errorOut) {
                *errorOut = QCoreApplication::translate(
                    "ch::keyref", "That location cannot be referenced.");
            }
            return {};
        }

        // The picked URL already carries its extension, so access has to be
        // started before the bookmark can be created from it. An unbalanced start
        // leaks a sandbox extension for the life of the process, so the stop is
        // unconditional — and it is conditional on `started` because
        // stopAccessingSecurityScopedResource must not be called for access that
        // was never granted.
        //
        // NSURLBookmarkCreationWithSecurityScope is deliberately NOT passed, and
        // neither is its resolution counterpart in readIosBookmark(): BOTH of
        // those options are macOS-only, and naming either one here is a compile
        // error on iOS rather than a portability wart. The iOS model is different:
        // a bookmark is made from a URL whose access is currently open, it is
        // resolved later with plain options, and the access is reopened by
        // -startAccessingSecurityScopedResource on the resolved URL. That call is
        // available on iOS and is the only thing that grants access there.
        //
        // Verified against the SDK rather than the documentation, because the two
        // disagree: Foundation's NSURL.h annotates both constants
        // API_AVAILABLE(macos, macCatalyst) API_UNAVAILABLE(ios, watchos, tvos),
        // while Apple's per-constant documentation pages show iOS badges. The
        // header is what the compiler enforces. Apple's current iOS article on
        // providing access to directories describes exactly the sequence used
        // here - picker URL, minimal bookmark, resolve with default options,
        // startAccessingSecurityScopedResource on the resolved URL - and the
        // creation documentation additionally forbids combining the scope option
        // with a minimal bookmark, so the minimal form is not a compromise.
        const bool started = [url startAccessingSecurityScopedResource];
        NSError* error = nil;
        NSData* bookmark =
            [url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark
                 includingResourceValuesForKeys:nil
                                  relativeToURL:nil
                                          error:&error];
        if (started)
            [url stopAccessingSecurityScopedResource];

        if (!bookmark) {
            if (errorOut) {
                const QString reason =
                    error ? QString::fromNSString(error.localizedDescription)
                          : QStringLiteral("unknown error");
                *errorOut =
                    QCoreApplication::translate(
                        "ch::keyref",
                        "iOS could not remember access to that file (%1). "
                        "Pick it again each time, or paste the key instead.")
                        .arg(reason);
            }
            return {};
        }

        const QByteArray data(static_cast<const char*>(bookmark.bytes),
                              qsizetype(bookmark.length));
        return QString(kBookmarkScheme) + QString::fromLatin1(data.toBase64());
    }
}

QByteArray readIosBookmark(const QString& reference, qint64 maxBytes,
                           QString* errorOut)
{
    if (errorOut)
        errorOut->clear();

    // kBookmarkScheme.size() directly: QString(kBookmarkScheme) allocated a
    // throwaway copy of a literal just to ask its length.
    //
    // fromBase64Encoding() answers a small result STRUCT, not a QByteArray and
    // not a pointer: it converts to bool for "did it decode" and carries the
    // bytes in its `decoded` member. It has no operator->, so the bytes have to
    // be taken from that member by name.
    const QByteArray::FromBase64Result result = QByteArray::fromBase64Encoding(
        reference.sliced(kBookmarkScheme.size()).toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!result || result.decoded.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "ch::keyref",
                "That stored key reference is damaged. Choose the key file "
                "again.");
        }
        return {};
    }
    const QByteArray &decoded = result.decoded;

    QByteArray contents;
    QString failure;
    @autoreleasepool {
        NSData* bookmark = [NSData dataWithBytes:decoded.constData()
                                         length:NSUInteger(decoded.size())];
        BOOL stale = NO;
        NSError* error = nil;
        // NSURLBookmarkResolutionWithoutUI, NOT
        // NSURLBookmarkResolutionWithSecurityScope: that option is macOS-only and
        // is a hard compile error here ("unavailable: not available on iOS"). On
        // iOS the security scope does not come from a resolution flag at all — a
        // bookmark made from a document the picker handed over resolves with plain
        // options, and access is opened by -startAccessingSecurityScopedResource
        // on the resolved URL, which IS available on iOS and is what the code
        // below calls. WithoutUI is used because this runs on the UI thread while
        // the user waits for a connection: it forbids the system from putting up
        // any of its own dialogs to resolve the reference.
        NSURL* url = [NSURL URLByResolvingBookmarkData:bookmark
                                              options:NSURLBookmarkResolutionWithoutUI
                                        relativeToURL:nil
                                  bookmarkDataIsStale:&stale
                                                error:&error];
        if (!url) {
            if (errorOut) {
                const QString reason =
                    error ? QString::fromNSString(error.localizedDescription)
                          : QStringLiteral("unknown error");
                *errorOut = QCoreApplication::translate(
                                "ch::keyref",
                                "The referenced key file could not be opened "
                                "(%1). Choose it again.")
                                .arg(reason);
            }
            return {};
        }
        // A stale bookmark still resolved, so it is used rather than refused; the
        // caller re-references the file the next time the user picks one, which is
        // the only moment a fresh bookmark can be made. Refusing here would lock
        // the user out of a file that is demonstrably still readable.

        if (![url startAccessingSecurityScopedResource]) {
            if (errorOut) {
                *errorOut = QCoreApplication::translate(
                    "ch::keyref",
                    "iOS did not grant access to the referenced key file. Choose "
                    "it again.");
            }
            return {};
        }

        // A resolved URL is not guaranteed to be a file URL, and -path is nil for
        // one that is not. QString::fromNSString(nil) is an empty string, which
        // QFile would then report as a mysterious open failure on ""; say what
        // actually happened instead. The access started above is stopped either
        // way, because an unbalanced start leaks a sandbox extension for the life
        // of the process.
        const QString path =
            url.path ? QString::fromNSString(url.path) : QString();
        if (path.isEmpty()) {
            failure = QCoreApplication::translate(
                "ch::keyref", "the reference does not name a file");
        } else {
            QFile in(path);
            if (!in.open(QIODevice::ReadOnly)) {
                failure = in.errorString();
            } else {
                // One byte past the bound, so an oversized pick is detected
                // rather than truncated into something that looks like a damaged
                // key.
                contents = in.read(maxBytes + 1);
                in.close();
            }
        }
        [url stopAccessingSecurityScopedResource];
    }

    if (!failure.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                            "ch::keyref", "Could not read the key file (%1).")
                            .arg(failure);
        }
        return {};
    }
    if (contents.size() > maxBytes) {
        // Overwritten rather than merely dropped: whatever this was, it was read
        // in the belief that it might be a private key, and the caller that would
        // normally wipe it is never handed it.
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

}  // namespace ch::keyref
