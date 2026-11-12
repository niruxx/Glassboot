#!/usr/bin/env bash
# Build a pinned upstream type-2 runtime, statically linked with distro dev libs.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/packaging/versions.env"
OUT="${GLASSBOOT_OUT_DIR:-$ROOT/out}/appimage"
work=$(mktemp -d /var/tmp/glassboot-runtime.XXXXXX)
trap 'rm -rf "$work"' EXIT
mkdir -p "$OUT"
git init -q "$work/runtime"
git -C "$work/runtime" fetch -q --depth=1 https://github.com/AppImage/type2-runtime.git "$APPIMAGE_RUNTIME_COMMIT"
git -C "$work/runtime" checkout -q --detach FETCH_HEAD
[[ $(git -C "$work/runtime" rev-parse HEAD) == "$APPIMAGE_RUNTIME_COMMIT" ]]
curl -fL --retry 2 -o "$work/squashfuse.tar.gz" "https://codeload.github.com/vasi/squashfuse/tar.gz/refs/tags/$SQUASHFUSE_VERSION"
echo "$SQUASHFUSE_SHA256  $work/squashfuse.tar.gz" | sha256sum -c -
tar -C "$work" -xf "$work/squashfuse.tar.gz"
prefix="$work/deps"
(
 cd "$work/squashfuse-$SQUASHFUSE_VERSION"
 ./autogen.sh
 ./configure --prefix="$prefix" --disable-shared --enable-static --with-zstd=/usr --with-zlib=/usr --without-xz --without-lzo --without-lz4
 make -j"${JOBS:-4}"
 make install
 mkdir -p "$prefix/include/squashfuse"
 cp ./*.h "$prefix/include/squashfuse/"
)
(
 cd "$work/runtime/src/runtime"
 # No optional mimalloc override: use the statically linked C library allocator.
 gcc -std=gnu99 -Os -D_FILE_OFFSET_BITS=64 "-DGIT_COMMIT=\"$APPIMAGE_RUNTIME_COMMIT\"" \
   -T data_sections.ld -ffunction-sections -fdata-sections -Wl,--gc-sections -static-pie \
   -I"$prefix/include" -I/usr/include/fuse3 runtime.c -L"$prefix/lib" \
   -lsquashfuse_ll -lsquashfuse -lfuse3 -lzstd -lz -lpthread -lm -ldl -o "$OUT/runtime-x86_64"
)
strip "$OUT/runtime-x86_64"
printf 'AI\002' | dd of="$OUT/runtime-x86_64" bs=1 seek=8 conv=notrunc status=none
cp "$work/runtime/LICENSE" "$OUT/LICENSE.runtime" 2>/dev/null || cp "$work/runtime/src/runtime/runtime.c" "$OUT/LICENSE.runtime-source"
cp "$work/squashfuse-$SQUASHFUSE_VERSION/LICENSE" "$OUT/LICENSE.squashfuse"
"$OUT/runtime-x86_64" --appimage-version
(cd "$OUT"; sha256sum runtime-x86_64 > runtime.sha256)
