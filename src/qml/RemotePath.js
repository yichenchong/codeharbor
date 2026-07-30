.pragma library

// Shared conversion from a remote file:// URL to the plain server-side path the
// remote file service speaks (SPEC 8.3): file:// inside CodeHarbor always means
// the remote SSH server. Strip the "file://" scheme, then percent-decode.
//
// A remote file:// URL never carries a host, so whatever follows the scheme is
// already the server-absolute path. A value that is not a file:// URL is left
// untouched. This is a security-relevant conversion, so it lives in exactly one
// place: ViewerTextView, ViewerDirectoryView and EditorPaneView all call here
// instead of each re-implementing the rule (QM17).
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
