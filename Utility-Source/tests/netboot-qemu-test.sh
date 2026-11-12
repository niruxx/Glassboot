#!/usr/bin/env bash
# Usage: netboot-qemu-test.sh BIOS_OR_UEFI DEBIAN_LIVE_ISO
# No privileged networking: QEMU user networking supplies DHCP/TFTP.
set -euo pipefail
cd "$(dirname "$0")/../.."
fw="${1:?usage: netboot-qemu-test.sh bios|uefi DEBIAN_LIVE_ISO}"
iso="$(realpath "${2:?specify a Debian Live ISO}")"
[[ "$fw" == bios || "$fw" == uefi ]] || exit 2
root="$PWD"
work=$(mktemp -d /var/tmp/glassboot-pxe-real.XXXXXX)
http=""; qemu=""
cleanup() {
 [[ -z "$qemu" ]] || { kill "$qemu" 2>/dev/null || true; wait "$qemu" 2>/dev/null || true; }
 [[ -z "$http" ]] || { kill "$http" 2>/dev/null || true; wait "$http" 2>/dev/null || true; }
 rm -rf "$work"
}
trap cleanup EXIT
mkdir "$work/isos"
cp --reflink=auto "$iso" "$work/isos/Debian.iso"
python3 scripts/netboot.py prepare --isos "$work/isos" --output "$work/tree" --server 10.0.2.2 --interface eth0 --port 18080 --platforms i386-pc x86_64-efi
python3 - "$work/tree/boot/grub/grub.cfg" <<'PY'
import sys
from pathlib import Path
p=Path(sys.argv[1]); s=p.read_text().replace('set timeout=-1', 'set timeout=2')
s='insmod serial\nserial --port=0x3f8 --speed=115200\nterminal_output serial\n'+s
s=s.replace('boot=live components', 'console=ttyS0,115200 boot=live components')
p.write_text(s)
PY
python3 scripts/netboot.py serve --root "$work/tree" --port 18080 > out/validation/pxe-http.log 2>&1 &
http=$!
args=()
bootfile=glassboot.0
if [[ "$fw" == uefi ]]; then
 cp /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"
 args+=(-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd
         -drive "if=pflash,format=raw,file=$work/vars.fd")
 bootfile=BOOTX64.EFI
fi
log="$root/out/validation/pxe-debian-$fw.log"
: > "$log"
qemu-system-x86_64 "${args[@]}" -machine q35 -m 2048 -smp 2 -display none -monitor none \
 -serial "file:$log" \
 -netdev "user,id=net,tftp=$work/tree,bootfile=$bootfile" -device e1000,netdev=net -boot n &
qemu=$!
for ((i=0; i<240; i++)); do
 kill -0 "$qemu" 2>/dev/null || { echo "QEMU exited unexpectedly"; exit 1; }
 if grep -Eq 'Debian GNU/Linux.*ttyS0|debian login:' "$log"; then
  echo "PASS: $fw PXE fetched Debian Live over HTTP and reached userspace"
  exit 0
 fi
 sleep 1
done
tail -60 "$log"
exit 1
