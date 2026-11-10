#!/usr/bin/env bash
# Usage: bash .../freebsd-qemu-test.sh FREEBSD_BOOTONLY_ISO FREEBSD_MEMSTICK_IMG [bios|uefi]
# Builds temporary rescue media. Never attaches a host disk to QEMU.
set -euo pipefail
cd "$(dirname "$0")/../.."
iso="$(realpath "$1")"; img="$(realpath "$2")"; fw="${3:-bios}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/tree/boot/grub" out/validation
if [[ ! -f out/grub/share/grub/unicode.pf2 ]]; then
  out/grub/bin/grub-mkfont -s 16 -o out/grub/share/grub/unicode.pf2 \
    /usr/share/fonts/opentype/unifont/unifont.otf
fi
for kind in ${FREEBSD_TEST_KINDS:-iso img}; do
  cp "${!kind}" "$work/tree/freebsd.$kind"
  # Serial output is the only test-specific change to the production boot function.
  sed 's/\r$//; s/kfreebsd "\$1\/boot\/kernel\/kernel"/kfreebsd --serial "$1\/boot\/kernel\/kernel"/' \
    boot/grub/glassboot-detect.cfg > "$work/tree/boot/grub/detect.cfg"
  cat > "$work/tree/boot/grub/grub.cfg" <<EOF
serial --speed=115200
terminal_output serial
set timeout=0
insmod glassboot
insmod ufs2
insmod part_gpt
insmod part_msdos
insmod part_bsd
source /boot/grub/detect.cfg
set gb_data="\$root"
set gb_mode=""
function gb_pause { sleep 1; }
EOF
  if [[ "$kind" == iso ]]; then
    cat >> "$work/tree/boot/grub/grub.cfg" <<'EOF'
loopback loop /freebsd.iso
gb_boot_freebsd_ram (loop) /freebsd.iso cd9660:/dev/md0
EOF
  else
    echo 'gb_boot_img /freebsd.img' >> "$work/tree/boot/grub/grub.cfg"
  fi
  out/grub/bin/grub-script-check "$work/tree/boot/grub/grub.cfg"
  platform=i386-pc
  [[ "$fw" == uefi ]] && platform=x86_64-efi
  out/grub/bin/grub-mkrescue --directory="$PWD/out/grub/lib/grub/$platform" \
    --fonts="" --themes="" -o "$work/test.iso" "$work/tree" > "$work/build.log" 2>&1 || { cat "$work/build.log"; exit 1; }
  args=()
  if [[ "$fw" == uefi ]]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"
    args+=(-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd
           -drive "if=pflash,format=raw,file=$work/vars.fd")
  fi
  log="$PWD/out/validation/freebsd-$kind-$fw.log"
  : > "$log"
  timeout "${FREEBSD_TEST_TIMEOUT:-180}" qemu-system-x86_64 -m 2048 -smp 2 -display none -monitor none \
    -serial "file:$log" -cdrom "$work/test.iso" -boot d "${args[@]}" &
  guest=$!
  while kill -0 "$guest" 2>/dev/null; do
    if grep -Eq 'Welcome to FreeBSD|mountroot>|panic:' "$log" 2>/dev/null; then
      kill "$guest" 2>/dev/null || true
      break
    fi
    sleep 2
  done
  wait "$guest" || [[ $? == 124 || $? == 143 ]]
  grep -E 'Trying to mount root from (cd9660:/dev/md0|ufs:/dev/md0(p[0-9]+|s[0-9]+a?))' "$log"
  if grep -Eq 'mountroot>|Mounting from .* failed|panic:' "$log"; then
    tail -40 "$log"; exit 1
  fi
  grep -Eq 'Welcome to FreeBSD|FreeBSD Installer|login:' "$log"
  echo "PASS: FreeBSD $kind $fw reached userspace with RAM root"
  rm "$work/tree/freebsd.$kind"
done
