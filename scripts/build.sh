#!/usr/bin/env bash
# Build all BIOS-USB boot components from pinned upstream source.
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
BUILD="${BIOSUSB_BUILD_DIR:-/var/tmp/biosusb-build}"
OUT="${BIOSUSB_OUT_DIR:-$ROOT/out}"
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

case "${1:-all}" in
  grub)    build_grub ;;
  wimboot) build_wimboot ;;
  all)     build_grub; build_wimboot ;;
  *) echo "usage: $0 [grub|wimboot|all]" >&2; exit 2 ;;
esac

log "Done. Outputs in $OUT"
