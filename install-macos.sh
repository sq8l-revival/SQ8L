#!/bin/bash
# Install staged SQ8L bundles into the user's macOS plugin folders.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SOURCE="$ROOT/dist/macos"
DEST="${HOME}/Library/Audio/Plug-Ins"
DRY_RUN=false

usage() {
    echo "Usage: $0 [--dry-run] [--source DIRECTORY] [--dest DIRECTORY]"
    echo 'Defaults: ./dist/macos -> ~/Library/Audio/Plug-Ins'
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=true; shift ;;
        --source|--dest)
            [[ $# -ge 2 && -n "$2" ]] || { usage >&2; exit 2; }
            if [[ "$1" == --source ]]; then SOURCE="$2"; else DEST="$2"; fi
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
[[ $(uname -s) == Darwin ]] || { echo 'This script requires macOS.' >&2; exit 1; }

# Check every bundle before copying any of them.
for bundle in SQ8L.component SQ8L.vst SQ8L.vst3 SQ8L.clap; do
    [[ -d "$SOURCE/$bundle/Contents" ]] || {
        echo "Missing bundle: $SOURCE/$bundle. Run build-macos.sh first." >&2
        exit 1
    }
    codesign --verify --strict "$SOURCE/$bundle"
done
SOURCE=$(cd "$SOURCE" && pwd -P)
for mapping in 'SQ8L.component:Components' 'SQ8L.vst:VST' 'SQ8L.vst3:VST3' 'SQ8L.clap:CLAP'; do
    bundle=${mapping%%:*}
    folder=${mapping#*:}
    target="$DEST/$folder/$bundle"
    [[ ! -L "$target" ]] || { echo "Refusing symlink destination: $target" >&2; exit 1; }
    echo "$SOURCE/$bundle -> $target"
    if ! $DRY_RUN; then
        mkdir -p "$DEST/$folder"
        # --delete removes stale files only inside this specific SQ8L bundle.
        rsync -a --delete "$SOURCE/$bundle/" "$target/"
        codesign --verify --strict "$target"
    fi
done
if $DRY_RUN; then
    echo 'Dry run: no files copied.'
else
    echo 'Installed SQ8L. Restart your DAW and rescan plugins if needed.'
fi
