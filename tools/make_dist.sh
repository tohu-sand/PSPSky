#!/bin/sh
# Package the built EBOOT.PBP for binary distribution.
# Usage: sh tools/make_dist.sh
#
# HEAD must be a release tag without uncommitted changes, so that the tag's
# source archive is the corresponding source of the packaged binary.
set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$REPO_ROOT/dist"

die() {
    echo "error: $*" >&2
    exit 1
}

cd "$REPO_ROOT"

TAG=$(git describe --tags --exact-match HEAD 2>/dev/null) ||
    die "HEAD is not tagged; tag the release commit first"
git diff --quiet HEAD -- || die "tracked files have uncommitted changes"

[ -f EBOOT.PBP ] || die "EBOOT.PBP not found; build it with make first"
# The Makefile does not track header dependencies, so a stale EBOOT.PBP can
# survive make; compare timestamps against every tracked build input.
stale=$(git ls-files -z -- src include vendor Makefile ICON0.PNG PIC1.PNG |
        xargs -0 sh -c 'find "$@" -prune -newer EBOOT.PBP -print' sh |
        head -n 1)
[ -z "$stale" ] || die "EBOOT.PBP is older than $stale; run make clean && make"

PSPDEV=${PSPDEV:-$(psp-config --pspdev-path 2>/dev/null || true)}
[ -r "$PSPDEV/build.txt" ] || die "set PSPDEV to the pspdev SDK used for the build"
PTE_COMMIT=$(awk '$1 == "pthread-embedded" { print $2; exit }' "$PSPDEV/build.txt")
[ -n "$PTE_COMMIT" ] || die "pthread-embedded revision not found in $PSPDEV/build.txt"

command -v zip >/dev/null 2>&1 || die "zip not found"
command -v curl >/dev/null 2>&1 || die "curl not found"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

NAME="PSPSky-$TAG"
STAGE="$WORK/$NAME"
mkdir -p "$STAGE/PSP/GAME/PSPSKY"
cp EBOOT.PBP assets/cacert.pem "$STAGE/PSP/GAME/PSPSKY/"
cp LICENSE THIRD_PARTY_LICENSES.md README.md README.en.md "$STAGE/"
cp -R licenses "$STAGE/"

mkdir -p "$OUT"
ZIP="$OUT/$NAME.zip"
rm -f "$ZIP"
(cd "$WORK" && zip -qrX "$ZIP" "$NAME")

# pthread-embedded is LGPL and statically linked; LGPL-2.1 section 6 requires
# its source to be offered from the same place as the binary.
PTE_TGZ="$OUT/pthread-embedded-$(printf '%.7s' "$PTE_COMMIT").tar.gz"
curl -fsSL --retry 3 --connect-timeout 20 -o "$PTE_TGZ" \
    "https://github.com/pspdev/pthread-embedded/archive/$PTE_COMMIT.tar.gz"

echo "wrote $ZIP"
echo "wrote $PTE_TGZ"
echo "Attach both files to the $TAG release."
