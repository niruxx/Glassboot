#!/usr/bin/env bash
# Install everything needed to build and test Glassboot on Debian/Ubuntu (incl. WSL2).
# All packages come from the distro archive; nothing is downloaded from third parties here.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  exec sudo "$0" "$@"
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential gcc-multilib bison flex gettext autoconf automake autoconf-archive autopoint pkg-config \
  python3 libfreetype-dev unifont fonts-unifont liblzma-dev libdevmapper-dev \
  binutils-dev zlib1g-dev libiberty-dev \
  git curl ca-certificates xz-utils gpg gpgv \
  util-linux fdisk dosfstools exfatprogs exfat-fuse kmod mtools xorriso cpio linux-libc-dev \
  qemu-system-x86 qemu-utils ovmf \
  cmake ninja-build qt6-base-dev qt6-base-dev-tools qt6-l10n-tools qt6-tools-dev mingw-w64 \
  nasm uuid-dev wimtools
