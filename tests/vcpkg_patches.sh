#!/usr/bin/env bash
# Structural check of the in-tree vcpkg overlay patches.
#
# WHY THIS EXISTS. The patches under packaging/vcpkg-ports/ are unified diffs
# applied to a source tarball that is downloaded on a Windows release runner.
# Nothing in an ordinary Linux or macOS build reads them, and the runner caches
# the built dependency, so a broken patch can sit in the tree unnoticed and then
# fail a release build ten minutes in, long after the tag is permanent.
#
# It has happened. A reviewer saw that one line reads
#   set(LIBSSSH_PC_REQUIRES_PRIVATE "")
# with three S where the rest of the file uses two, "corrected" the spelling,
# and in doing so deleted the single leading space that marked the line as
# CONTEXT. Two separate faults in one edit: a context line must reproduce the
# upstream file byte for byte (upstream really does contain that misspelling),
# and a body line with no marker at all makes git reject the hunk outright with
# "corrupt patch at line 10".
#
# WHAT IS CHECKED. For every hunk: that each body line starts with a space, a
# plus, or a minus, and that the number of old-side and new-side lines matches
# the counts declared in the @@ header. Those two rules are exactly what git
# apply verifies before it looks at the target file, so anything that passes
# here will at least parse there. This deliberately does NOT try to apply the
# patches: that needs the upstream tarball and a network fetch, which is not
# something a unit test should do.
set -euo pipefail

root=${1:-.}
cd "$root"

dir=packaging/vcpkg-ports
if [ ! -d "$dir" ]; then
    echo "vcpkg_patches.sh: no $dir directory to check" >&2
    exit 1
fi

# A trailing newline after the last hunk is optional in a diff, and a "\ No
# newline at end of file" marker is a legal body line that belongs to neither
# side. Both are handled by the reader below.
status=0
found=0
while IFS= read -r -d '' patch; do
    found=$((found + 1))
    if ! awk -v file="$patch" '
        function flush() {
            if (!inhunk) return
            if (old != oldwant || new != newwant) {
                printf "%s:%d: hunk declares -%d,+%d but body has -%d,+%d\n",
                    file, hunkline, oldwant, newwant, old, new
                bad = 1
            }
            inhunk = 0
        }
        /^@@ / {
            flush()
            if (match($0, /^@@ -[0-9]+(,[0-9]+)? \+[0-9]+(,[0-9]+)? @@/) == 0) {
                printf "%s:%d: malformed hunk header\n", file, NR
                bad = 1
                next
            }
            split($2, a, ",")
            split($3, b, ",")
            oldwant = (2 in a) ? a[2] : 1
            newwant = (2 in b) ? b[2] : 1
            old = 0; new = 0; inhunk = 1; hunkline = NR
            next
        }
        /^(diff --git |--- |\+\+\+ |index |new file |deleted file |old mode |new mode |similarity |rename )/ {
            # A file header ends the previous hunk. Note this must come AFTER
            # the @@ rule but BEFORE the body rule, because "--- " and "+++ "
            # would otherwise be read as ordinary removed and added lines.
            if (!inhunk) next
            flush()
            next
        }
        inhunk {
            if ($0 ~ /^\\/) next          # "\ No newline at end of file"
            c = substr($0, 1, 1)
            if (c == " " || $0 == "") { old++; new++; next }
            if (c == "-") { old++; next }
            if (c == "+") { new++; next }
            printf "%s:%d: body line starts with %s, not a space, + or -\n",
                file, NR, (c == "" ? "nothing" : "\"" c "\"")
            bad = 1
        }
        END { flush(); exit bad ? 1 : 0 }
    ' "$patch"; then
        status=1
    fi
done < <(find "$dir" -type f \( -name '*.patch' -o -name '*.diff' \) -print0)

if [ "$found" -eq 0 ]; then
    echo "vcpkg_patches.sh: found no patches under $dir; the check would pass vacuously" >&2
    exit 1
fi

if [ "$status" -ne 0 ]; then
    echo "vcpkg_patches.sh: $found patch(es) checked, at least one is malformed" >&2
    echo "A context line must reproduce the upstream file EXACTLY, including any" >&2
    echo "upstream typo, and must keep its leading space." >&2
    exit 1
fi

echo "vcpkg_patches.sh: $found patch(es) are well formed"
