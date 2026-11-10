#!/usr/bin/env bash
# Linux/root integration checks. Only temporary image files are modified.
# Usage: tests/platform-image-test.sh BUILD_DIR [PAYLOAD_DIR]
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="$1"; payload="${2:-$root/out/payload}"
work="$(mktemp -d /tmp/glassboot-platform-test.XXXXXX)"
loop=""; mounted=0
cleanup() {
  if [[ "$mounted" == 1 ]]; then umount "$work/mnt"; fi
  if [[ -n "$loop" ]]; then losetup -d "$loop"; fi
  rm -f "$work/"*.img
  rmdir "$work/mnt" "$work"
}
trap cleanup EXIT
mkdir "$work/mnt"
gcc -std=c11 -O2 -Wall -Wextra -Wno-format-truncation "$root/Utility-Source/tests/hook-test.c" -o "$build/glassboot-hook-test"
gcc -std=c11 -O2 -Wall -Wextra -Wno-format-truncation "$root/hook/glassboot-hook.c" -o "$build/glassboot-hook-testmode"
for kind in mbr gpt large; do
  args=(); size=1G
  [[ "$kind" != mbr ]] && args+=(--gpt)
  [[ "$kind" == large ]] && size=3T
  img="$work/$kind.img"
  "$build/glassboot-cli" install --image "$img" --size "$size" --reserve 128M --payload "$payload" "${args[@]}"
  if [[ "$kind" != mbr ]]; then
    sgdisk --verify "$img"
    "$build/glassboot-hook-test" "$img"
  else
    "$build/glassboot-hook-test"
  fi
  "$build/glassboot-hook-testmode" --test "$img" 1 /glassboot/README.txt
  loop="$(losetup --show -f -P "$img")"
  fsck.exfat -n "${loop}p1"
  fsck.vfat -n "${loop}p2"
  mount "${loop}p1" "$work/mnt"; mounted=1
  printf 'Preserve these ISO bytes during update.\n' > "$work/mnt/preserve.iso"
  before="$(sha256sum "$work/mnt/preserve.iso" | cut -d' ' -f1)"
  umount "$work/mnt"; mounted=0
  losetup -d "$loop"; loop=""
  if [[ "$kind" != mbr ]]; then
    sgdisk --new=3:0:+16M --change-name=3:EXTRA "$img"
  fi
  table_before="$(sfdisk --dump "$img")"
  "$build/glassboot-cli" update --image "$img" --payload "$payload"
  [[ "$table_before" == "$(sfdisk --dump "$img")" ]]
  loop="$(losetup --show -f -P "$img")"
  mount -o ro "${loop}p1" "$work/mnt"; mounted=1
  [[ "$before" == "$(sha256sum "$work/mnt/preserve.iso" | cut -d' ' -f1)" ]]
  umount "$work/mnt"; mounted=0
  losetup -d "$loop"; loop=""
  [[ "$kind" == mbr ]] || sgdisk --verify "$img"
  rm "$img"
done
echo 'PASS: MBR, GPT, 3 TiB, hook matching, filesystem checks and updates'
