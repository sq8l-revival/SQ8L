#!/bin/sh
# Package built plug-ins into a release archive.
#   tools/package.sh <build dir> <platform: macos|windows|linux> <output .zip or .tar.gz>
set -e
BUILD=$1; PLATFORM=$2; OUT=$3
case $OUT in /*) ;; *) OUT="$PWD/$OUT" ;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAGE=$(mktemp -d)/SQ8L
mkdir -p "$STAGE"
if [ "$PLATFORM" = macos ]; then
    cp -R "$BUILD/bin/SQ8L.vst" "$BUILD/bin/SQ8L.vst3" "$BUILD/bin/SQ8L.component" "$BUILD/bin/SQ8L.clap" "$STAGE/"
    # Ad-hoc signature over the whole bundle (the linker only signs the binary). Not a
    # Developer ID: downloaded copies still need the quarantine flag removed.
    for b in SQ8L.vst SQ8L.vst3 SQ8L.component SQ8L.clap; do
        codesign --force --sign - --timestamp=none "$STAGE/$b"
        codesign --verify --strict "$STAGE/$b"
    done
elif [ "$PLATFORM" = linux ]; then
    cp -R "$BUILD/bin/SQ8L.so" "$BUILD/bin/SQ8L.vst3" "$BUILD/bin/SQ8L.clap" "$BUILD/bin/SQ8L.lv2" "$STAGE/"
    ${STRIP:-strip} "$STAGE/SQ8L.so" "$STAGE"/SQ8L.vst3/Contents/*-linux/SQ8L.so "$STAGE/SQ8L.clap" \
        "$STAGE/SQ8L.lv2/SQ8L_dsp.so" "$STAGE/SQ8L.lv2/SQ8L_ui.so"
else
    cp -R "$BUILD/bin/SQ8L.dll" "$BUILD/bin/SQ8L.vst3" "$BUILD/bin/SQ8L.clap" "$STAGE/"
    ${STRIP:-x86_64-w64-mingw32-strip} "$STAGE/SQ8L.dll" "$STAGE/SQ8L.vst3/Contents/x86_64-win/SQ8L.vst3" "$STAGE/SQ8L.clap"
fi
cp "$ROOT/README.md" "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/LICENSE.md" "$STAGE/"
case $OUT in
    *.tar.gz) (cd "$(dirname "$STAGE")" && tar czf "$OUT" SQ8L) ;;
    *) (cd "$(dirname "$STAGE")" && zip -qr -X "$OUT" SQ8L) ;;
esac
echo "wrote $OUT"
