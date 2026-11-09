#!/usr/bin/env bash
# Build all Glassboot boot components from pinned upstream source.
#
#   out/grub/      GRUB tools + modules for i386-pc (BIOS), x86_64-efi and i386-efi
#   out/wimboot/   wimboot (BIOS + x86_64 UEFI) and wimboot.i386.efi
#
# Usage: scripts/build.sh [grub|wimboot|all]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../versions.env
source "$ROOT/versions.env"

# Build on the Linux filesystem: building on /mnt/c (WSL drvfs) is slow and breaks symlinks.
BUILD="${GLASSBOOT_BUILD_DIR:-/var/tmp/glassboot-build}"
OUT="${GLASSBOOT_OUT_DIR:-$ROOT/out}"
JOBS="$(nproc)"

log() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }

build_grub() {
  local src="$BUILD/grub-$GRUB_VERSION" tar="$BUILD/grub-$GRUB_VERSION.tar.xz"
  mkdir -p "$BUILD"

  if [[ ! -f "$tar" ]]; then
    log "Downloading GRUB $GRUB_VERSION"
    curl -fL --proto '=https' -o "$tar.part" "https://ftp.gnu.org/gnu/grub/grub-$GRUB_VERSION.tar.xz"
    mv "$tar.part" "$tar"
  fi
  curl -fsL --proto '=https' -o "$tar.sig" "https://ftp.gnu.org/gnu/grub/grub-$GRUB_VERSION.tar.xz.sig"
  curl -fsL --proto '=https' -o "$BUILD/gnu-keyring.gpg" "https://ftp.gnu.org/gnu/gnu-keyring.gpg"

  log "Verifying GRUB source"
  echo "$GRUB_SHA256  $tar" | sha256sum -c -
  gpgv --keyring "$BUILD/gnu-keyring.gpg" "$tar.sig" "$tar"

  rm -rf "$src"
  tar -C "$BUILD" -xf "$tar"

  # Add the Glassboot module (boot/grub-module/glassboot.c) as an ordinary GRUB module,
  # then regenerate the build system so it is compiled for every platform.
  log "Adding the glassboot GRUB module"
  cp "$ROOT/boot/grub-module/glassboot.c" "$src/grub-core/commands/glassboot.c"
  cat >> "$src/grub-core/Makefile.core.def" <<'EOF'

module = {
  name = glassboot;
  common = commands/glassboot.c;
};
EOF
  (cd "$src" && ./autogen.sh >/dev/null)

  rm -rf "$OUT/grub"
  # target:platform pairs. All three install into the same prefix; grub-install then
  # finds each platform's modules under lib/grub/<target>-<platform>/.
  local spec target platform
  for spec in i386:pc x86_64:efi i386:efi; do
    target="${spec%%:*}" platform="${spec##*:}"
    log "Building GRUB for $target-$platform"
    mkdir -p "$src/build-$target-$platform"
    (
      cd "$src/build-$target-$platform"
      ../configure --quiet \
        --prefix="$OUT/grub" \
        --target="$target" --with-platform="$platform" \
        --disable-werror --disable-nls
      make -s -j"$JOBS"
      make -s install
    )
  done
}

build_wimboot() {
  local src="$BUILD/wimboot"
  log "Fetching wimboot $WIMBOOT_TAG"
  rm -rf "$src"
  git clone --quiet --depth 1 --branch "$WIMBOOT_TAG" https://github.com/ipxe/wimboot.git "$src"
  local got
  got="$(git -C "$src" rev-parse HEAD)"
  if [[ "$got" != "$WIMBOOT_COMMIT" ]]; then
    echo "wimboot commit mismatch: expected $WIMBOOT_COMMIT, got $got" >&2
    exit 1
  fi

  log "Building wimboot"
  # Only x86 targets: "all" also builds arm64, which needs a cross toolchain.
  make -s -C "$src/src" -j"$JOBS" wimboot wimboot.i386.efi
  rm -rf "$OUT/wimboot"
  mkdir -p "$OUT/wimboot"
  cp "$src/src/wimboot" "$OUT/wimboot/"
  [[ -f "$src/src/wimboot.i386.efi" ]] && cp "$src/src/wimboot.i386.efi" "$OUT/wimboot/"
  cp "$src/COPYING"* "$OUT/wimboot/" 2>/dev/null || true
}

# The initrd hook (hook/glassboot-hook.c): a static x86_64 binary packed as a newc cpio
# that GRUB appends to a distribution's initrd. Fixed mtimes keep the cpio reproducible.
build_hook() {
  log "Building initrd hook"
  local stage="$BUILD/hook-stage"
  rm -rf "$stage" "$OUT/hook"
  mkdir -p "$stage/glassboot/proc" "$stage/glassboot/sys" "$stage/glassboot/dev" "$OUT/hook"
  gcc -std=c11 -O2 -Wall -Wextra -Wno-format-truncation -static -s -o "$stage/glassboot/init" "$ROOT/hook/glassboot-hook.c"
  find "$stage" -exec touch -h -d @0 {} +
  (cd "$stage" && find glassboot | LC_ALL=C sort | cpio --quiet -o -H newc --reproducible --owner=0:0) \
    > "$OUT/hook/glassboot-hook-x86_64.cpio"
  ls -l "$OUT/hook/"
}

# Windows PE helper (win-helper/): attaches the chosen ISO as a virtual DVD so Windows
# Setup can find install.wim. Built for x64 and x86 WinPE with MinGW-w64.
build_winhelper() {
  log "Building Windows PE helper"
  rm -rf "$OUT/winhelper"
  mkdir -p "$OUT/winhelper"
  x86_64-w64-mingw32-gcc -municode -O2 -Wall -s -o "$OUT/winhelper/glassboot-attach.exe" \
    "$ROOT/win-helper/glassboot-attach.c" -lvirtdisk
  i686-w64-mingw32-gcc -municode -O2 -Wall -s -o "$OUT/winhelper/glassboot-attach32.exe" \
    "$ROOT/win-helper/glassboot-attach.c" -lvirtdisk
  ls -l "$OUT/winhelper/"
}

# memdisk (from syslinux): boots disk images and small ISOs from RAM on Legacy BIOS.
build_memdisk() {
  local tar="$BUILD/syslinux-$SYSLINUX_VERSION.tar.xz" src="$BUILD/syslinux-$SYSLINUX_VERSION"
  mkdir -p "$BUILD"
  if [[ ! -f "$tar" ]]; then
    log "Downloading syslinux $SYSLINUX_VERSION"
    curl -fL --proto '=https' -o "$tar.part" \
      "https://mirrors.edge.kernel.org/pub/linux/utils/boot/syslinux/syslinux-$SYSLINUX_VERSION.tar.xz"
    mv "$tar.part" "$tar"
  fi
  log "Verifying syslinux source"
  echo "$SYSLINUX_SHA256  $tar" | sha256sum -c -
  rm -rf "$src"
  tar -C "$BUILD" -xf "$tar"
  log "Building memdisk"
  make -s -C "$src" bios -j"$JOBS" > "$BUILD/syslinux-build.log" 2>&1 || true
  [[ -f "$src/bios/memdisk/memdisk" ]] || { tail -20 "$BUILD/syslinux-build.log"; exit 1; }
  rm -rf "$OUT/memdisk"
  mkdir -p "$OUT/memdisk"
  cp "$src/bios/memdisk/memdisk" "$src/COPYING" "$OUT/memdisk/"
  ls -l "$OUT/memdisk/"
}

case "${1:-all}" in
  grub)      build_grub ;;
  wimboot)   build_wimboot ;;
  hook)      build_hook ;;
  winhelper) build_winhelper ;;
  memdisk)   build_memdisk ;;
  all)       build_grub; build_wimboot; build_hook; build_winhelper; build_memdisk ;;
  *) echo "usage: $0 [grub|wimboot|hook|winhelper|memdisk|all]" >&2; exit 2 ;;
esac

log "Done. Outputs in $OUT"
