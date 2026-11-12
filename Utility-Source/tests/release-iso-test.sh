#!/usr/bin/env bash
# Reproducible ISO matrix fixture. Only an Alpine live boot, no installation targets.
set -euo pipefail
cd "$(dirname "$0")/../.."
build="$(realpath "$1")"
cache="${GLASSBOOT_ISO_CACHE:-/var/tmp/glassboot-iso-cache}"
mkdir -p "$cache" out/validation
iso="$cache/alpine-standard-3.24.4-x86_64.iso"
sha=92e3e4e4c6b8e265a889278de2a633c0f1456ca52820952666ecb395c3d23582
if [[ ! -f "$iso" ]]; then
 curl -fL --retry 2 -o "$iso.part" https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64/alpine-standard-3.24.4-x86_64.iso
 mv "$iso.part" "$iso"
fi
echo "$sha  $iso" | sha256sum -c -
work=$(mktemp -d /var/tmp/glassboot-release-iso.XXXXXX)
loop=""; mounted=0
cleanup() {
 [[ "$mounted" == 0 ]] || umount "$work/mnt"
 [[ -z "$loop" ]] || losetup -d "$loop"
 rm -rf "$work"
}
trap cleanup EXIT
mkdir "$work/mnt"
"$build/glassboot-cli" install --image "$work/test.img" --size 1G --payload out/payload > "$work/install.log"
loop=$(losetup --find --show --partscan "$work/test.img")
mount "${loop}p1" "$work/mnt"; mounted=1
cp "$iso" "$work/mnt/Alpine.iso"
echo 'args /Alpine.iso = console=ttyS0,115200' > "$work/mnt/glassboot/glassboot.cfg"
umount "$work/mnt"; mounted=0; losetup -d "$loop"; loop=""
for fw in bios uefi; do
 QEMU_MEM=1024 QEMU_SERIAL="$PWD/out/validation/release-alpine-$fw.log" \
   bash scripts/qemu-script.sh "$fw" "$work/test.img" wait:12 key:ret wait:55 "shot:$PWD/out/validation/release-alpine-$fw.png"
 grep -Eq 'Welcome to Alpine Linux|localhost login:' "out/validation/release-alpine-$fw.log"
 echo "PASS: pinned Alpine live ISO reached userspace ($fw)"
done
