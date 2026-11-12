#!/usr/bin/env bash
# End-to-end ISO discovery by a real Linux initrd on ext4 and on 4Kn exFAT.
# Usage: linux-image-boot-test.sh BUILD_DIR ALPINE_STANDARD_ISO
set -euo pipefail
cd "$(dirname "$0")/../.."
cli="$(realpath "$1")/glassboot-cli"; iso="$(realpath "$2")"
work=$(mktemp -d /var/tmp/glassboot-linux-boot.XXXXXX)
loop=""; mounted=0
cleanup() {
  [[ "$mounted" == 0 ]] || umount "$work/mnt"
  [[ -z "$loop" ]] || losetup -d "$loop"
  rm -rf "$work"
}
trap cleanup EXIT
mkdir "$work/mnt"; mkdir -p out/validation
tr -d '\r' < scripts/qemu-script.sh > "$work/qemu.sh"
for spec in ${GLASSBOOT_BOOT_SPECS:-ext4 4kn}; do
  fs="$spec"; sector=512; bus=usb; opts=()
  if [[ "$spec" == 4kn ]]; then fs=exfat; sector=4096; bus=nvme; opts=(--gpt --sector-size 4096); fi
  "$cli" install --image "$work/test.img" --size 1G --filesystem "$fs" "${opts[@]}" --payload out/payload > "$work/install.log"
  loop=$(losetup --find --show --partscan --sector-size "$sector" "$work/test.img")
  mount -t "${fs/ntfs/ntfs-3g}" "${loop}p1" "$work/mnt"; mounted=1
  cp "$iso" "$work/mnt/Alpine.iso"
  printf 'args /Alpine.iso = console=ttyS0,115200 loglevel=7\n' > "$work/mnt/glassboot/glassboot.cfg"
  printf 'gb_boot_iso /Alpine.iso\n' > "$work/mnt/glassboot/runtime.cfg"
  umount "$work/mnt"; mounted=0; losetup -d "$loop"; loop=""
  QEMU_MEM=1024 QEMU_DISK_BUS="$bus" QEMU_SECTOR_SIZE="$sector" \
    QEMU_SERIAL_SOCKET="$work/serial.sock" \
    QEMU_SERIAL="$PWD/out/validation/linux-boot-$spec.log" \
    bash "$work/qemu.sh" uefi "$work/test.img" wait:8 key:c wait:1 \
      'type:source ($gb_data)/glassboot/runtime.cfg' key:ret wait:60 \
      'serial:dmesg | grep glassboot; ls /sys/block; modprobe xhci_pci; modprobe usb_storage; sleep 2; dmesg | grep glassboot' wait:5
  if ! grep -Eq 'Welcome to Alpine Linux|localhost login:' "out/validation/linux-boot-$spec.log"; then
    tail -80 "out/validation/linux-boot-$spec.log"; exit 1
  fi
  echo "PASS: Alpine ISO reached userspace from $spec"
done
