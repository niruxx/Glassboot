#!/usr/bin/env bash
# Linux/root only. Formats temporary image files, never physical disks.
set -euo pipefail
cd "$(dirname "$0")/../.."
cli="$(realpath "$1")/glassboot-cli"
work="$(mktemp -d /var/tmp/glassboot-storage.XXXXXX)"
loop=""; mounted=0
mkdir "$work/mnt"; mkdir -p out/validation
cleanup() {
  [[ "$mounted" == 0 ]] || umount "$work/mnt"
  [[ -z "$loop" ]] || losetup -d "$loop"
  rm -rf "$work"
}
trap cleanup EXIT
for fs in ${GLASSBOOT_TEST_FS:-exfat ntfs ext4 xfs udf}; do
  "$cli" install --image "$work/$fs.img" --size 1G --filesystem "$fs" --payload out/payload > "out/validation/format-$fs.log"
  loop=$(losetup --find --show --partscan "$work/$fs.img")
  mount -t "${fs/ntfs/ntfs-3g}" "${loop}p1" "$work/mnt"; mounted=1
  [[ $(stat -c %s "$work/mnt/glassboot/grubenv") == 1024 ]]
  grep -q Glassboot "$work/mnt/glassboot/README.txt"
  umount "$work/mnt"; mounted=0
  fsck.fat -n "${loop}p2" > "out/validation/esp-$fs.log"
  losetup -d "$loop"; loop=""
  "$cli" update --image "$work/$fs.img" --payload out/payload >> "out/validation/format-$fs.log"
  echo "PASS: $fs install, seed files, ESP check, update"
done

# NTFS shrink-and-add: no free tail initially, user file survives relocation/resize.
truncate -s 1G "$work/shrink.img"
printf 'label: dos\nstart=2048,type=7\n' | sfdisk "$work/shrink.img" >/dev/null
loop=$(losetup --find --show --partscan "$work/shrink.img")
mkfs.ntfs -F -Q "${loop}p1" >/dev/null 2>&1
mount -t ntfs-3g "${loop}p1" "$work/mnt"; mounted=1
printf 'Must survive NTFS shrink-and-add\n' > "$work/mnt/keep.txt"
before=$(sha256sum "$work/mnt/keep.txt" | cut -d' ' -f1)
umount "$work/mnt"; mounted=0; losetup -d "$loop"; loop=""
"$cli" install --image "$work/shrink.img" --shrink-ntfs --payload out/payload > out/validation/ntfs-shrink.log
loop=$(losetup --find --show --partscan "$work/shrink.img")
mount -t ntfs-3g -o ro "${loop}p1" "$work/mnt"; mounted=1
after=$(sha256sum "$work/mnt/keep.txt" | cut -d' ' -f1)
[[ "$before" == "$after" ]]
umount "$work/mnt"; mounted=0
fsck.fat -n "${loop}p2" >/dev/null
losetup -d "$loop"; loop=""
echo 'PASS: NTFS shrink-and-add, file checksum preserved, valid ESP'

for size in 1G 3T; do
"$cli" install --image "$work/4kn.img" --size "$size" --gpt --sector-size 4096 --payload out/payload > "out/validation/4kn-$size-install.log"
loop=$(losetup --find --show --sector-size 4096 --partscan "$work/4kn.img")
[[ $(blockdev --getss "$loop") == 4096 ]]
fsck.exfat -n "${loop}p1" > "out/validation/4kn-$size-exfat.log"
fsck.fat -n "${loop}p2" > "out/validation/4kn-$size-esp.log"
mount "${loop}p1" "$work/mnt"; mounted=1
[[ $(stat -c %s "$work/mnt/glassboot/grubenv") == 1024 ]]
printf '4Kn hook fixture' > "$work/mnt/test.iso"
umount "$work/mnt"; mounted=0
losetup -d "$loop"; loop=""
gcc -O2 -static -Wno-format-truncation -o "$work/hook" hook/glassboot-hook.c
"$work/hook" --test "$work/4kn.img" 1 /test.iso > "out/validation/4kn-$size-hook.log"
"$cli" update --image "$work/4kn.img" --sector-size 4096 --payload out/payload >> "out/validation/4kn-$size-install.log"
echo "PASS: $size 4Kn GPT install, both filesystem checks, hook lookup, update"
done

# Preserve mode: hash every byte of the existing data partition before and after.
for fs in exfat ntfs; do
  truncate -s 1G "$work/preserve-$fs.img"
  printf 'label: dos\nstart=2048,size=1400000,type=7\n' | sfdisk "$work/preserve-$fs.img" >/dev/null
  loop=$(losetup --find --show --partscan "$work/preserve-$fs.img")
  if [[ "$fs" == ntfs ]]; then mkfs.ntfs -F -Q "${loop}p1" >/dev/null 2>&1
  else mkfs.exfat "${loop}p1" >/dev/null; fi
  mount -t "${fs/ntfs/ntfs-3g}" "${loop}p1" "$work/mnt"; mounted=1
  printf 'Existing user file; preserve all bytes' > "$work/mnt/keep.txt"
  umount "$work/mnt"; mounted=0
  before=$(sha256sum "${loop}p1" | cut -d' ' -f1)
  losetup -d "$loop"; loop=""
  "$cli" install --preserve-data --image "$work/preserve-$fs.img" --payload out/payload > "out/validation/preserve-$fs.log"
  loop=$(losetup --find --show --partscan "$work/preserve-$fs.img")
  after=$(sha256sum "${loop}p1" | cut -d' ' -f1)
  [[ "$before" == "$after" ]]
  fsck.fat -n "${loop}p2" > /dev/null
  losetup -d "$loop"; loop=""
  "$cli" update --image "$work/preserve-$fs.img" --payload out/payload >> "out/validation/preserve-$fs.log"
  echo "PASS: preserve $fs (whole partition SHA256 unchanged), update"
done

# An unsupported preservation request must leave the entire image untouched.
before=$(sha256sum "$work/preserve-ntfs.img" | cut -d' ' -f1)
if "$cli" install --preserve-data --gpt --image "$work/preserve-ntfs.img" --payload out/payload > "$work/rejected.log" 2>&1; then
  echo 'unexpected acceptance of incompatible preservation options' >&2; exit 1
fi
after=$(sha256sum "$work/preserve-ntfs.img" | cut -d' ' -f1)
[[ "$before" == "$after" ]]
echo 'PASS: rejected preservation request performed no writes'
