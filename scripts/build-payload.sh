#!/usr/bin/env bash
# Assemble the Glassboot "payload": the exact bytes the installer writes to a USB stick.
# Every installer (the Qt utility on Windows/Linux, scripts/mkusb.sh) consumes this same
# payload, so a stick made on any OS is bit-for-bit the same apart from its size, volume
# serials and partition table.
#
#   out/payload/
#     mbr.bin       GRUB boot.img (first 440 bytes go into the MBR)
#     core.img      GRUB i386-pc core image, written at LBA 1 (inside the 1 MiB gap)
#     esp.img       complete FAT16 EFI System Partition image (ESP_MB MiB)
#     manifest.txt  name, size, sha256 of each file + layout constants
#
# Requires scripts/build.sh to have been run. No root needed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${GLASSBOOT_OUT_DIR:-$ROOT/out}"
GRUB="$OUT/grub"
PAYLOAD="$OUT/payload"
ESP_MB="${ESP_MB:-64}"
ESP_LABEL="GLASS_EFI"
VERSION="$(sed -n 's/^set glassboot_version="\(.*\)"/\1/p' "$ROOT/boot/grub/grub.cfg")"

log() { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
[[ -x "$GRUB/bin/grub-mkimage" ]] || { echo "run scripts/build.sh first" >&2; exit 1; }
command -v mcopy >/dev/null || { echo "mtools missing (scripts/install-deps.sh)" >&2; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD"

# Modules embedded in every core image: just enough to read the ESP and run the
# early config. Everything else is loaded from /boot/grub/<platform>/ on the ESP.
CORE_MODS="part_msdos fat search search_label test regexp configfile echo normal"

# ---------------------------------------------------------------- BIOS core.img
# At boot $root is the BIOS boot drive, e.g. "hd0". The ESP is always partition 2.
cat > "$STAGE/early-pc.cfg" <<'EOF'
set gb_esp="${root},msdos2"
if [ -f "($gb_esp)/boot/grub/grub.cfg" ]; then
  set root="$gb_esp"
else
  search --no-floppy --label GLASS_EFI --set=root
fi
set prefix="($root)/boot/grub"
EOF
log "Building BIOS core.img"
"$GRUB/bin/grub-mkimage" -O i386-pc -d "$GRUB/lib/grub/i386-pc" \
  -c "$STAGE/early-pc.cfg" -p /boot/grub -o "$PAYLOAD/core.img" \
  biosdisk $CORE_MODS
cp "$GRUB/lib/grub/i386-pc/boot.img" "$PAYLOAD/mbr.bin"

core_size=$(stat -c %s "$PAYLOAD/core.img")
# The gap before partition 1 is LBA 1..2047.
(( core_size <= 2047 * 512 )) || { echo "core.img too large: $core_size" >&2; exit 1; }

# ---------------------------------------------------------------- ESP tree
log "Staging ESP"
ESP="$STAGE/esp"
mkdir -p "$ESP/EFI/BOOT" "$ESP/boot/grub" "$ESP/boot/glassboot/win"

# On UEFI, GRUB sets $root to the partition it was loaded from (the ESP).
"$GRUB/bin/grub-mkimage" -O x86_64-efi -d "$GRUB/lib/grub/x86_64-efi" \
  -p /boot/grub -o "$ESP/EFI/BOOT/BOOTX64.EFI" $CORE_MODS
"$GRUB/bin/grub-mkimage" -O i386-efi -d "$GRUB/lib/grub/i386-efi" \
  -p /boot/grub -o "$ESP/EFI/BOOT/BOOTIA32.EFI" $CORE_MODS

for plat in i386-pc x86_64-efi i386-efi; do
  mkdir -p "$ESP/boot/grub/$plat"
  cp "$GRUB/lib/grub/$plat/"*.mod "$GRUB/lib/grub/$plat/"*.lst "$ESP/boot/grub/$plat/"
done
cp "$ROOT/boot/grub/"*.cfg "$ESP/boot/grub/"
# $prefix/fonts/unicode.pf2, as a normal GRUB install has: ISO menus that run inside our
# GRUB (e.g. Debian's "loadfont unicode") need it, or every character shows as a box.
# Made with our own grub-mkfont from GNU Unifont (GPL; package fonts-unifont).
UNIFONT=/usr/share/fonts/opentype/unifont/unifont.otf
[[ -f "$UNIFONT" ]] || { echo "GNU Unifont missing (scripts/install-deps.sh)" >&2; exit 1; }
mkdir -p "$ESP/boot/grub/fonts"
"$GRUB/bin/grub-mkfont" -s 16 -o "$ESP/boot/grub/fonts/unicode.pf2" "$UNIFONT"
cp "$ROOT/boot/win/"* "$ESP/boot/glassboot/win/"
if [[ -f "$OUT/wimboot/wimboot" ]]; then
  cp "$OUT/wimboot/wimboot" "$ESP/boot/glassboot/"
  [[ -f "$OUT/wimboot/wimboot.i386.efi" ]] && cp "$OUT/wimboot/wimboot.i386.efi" "$ESP/boot/glassboot/"
else
  echo "warning: wimboot not built; Windows ISOs will not boot" >&2
fi
HOOK="$OUT/hook/glassboot-hook-x86_64.cpio"
[[ -f "$HOOK" ]] || { echo "initrd hook not built; run scripts/build.sh hook" >&2; exit 1; }
cp "$HOOK" "$ESP/boot/glassboot/"
[[ -f "$OUT/winhelper/glassboot-attach.exe" ]] || { echo "Windows helper not built; run scripts/build.sh winhelper" >&2; exit 1; }
cp "$OUT/winhelper/"*.exe "$ESP/boot/glassboot/win/"
[[ -f "$OUT/memdisk/memdisk" ]] || { echo "memdisk not built; run scripts/build.sh memdisk" >&2; exit 1; }
cp "$OUT/memdisk/memdisk" "$ESP/boot/glassboot/"
cp "$ROOT/versions.env" "$ESP/boot/glassboot/"
echo "$VERSION" > "$ESP/boot/glassboot/VERSION"

# ---------------------------------------------------------------- ESP image
log "Building esp.img (${ESP_MB} MiB FAT16)"
# Fixed serial for reproducibility; the installer randomises it per stick.
mkfs.vfat -C -F 16 -n "$ESP_LABEL" -i 47420001 "$PAYLOAD/esp.img" $((ESP_MB * 1024)) >/dev/null
MTOOLS_SKIP_CHECK=1 mcopy -s -Q -m -i "$PAYLOAD/esp.img" "$ESP"/* ::/
used=$(MTOOLS_SKIP_CHECK=1 mdu -s -i "$PAYLOAD/esp.img" :: 2>/dev/null | awk '{print $NF}' || true)

# ---------------------------------------------------------------- manifest
log "Writing manifest"
{
  echo "# Glassboot payload manifest"
  echo "version $VERSION"
  echo "esp_mb $ESP_MB"
  echo "data_label GLASSBOOT"
  echo "esp_label $ESP_LABEL"
  for f in mbr.bin core.img esp.img; do
    echo "file $f $(stat -c %s "$PAYLOAD/$f") $(sha256sum "$PAYLOAD/$f" | cut -d' ' -f1)"
  done
} > "$PAYLOAD/manifest.txt"

cat "$PAYLOAD/manifest.txt"
log "Payload ready in $PAYLOAD (ESP usage: ${used:-?})"
