#!/usr/bin/env bash
# Create a BIOS-USB drive (or a disk image for testing / writing with a raw imager).
#
#   scripts/mkusb.sh --image out/biosusb.img [--size 8G]
#   scripts/mkusb.sh --device /dev/sdX            # DESTROYS ALL DATA ON /dev/sdX
#
# Layout (MBR, works with both Legacy BIOS and UEFI):
#   [MBR + GRUB core.img in the 1 MiB gap]
#   p1  exFAT  "BIOSUSB"      everything except the last ESP_MB  -> drop ISOs here
#   p2  FAT16  "BIOSUSB_EFI"  ESP_MB MiB, type 0xEF             -> GRUB, configs, wimboot
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${BIOSUSB_OUT_DIR:-$ROOT/out}"
GRUB="$OUT/grub"
ESP_MB="${ESP_MB:-64}"
DATA_LABEL="BIOSUSB"
ESP_LABEL="BIOSUSB_EFI"

die() { echo "error: $*" >&2; exit 1; }
log() { printf '\033[1;34m==> %s\033[0m\n' "$*"; }

mode="" target="" size="8G" assume_yes=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)  mode=image;  target="$2"; shift 2 ;;
    --device) mode=device; target="$2"; shift 2 ;;
    --size)   size="$2"; shift 2 ;;
    --yes)    assume_yes=1; shift ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[[ -n "$mode" ]] || die "specify --image FILE or --device /dev/sdX (see --help)"
[[ $EUID -eq 0 ]] || die "must run as root (partitioning and mounting)"
[[ -x "$GRUB/sbin/grub-install" ]] || die "GRUB not built; run scripts/build.sh first"
[[ -f "$OUT/wimboot/wimboot" ]] || echo "warning: wimboot not built; Windows ISOs will not boot" >&2

WORK="$(mktemp -d /tmp/biosusb.XXXXXX)"
LOOP=""
cleanup() {
  set +e
  umount "$WORK/esp" "$WORK/data" 2>/dev/null
  [[ -n "$LOOP" ]] && losetup -d "$LOOP"
  rm -rf "$WORK"
}
trap cleanup EXIT

# ---------------------------------------------------------------- target setup
if [[ $mode == image ]]; then
  rm -f "$target"
  truncate -s "$size" "$target"
  LOOP="$(losetup --show -f -P "$target")"
  DEV="$LOOP"
else
  DEV="$target"
  [[ -b "$DEV" ]] || die "$DEV is not a block device"
  [[ "$(lsblk -dno TYPE "$DEV")" == disk ]] || die "$DEV is not a whole disk (give /dev/sdX, not a partition)"
  # Refuse anything that currently holds a mounted filesystem (e.g. the system disk).
  if lsblk -no MOUNTPOINTS "$DEV" | grep -q .; then
    die "$DEV has mounted filesystems; refusing"
  fi
  lsblk -o NAME,SIZE,MODEL,TRAN,RM "$DEV"
  if [[ $assume_yes -ne 1 ]]; then
    echo
    echo "ALL DATA ON $DEV WILL BE DESTROYED."
    read -r -p "Type the device name ($(basename "$DEV")) to continue: " answer
    [[ "$answer" == "$(basename "$DEV")" ]] || die "aborted"
  fi
fi

part() {  # part DEV N -> partition device path (/dev/sdb1, /dev/loop0p1, /dev/nvme0n1p1)
  if [[ "$1" =~ [0-9]$ ]]; then echo "${1}p$2"; else echo "${1}$2"; fi
}

# ---------------------------------------------------------------- partitioning
log "Partitioning $DEV"
total=$(blockdev --getsz "$DEV")            # 512-byte sectors
esp_sectors=$((ESP_MB * 2048))
data_start=2048                              # 1 MiB: room for GRUB core.img
data_size=$(( (total - data_start - esp_sectors) / 2048 * 2048 ))
esp_start=$((data_start + data_size))
(( data_size > 2048 * 64 )) || die "device too small"

wipefs -aq "$DEV"
sfdisk -q "$DEV" <<SFDISK
label: dos
start=$data_start, size=$data_size, type=7, bootable
start=$esp_start, size=$esp_sectors, type=ef
SFDISK
partprobe "$DEV" 2>/dev/null || blockdev --rereadpt "$DEV" || true
udevadm settle 2>/dev/null || sleep 1

P1="$(part "$DEV" 1)" P2="$(part "$DEV" 2)"
mkfs.exfat -q -L "$DATA_LABEL" "$P1" >/dev/null 2>&1 || mkfs.exfat -L "$DATA_LABEL" "$P1"
mkfs.vfat -F 16 -n "$ESP_LABEL" "$P2" >/dev/null

mkdir -p "$WORK/esp" "$WORK/data"
mount "$P2" "$WORK/esp"

# ---------------------------------------------------------------- GRUB
export PATH="$GRUB/bin:$GRUB/sbin:$PATH"
MODS="part_msdos fat exfat"

log "Installing GRUB (Legacy BIOS)"
grub-install --target=i386-pc --directory="$GRUB/lib/grub/i386-pc" \
  --boot-directory="$WORK/esp/boot" --modules="$MODS" "$DEV"

log "Installing GRUB (UEFI x86_64)"
grub-install --target=x86_64-efi --directory="$GRUB/lib/grub/x86_64-efi" \
  --boot-directory="$WORK/esp/boot" --efi-directory="$WORK/esp" \
  --removable --no-nvram --modules="$MODS"

log "Installing GRUB (UEFI ia32)"
grub-install --target=i386-efi --directory="$GRUB/lib/grub/i386-efi" \
  --boot-directory="$WORK/esp/boot" --efi-directory="$WORK/esp" \
  --removable --no-nvram --modules="$MODS"

log "Installing BIOS-USB menu and loaders"
cp "$ROOT/boot/grub/"*.cfg "$WORK/esp/boot/grub/"
mkdir -p "$WORK/esp/boot/biosusb/win"
cp "$ROOT/boot/win/"* "$WORK/esp/boot/biosusb/win/"
if [[ -d "$OUT/wimboot" ]]; then
  cp "$OUT/wimboot/"* "$WORK/esp/boot/biosusb/"
fi
cp "$ROOT/versions.env" "$WORK/esp/boot/biosusb/"
umount "$WORK/esp"

# ---------------------------------------------------------------- data partition
log "Preparing data partition"
modprobe exfat 2>/dev/null || true
if grep -qw exfat /proc/filesystems; then
  mount "$P1" "$WORK/data"
else
  mount.exfat-fuse "$P1" "$WORK/data"
fi
mkdir -p "$WORK/data/biosusb"
# 1 KiB GRUB environment block: GRUB records the selected ISO here so the
# Windows PE helper knows which ISO to attach. It is overwritten in place.
grub-editenv "$WORK/data/biosusb/grubenv" create
cat > "$WORK/data/biosusb/README.txt" <<'TXT'
BIOS-USB data partition.

Copy .iso files anywhere in the top two folder levels of this drive
(e.g. \ubuntu.iso or \linux\fedora.iso) and boot from the USB stick.

Do not delete this "biosusb" folder; it is used when booting Windows ISOs.
TXT
umount "$WORK/data"
sync

log "Done: $target"
