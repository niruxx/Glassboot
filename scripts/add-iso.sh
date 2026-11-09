#!/usr/bin/env bash
# Copy ISO files onto the data partition of a Glassboot disk image (for QEMU testing).
# On a real USB stick you don't need this: just drag the ISOs onto the GLASSBOOT drive.
#
#   scripts/add-iso.sh IMAGE file.iso [more.iso ...]
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 1; }
img="$1"; shift

loop="$(losetup --show -f -P "$img")"
mnt="$(mktemp -d)"
trap 'umount "$mnt" 2>/dev/null; losetup -d "$loop"; rmdir "$mnt"' EXIT

modprobe exfat 2>/dev/null || true
if grep -qw exfat /proc/filesystems; then mount "${loop}p1" "$mnt"; else mount.exfat-fuse "${loop}p1" "$mnt"; fi
for iso in "$@"; do
  echo "copying $(basename "$iso")"
  cp "$iso" "$mnt/"
done
sync
df -h "$mnt" | tail -1
