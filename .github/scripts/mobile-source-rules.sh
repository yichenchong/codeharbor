#!/usr/bin/env bash
# The two source-level rules the mobile client cannot be allowed to break
# (docs/SPEC.md 7.5 / 2.4), checked by reading the sources:
#
#   1. Every server-controlled string it renders is Text.PlainText. The
#      rich-text modes are what turn a file name, a commit message or a diff
#      line coming off the dev server into markup the client executes.
#   2. Nothing under src/mobile or src/vt names Qt WebEngine, Qt WebChannel,
#      QtWidgets or QtDBus. WebEngine and WebChannel exist on neither Android
#      nor iOS; QtWidgets and QtDBus are banned so the mobile shell cannot grow
#      a desktop-only dependency that happens to link on a workstation.
#
# It lives in a script rather than inline in a workflow because BOTH the CI
# workflow (which gates every commit) and the release workflow (which gates a
# tag, and a tag can be pushed at a tree CI never saw) have to run it, and a
# second copy of these patterns is a second thing to keep in step with the
# specification.
#
# Must be run from the repository root. Prints what it found and exits non-zero
# if anything did.
set -euo pipefail

# The rich-text enumerators are matched UNQUALIFIED (`RichText`, not
# `Text.RichText`): the same values are spelled `TextEdit.RichText` on a
# TextEdit/TextArea and `TextInput.RichText` on an input, so a pattern anchored
# on `Text.` walked straight past the editable fields - which are exactly where
# KeyImportSheet.qml pastes text the user did not type. Once comments are
# stripped, these four words have no legitimate use in this tree.
banned='\b(MarkdownText|StyledText|RichText|AutoText)\b'
banned="$banned|QtWebEngine|QWebEngine|QtWebChannel|QWebChannel|qwebchannel\.js"
banned="$banned|QtWidgets|QApplication|QWidget|QtDBus|QDBus"

# Comments are stripped before matching, because the sources explain WHY these
# modes are banned by naming them - a check that trips over its own
# documentation is a check somebody deletes.
#
# Block comments are replaced by their own newlines rather than deleted, so the
# line numbers printed below still match the file. `(?<!:)` keeps a `//` that
# follows a colon - the `https://` inside a string literal - from being taken
# for a comment start, which would blank the rest of a real line of code.
strip='s{/\*.*?\*/}{$& =~ tr/\n//cdr}gse; s{(?<!:)//[^\n]*}{}g'

# .mm is in the list because src/mobile/MobileKeyReferenceIos.mm is
# Objective-C++, and an #import there is as much a dependency as an #include
# anywhere else.
#
# The loop runs in the pipeline's subshell, so it reports its verdict through
# the exit status rather than through a variable the parent cannot see.
find src/mobile src/vt -type f \
     \( -name '*.qml' -o -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) |
  sort | {
    clean=0
    while IFS= read -r file; do
      hits=$(perl -0777 -pe "$strip" "$file" | grep -nE "$banned" || true)
      if [ -n "$hits" ]; then
        clean=1
        echo "$file:"
        echo "$hits"
      fi
    done
    [ "$clean" -eq 0 ]
  } || {
    echo "the mobile client must render server-controlled text as" >&2
    echo "Text.PlainText and must not reference WebEngine, WebChannel," >&2
    echo "QtWidgets or QtDBus (SPEC 7.5 / 2.4)." >&2
    exit 1
  }
echo "mobile sources are free of rich text and of WebEngine, WebChannel,"
echo "QtWidgets and QtDBus."
