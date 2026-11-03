#!/bin/bash
# Build universal macOS plugins and stage signed bundles outside the source repo.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SOURCE="$ROOT"
BUILD="$SOURCE/build/macos-release"
DIST="$ROOT/dist/macos"

[[ $(uname -s) == Darwin ]] || { echo 'This script requires macOS.' >&2; exit 1; }
git -C "$SOURCE" submodule update --init --recursive
cmake -S "$SOURCE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15
cmake --build "$BUILD" --parallel "${BUILD_JOBS:-$(sysctl -n hw.logicalcpu)}"

mkdir -p "$DIST"
STAGE=$(mktemp -d "$DIST/.stage.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
for bundle in SQ8L.component SQ8L.vst SQ8L.vst3 SQ8L.clap; do
    ditto "$BUILD/bin/$bundle" "$STAGE/$bundle"
    codesign --force --sign - --timestamp=none "$STAGE/$bundle"
    codesign --verify --strict "$STAGE/$bundle"
done
for bundle in SQ8L.component SQ8L.vst SQ8L.vst3 SQ8L.clap; do
    rm -rf "$DIST/$bundle"
    mv "$STAGE/$bundle" "$DIST/$bundle"
done
echo "Built and signed universal plugins: $DIST"
