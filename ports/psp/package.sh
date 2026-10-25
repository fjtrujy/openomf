#!/bin/sh
# Stage a ready-to-copy PSP/GAME/OpenOMF directory.
# Original One Must Fall 2097 freeware assets are downloaded separately and
# intentionally never added to the git repository.
set -eu

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$PORT_DIR/../.." && pwd)
BUILD_DIR=${1:-"$ROOT/build/psp"}
ZIP=${OMF_ASSET_ARCHIVE:-"$BUILD_DIR/openomf-assets.zip"}
DST="$BUILD_DIR/PSP/GAME/OpenOMF"

if [ ! -f "$BUILD_DIR/EBOOT.PBP" ] || [ ! -f "$BUILD_DIR/resources/ENGLISH.DAT2" ]; then
    echo "Build the PSP target first: psp-cmake -S ports/psp -B build/psp && cmake --build build/psp" >&2
    exit 1
fi

if [ ! -f "$ZIP" ]; then
    echo "Fetching freeware One Must Fall 2097 game resources from omf2097.com"
    curl -fL --retry 2 -o "$ZIP" \
      'https://www.omf2097.com/pub/files/omf/openomf-assets.zip'
fi

mkdir -p "$DST/resources" "$DST/save"
unzip -jo "$ZIP" 'OMF2097/*' -d "$DST/resources" >/dev/null
cp "$BUILD_DIR/EBOOT.PBP" "$DST/EBOOT.PBP"
cp "$BUILD_DIR/resources/openomf.bk" "$DST/resources/openomf.bk"
cp "$BUILD_DIR/resources/ENGLISH.DAT2" "$DST/resources/ENGLISH.DAT2"

for file in MAIN.BK INTRO.BK SOUNDS.DAT ENGLISH.DAT; do
    if [ ! -s "$DST/resources/$file" ]; then
        echo "Missing required game resource: $file" >&2
        exit 1
    fi
done
echo "PSP game staged: $DST"
du -sh "$DST" || true
