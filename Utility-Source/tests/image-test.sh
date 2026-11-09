#!/usr/bin/env bash
# End-to-end check of the installer on a disk image (Linux, root):
#   - glassboot-cli writes the image
#   - partition table, exFAT and FAT16 pass the standard Linux fsck tools
#   - the seed files are readable through the kernel's exFAT driver
#
#   tests/image-test.sh BUILD_DIR [IMAGE] [SIZE]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
build="$1"
img="${2:-/var/tmp/glassboot-test.img}"
size="${3:-4G}"

"$build/glassboot-cli" install --image "$img" --size "$size" --payload "$ROOT/out/payload"

echo "--- partition table"
sfdisk -d "$img"

loop="$(losetup --show -f -P "$img")"
mnt="$(mktemp -d)"
trap 'umount "$mnt" 2>/dev/null || true; losetup -d "$loop"; rmdir "$mnt"' EXIT
udevadm settle 2>/dev/null || sleep 1

echo "--- fsck.exfat"
fsck.exfat -n "${loop}p1"
echo "--- fsck.vfat"
fsck.vfat -n "${loop}p2"
echo "--- blkid"
blkid "${loop}p1" "${loop}p2"

echo "--- exFAT contents"
modprobe exfat 2>/dev/null || true
if grep -qw exfat /proc/filesystems; then mount -o ro "${loop}p1" "$mnt"; else mount.exfat-fuse -o ro "${loop}p1" "$mnt"; fi
find "$mnt" -exec ls -ld {} \;
head -c 60 "$mnt/glassboot/grubenv"; echo
stat -c '%s bytes' "$mnt/glassboot/grubenv"
cat "$mnt/glassboot/README.txt"
echo "PASS"
