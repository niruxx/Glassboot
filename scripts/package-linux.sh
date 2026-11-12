#!/usr/bin/env bash
# Build a self-contained Linux x86_64 AppImage into dist/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1704067200}" TZ=UTC LC_ALL=C
BUILD="${GLASSBOOT_LINUX_BUILD_DIR:-/var/tmp/glassboot-release-linux}"
OUT="${GLASSBOOT_OUT_DIR:-$ROOT/out}"
DIST="$ROOT/dist"
version=$(python3 scripts/release.py version)
runtime="$OUT/appimage/runtime-x86_64"
if [[ ! -x "$runtime" ]]; then bash scripts/build-appimage-runtime.sh; fi
(cd "$OUT/appimage"; sha256sum -c runtime.sha256)
cmake -S Utility-Source -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"${JOBS:-4}"
ctest --test-dir "$BUILD" --output-on-failure
work=$(mktemp -d /var/tmp/glassboot-appdir.XXXXXX)
trap 'rm -rf "$work"' EXIT
python3 scripts/release.py linux-stage "$BUILD" "$OUT/payload" "$work/AppDir"
mkdir -p "$work/AppDir/LICENSES"
cp "$OUT/appimage/"LICENSE* "$work/AppDir/LICENSES/"
cp packaging/versions.env "$work/AppDir/packaging-versions.env"
python3 scripts/release.py inventory "$work/AppDir" linux-x86_64
python3 scripts/release.py verify "$work/AppDir"
mkdir -p "$DIST"
env -u SOURCE_DATE_EPOCH mksquashfs "$work/AppDir" "$work/app.squashfs" -noappend -all-root -comp zstd \
  -processors 1 -mkfs-time "$SOURCE_DATE_EPOCH" -all-time "$SOURCE_DATE_EPOCH" > "$work/squashfs.log"
name="Glassboot-$version-linux-x86_64.AppImage"
cat "$runtime" "$work/app.squashfs" > "$DIST/$name.tmp"
chmod 755 "$DIST/$name.tmp"
mv "$DIST/$name.tmp" "$DIST/$name"
cp "$work/AppDir/SBOM.cdx.json" "$DIST/$name.sbom.json"
cp "$work/AppDir/release.json" "$DIST/$name.provenance.json"
python3 scripts/release.py hashes "$DIST"
echo "Built $DIST/$name"
