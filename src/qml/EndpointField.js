.pragma library

// The client-side copy of the rule ServerProfiles::isUsableEndpointField()
// enforces on a saved server profile's HOST and LOGIN NAME
// (src/app/ServerProfiles.cpp).
//
// That function is the authority: it refuses the write and it skips such a row
// when reading the settings file. This copy exists only so the two surfaces
// that edit a profile — ConnectSheet's Save button and the Settings window's
// Server pane — can say NO BEFORE the user presses Save, instead of reporting
// success while the store silently drops the record. Both of them call here
// rather than each re-deriving the rule, for the same reason RemotePath.js
// exists: one rule, one spelling.
//
// The rejected set is exactly the two character classes the C++ rejects:
//
//   \u0000-\u0020   the C0 controls, and the space
//   \u007f-\u00a0   DEL, the C1 controls (U+0085 NEL included), and NBSP
//   the rest        the Unicode space / line / paragraph separators above that
//                   (Zs, Zl, Zp)
//
// which together are precisely `QChar::isSpace() || category() ==
// QChar::Other_Control`.
//
// Deliberately NOT hostname grammar. "@", ":" and "%" stay legal, because an
// IPv6 literal with a zone identifier ("fe80::1%eth0") is a value people
// genuinely store, and a client that second-guesses libssh's own parsing
// refuses connections that work.
var REJECTED =
    /[\u0000-\u0020\u007f-\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]/;

// QString::trimmed() strips exactly the characters above, so trimming with the
// same set is what makes the value this module judges the same value the store
// will judge. Written as a scan rather than a second /.../g regular expression
// so the character class has ONE spelling; a non-global RegExp has no lastIndex
// state, so `test` here is safe to call repeatedly.
function trim(text) {
    var value = String(text);
    var start = 0;
    var end = value.length;
    while (start < end && REJECTED.test(value.charAt(start)))
        ++start;
    while (end > start && REJECTED.test(value.charAt(end - 1)))
        --end;
    return value.substring(start, end);
}

// Can this field be stored at all? Non-empty once trimmed, and free of the
// rejected characters inside it.
function isUsable(text) {
    var value = trim(text);
    return value.length > 0 && !REJECTED.test(value);
}

// The field holds something, but something the store will refuse. Separated
// from isUsable() so a caller can tell "you have not filled this in yet" apart
// from "what you pasted in here cannot be a host", which are different
// sentences to put in front of a user.
function hasRejectedCharacters(text) {
    var value = trim(text);
    return value.length > 0 && REJECTED.test(value);
}
