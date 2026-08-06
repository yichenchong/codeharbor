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
#
# A hunk ends when the DECLARED line counts are satisfied, not when a line that
# looks like a file header turns up. That distinction is the whole reason the
# reader is count-driven: inside a hunk, deleting a line whose own text begins
# `-- ` produces the body line `--- foo`, and adding a line beginning `++ `
# produces `+++ foo`. Treating either as the start of a new file's diff would
# end the hunk early and report a perfectly valid patch as malformed. git's own
# parser reads exactly the declared number of body lines for the same reason.
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
            completed = 1
        }
        /^@@ / {
            seen_hunk = 1
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
        !inhunk && /^(diff --git |--- |\+\+\+ |index |new file |deleted file |old mode |new mode |similarity |rename )/ {
            # These are file-level metadata, including the ambiguous-looking
            # ---/+++ headers. They are valid immediately after a completed
            # hunk and must not be counted as removed/added body lines.
            completed = 0
            next
        }
        inhunk && (old < oldwant || new < newwant) {
            if ($0 ~ /^\\/) next          # "\ No newline at end of file"
            c = substr($0, 1, 1)
            if (c == " " || $0 == "") {
                old++; new++
                if (old >= oldwant && new >= newwant) { inhunk = 0; completed = 1 }
                next
            }
            if (c == "-") {
                old++
                if (old >= oldwant && new >= newwant) { inhunk = 0; completed = 1 }
                next
            }
            if (c == "+") {
                new++
                if (old >= oldwant && new >= newwant) { inhunk = 0; completed = 1 }
                next
            }
            printf "%s:%d: body line starts with %s, not a space, + or -\n",
                file, NR, (c == "" ? "nothing" : "\"" c "\"")
            bad = 1
            next
        }
        completed && ($0 == "" || $0 ~ /^[ +-]/) {
            printf "%s:%d: body line appears after hunk counts are complete\n",
                file, NR
            bad = 1
            completed = 0
            next
        }
        # The declared counts are met (or we were never in a hunk), so this line
        # belongs to the surrounding file-level diff. Close the hunk and ignore
        # it: everything outside a hunk body is metadata this check does not
        # police.
        { flush() }
        END {
            flush()
            if (!seen_hunk) {
                printf "%s: no unified-diff hunks found\n", file
                bad = 1
            }
            exit bad ? 1 : 0
        }
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
fi

# Second contract: the portfile's PATCHES list and the patch files on disk must
# name exactly the same set.
#
# The loop above proves each patch PARSES; it says nothing about whether vcpkg
# will ever look at it. A patch that is renamed, or added to the directory and
# never listed, is invisible here: `vcpkg_extract_source_archive` applies only
# what PATCHES names, and it aborts the whole port when a named file is absent.
# Either way the first thing to notice is a Windows release job dying during
# extraction, ten minutes into a build for a tag that is already permanent.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

ports=0
while IFS= read -r -d '' portfile; do
    ports=$((ports + 1))
    portdir="$(dirname "$portfile")"

    # The names between `PATCHES` and the closing `)` of the
    # vcpkg_extract_source_archive() call. Comments and blank lines are ignored;
    # anything else in that block is a filename relative to the port directory.
    awk '
        !inlist && $1 == "PATCHES" {
            inlist = 1
            for (i = 2; i <= NF; i++) print $i
            next
        }
        inlist && /^[[:space:]]*\)/ { inlist = 0; next }
        inlist {
            sub(/#.*/, "")
            gsub(/^[[:space:]]+|[[:space:]]+$/, "")
            if ($0 != "") print
        }
    ' "$portfile" | sort >"$work/listed"

    find "$portdir" -maxdepth 1 -type f \( -name '*.patch' -o -name '*.diff' \) \
        -exec basename {} \; | sort >"$work/present"

    if ! diff -u "$work/listed" "$work/present" >"$work/diff"; then
        echo "vcpkg_patches.sh: $portfile's PATCHES list does not match $portdir" >&2
        echo "  (-) listed in the portfile but not on disk: vcpkg aborts the port." >&2
        echo "  (+) on disk but not listed: the patch is never applied." >&2
        sed -n '3,$p' "$work/diff" >&2
        status=1
    fi
done < <(find "$dir" -type f -name portfile.cmake -print0)

if [ "$ports" -eq 0 ]; then
    echo "vcpkg_patches.sh: found no portfile.cmake under $dir; the patches above" >&2
    echo "are applied by nothing, so the check would pass vacuously" >&2
    exit 1
fi

if [ "$status" -ne 0 ]; then
    exit 1
fi

echo "vcpkg_patches.sh: $found patch(es) are well formed and are exactly what" \
     "$ports portfile(s) apply"
