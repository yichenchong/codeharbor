#!/usr/bin/env bash
#
# Sign Android packages with a keystore, then PROVE the signature is one Android
# will accept.
#
# Why this exists as a script rather than inline in each workflow: the release
# job signs with the project's real upload key, which only exists as repository
# secrets, so that code path cannot run on a pull request or a dry run. Every
# CI run signs the same way with a throwaway key, so this file - the part that
# is easy to get wrong - is exercised continuously instead of only when a tag is
# cut. That is the same reason release-version.sh is a script.
#
# Usage:
#   sign-android.sh <keystore> <alias> <min-sdk> <file> [file...]
#
# Passwords come from the environment, never argv, because argv is visible to
# every process on the machine:
#   CH_KEYSTORE_PASSWORD   store password  (required)
#   CH_KEY_PASSWORD        key password    (optional; defaults to the store one)
#
# .apk and .aab are signed by DIFFERENT tools, and that is not interchangeable:
#   - An APK is what a device installs, so it needs an APK Signature Scheme v2+
#     block (mandatory for anything installed on API 30+). Only apksigner emits
#     one; jarsigner cannot.
#   - An AAB is not installed by anything. Play re-signs the APKs it generates,
#     and the bundle only has to carry a valid JAR signature from the upload
#     key. apksigner rejects bundles outright, so jarsigner is correct there.
set -euo pipefail

if [ "$#" -lt 4 ]; then
    echo "usage: $0 <keystore> <alias> <min-sdk> <file> [file...]" >&2
    exit 2
fi

keystore=$1
alias=$2
min_sdk=$3
shift 3

: "${CH_KEYSTORE_PASSWORD:?CH_KEYSTORE_PASSWORD is not set}"
key_password=${CH_KEY_PASSWORD:-$CH_KEYSTORE_PASSWORD}

if [ ! -f "$keystore" ]; then
    echo "keystore '$keystore' does not exist" >&2
    exit 1
fi

# Locate the build tools. The workflows pin a build-tools version through
# ANDROID_SDK_PACKAGES and install it with sdkmanager, so prefer that exact
# directory and fall back to the highest installed one; a signature produced by
# a tool nobody chose is the kind of thing this repository pins elsewhere.
sdk_root=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
if [ -z "$sdk_root" ] || [ ! -d "$sdk_root/build-tools" ]; then
    echo "no Android SDK build-tools directory (ANDROID_SDK_ROOT='$sdk_root')" >&2
    exit 1
fi

pinned=""
if [ -n "${ANDROID_SDK_PACKAGES:-}" ]; then
    case "$ANDROID_SDK_PACKAGES" in
        *build-tools\;*)
            pinned=${ANDROID_SDK_PACKAGES#*build-tools;}
            pinned=${pinned%% *}
            ;;
    esac
fi

build_tools=""
if [ -n "$pinned" ] && [ -d "$sdk_root/build-tools/$pinned" ]; then
    build_tools="$sdk_root/build-tools/$pinned"
else
    # Highest version present. `sort -V` so 36.0.0 beats 9.0.0.
    for candidate in $(ls "$sdk_root/build-tools" 2>/dev/null | sort -V -r); do
        if [ -x "$sdk_root/build-tools/$candidate/apksigner" ]; then
            build_tools="$sdk_root/build-tools/$candidate"
            break
        fi
    done
    if [ -n "$pinned" ] && [ -n "$build_tools" ]; then
        echo "note: pinned build-tools '$pinned' is absent; using $build_tools" >&2
    fi
fi

if [ -z "$build_tools" ]; then
    echo "no usable build-tools (looked for apksigner under $sdk_root/build-tools)" >&2
    ls -la "$sdk_root/build-tools" >&2 || true
    exit 1
fi

apksigner="$build_tools/apksigner"
zipalign="$build_tools/zipalign"
for tool in "$apksigner" "$zipalign"; do
    if [ ! -x "$tool" ]; then
        echo "$tool is missing or not executable" >&2
        exit 1
    fi
done
if ! command -v jarsigner >/dev/null 2>&1; then
    echo "jarsigner is not on PATH (a JDK must be set up before signing)" >&2
    exit 1
fi

echo "signing with $(basename "$build_tools") build-tools, min SDK $min_sdk"

for file in "$@"; do
    if [ ! -f "$file" ]; then
        echo "'$file' does not exist" >&2
        exit 1
    fi

    case "$file" in
    *.apk)
        # Align BEFORE signing: zipalign rewrites offsets, which would
        # invalidate a signature, and apksigner refuses to align afterwards.
        # -p aligns .so files to the page size so the loader can mmap them
        # straight out of the APK, which is what Qt's libraries want.
        aligned="$file.aligned"
        "$zipalign" -p -f 4 "$file" "$aligned"
        "$zipalign" -c -p 4 "$aligned"
        mv "$aligned" "$file"

        # --min-sdk-version drives which schemes are emitted. At 28 apksigner
        # writes v1+v2+v3; letting it read minSdk from the manifest would work
        # too, but stating it means a manifest change cannot silently drop v2
        # and produce a package that API 30+ devices refuse.
        CH_KEYSTORE_PASSWORD="$CH_KEYSTORE_PASSWORD" \
        CH_KEY_PASSWORD="$key_password" \
        "$apksigner" sign \
            --ks "$keystore" \
            --ks-key-alias "$alias" \
            --ks-pass env:CH_KEYSTORE_PASSWORD \
            --key-pass env:CH_KEY_PASSWORD \
            --min-sdk-version "$min_sdk" \
            "$file"

        # Verification is the point of the exercise. apksigner exits non-zero on
        # a bad signature, but it also exits ZERO while printing warnings, and
        # "signed, but not with a scheme this device requires" is precisely the
        # failure that shipped an uninstallable APK before. So assert the v2
        # line explicitly rather than trusting the exit status.
        report=$("$apksigner" verify --min-sdk-version "$min_sdk" --verbose --print-certs "$file")
        echo "$report"
        for scheme in "Verified using v1 scheme (JAR signing): true" \
                      "Verified using v2 scheme (APK Signature Scheme v2): true"; do
            if ! printf '%s\n' "$report" | grep -qF "$scheme"; then
                echo "$(basename "$file"): '$scheme' is absent from the verify" >&2
                echo "report above, so this package is not installable." >&2
                exit 1
            fi
        done
        ;;
    *.aab)
        CH_KEY_PASSWORD="$key_password" \
        jarsigner -verbose:summary \
            -sigalg SHA256withRSA -digestalg SHA-256 \
            -keystore "$keystore" \
            -storepass:env CH_KEYSTORE_PASSWORD \
            -keypass:env CH_KEY_PASSWORD \
            "$file" "$alias" >/dev/null
        # NOT `-strict`. Under -strict, jarsigner exits with the OR of its
        # warning codes, and code 4 is "this JAR contains entries whose signer
        # certificate is self signed" (JDK 17 jarsigner spec). An Android upload
        # key IS self-signed - Play pins the certificate itself, there is no CA
        # chain to validate - so -strict would fail every legitimate release.
        #
        # But the plain exit status is not trustworthy on its own either:
        # JDK-8031572 records `jarsigner -verify` exiting 0 for a jar that is
        # not properly signed. So assert the output text, and additionally check
        # structurally that signature entries now exist - the same belt-and-
        # braces as the apksigner branch above, for the same reason.
        report=$(jarsigner -verify "$file")
        if ! printf '%s\n' "$report" | grep -qF 'jar verified.'; then
            printf '%s\n' "$report" >&2
            echo "$(basename "$file"): jarsigner did not report 'jar verified.'" >&2
            exit 1
        fi
        if ! unzip -l "$file" | grep -qiE 'META-INF/.*\.(RSA|DSA|EC)$'; then
            echo "$(basename "$file"): no META-INF signature block after" >&2
            echo "signing, so the bundle is not signed." >&2
            exit 1
        fi
        echo "$(basename "$file"): jar signature verified (self-signed key, as"
        echo "an Android upload key always is)"
        ;;
    *)
        echo "'$file' is neither .apk nor .aab" >&2
        exit 1
        ;;
    esac
done

echo "all packages signed and verified"
