#!/usr/bin/env bash
# Boot the shipped ARM64 payload and a pinned ARM64 ISO, using only temporary disks.
set -euo pipefail
cd "$(dirname "$0")/../.."
build="$(realpath "${1:?usage: arm64-qemu-test.sh BUILD_DIR}")"
root="$PWD"
cache="${GLASSBOOT_ISO_CACHE:-/var/tmp/glassboot-iso-cache}"
mkdir -p "$cache" out/validation
iso="$cache/alpine-standard-3.24.4-aarch64.iso"
if [[ ! -f "$iso" ]]; then
  curl -fL --retry 2 -o "$iso.part" https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-standard-3.24.4-aarch64.iso
  mv "$iso.part" "$iso"
fi
echo "c0e2e9a8cb2f4848c5dc56ba26f58fbe4fdabdd35780b25a6c36f217ef67359f  $iso" | sha256sum -c -
work="$(mktemp -d /var/tmp/glassboot-arm64.XXXXXX)"
loop=""; mounted=0; pid=""
cleanup() {
  [[ -z "$pid" ]] || { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
  [[ "$mounted" == 0 ]] || umount "$work/mnt"
  [[ -z "$loop" ]] || losetup -d "$loop"
  rm -rf "$work"
}
trap cleanup EXIT
mkdir "$work/mnt"
"$build/glassboot-cli" install --image "$work/disk.img" --size 1G --gpt --payload out/payload > out/validation/arm64-install.log
loop="$(losetup --find --show --partscan "$work/disk.img")"
mount "${loop}p1" "$work/mnt"; mounted=1
cp "$iso" "$work/mnt/Alpine-arm64.iso"
printf 'timeout = 2\nargs /Alpine-arm64.iso = console=ttyAMA0,115200\n' > "$work/mnt/glassboot/glassboot.cfg"
umount "$work/mnt"; mounted=0
losetup -d "$loop"; loop=""
cp /usr/share/AAVMF/AAVMF_VARS.fd "$work/vars.fd"
: > out/validation/arm64-alpine.log
qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 1536 -smp 2 -display none -monitor none \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=none,id=stick,format=raw,snapshot=on,file=$work/disk.img" \
  -device virtio-blk-pci,drive=stick -serial "file:$root/out/validation/arm64-alpine.log" &
pid=$!
for ((i=0; i<150; i++)); do
  kill -0 "$pid" 2>/dev/null || { echo "QEMU exited unexpectedly"; exit 1; }
  if grep -Eq 'Welcome to Alpine Linux|localhost login:' out/validation/arm64-alpine.log; then
    echo 'PASS: ARM64 payload boots Alpine to userspace'
    exit 0
  fi
  sleep 1
done
tail -50 out/validation/arm64-alpine.log
exit 1
