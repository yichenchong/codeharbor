.pragma library

// Shared conversion from a remote file:// URL to the plain server-side path the
// remote file service speaks (SPEC 8.3): file:// inside CodeHarbor always means
// the remote SSH server. Strip the "file://" scheme, then percent-decode.
//
// A remote file:// URL never carries a host, so whatever follows the scheme is
// already the server-absolute path. A value that is not a file:// URL is left
// untouched. This is a security-relevant conversion, so it lives in exactly one
// place: ViewerPane, ViewerDirectoryView and EditorPaneView all
// call here instead of each re-implementing the rule (QM17).
function fileUrlToPath(url) {
    // String(url), not url.toString(): callers pass a string (a QML `url` value
    // stringifies to "" when marshalled into a .pragma library context, so the
    // conversion to a string must happen in the caller's QML scope), and String()
    // is safe on any value a caller might pass.
    var s = String(url);
    if (s.indexOf("file://") === 0)
        return decodeURIComponent(s.substring("file://".length));
    return s;
}

// The exact inverse of fileUrlToPath(): the remote POSIX path `path` spelled as
// the file:// URL the viewer stack passes around.
//
// Each SEGMENT is percent-encoded and the "/" separators are left alone, so
// decodeURIComponent() on the result returns the original path character for
// character. encodeURI() would NOT do: it leaves "#" and "?" unescaped, so a
// remote file named "notes#1" would silently become a URL with a fragment and
// the wrong file would be read.
//
// Paths here are always remote POSIX paths, so "/" is the ONE separator: a
// backslash is an ordinary character in a file name and is encoded like any
// other, never treated as a separator.
function pathToFileUrl(path) {
    var parts = String(path).split("/");
    for (var i = 0; i < parts.length; ++i)
        parts[i] = encodeURIComponent(parts[i]);
    return "file://" + parts.join("/");
}
