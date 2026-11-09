#!/usr/bin/env bash
# Boot a BIOS-USB image in QEMU as a USB stick.
#
#   scripts/test-qemu.sh bios|uefi [IMAGE] [-- extra qemu args]
#   scripts/add-iso.sh IMAGE some.iso      # copy ISOs onto the image first
#
# Under WSL2 the window appears through WSLg. Use QEMU_DISPLAY=vnc to get a VNC server
# on :5900 instead, or QEMU_DISPLAY=none with QEMU_SERIAL for headless runs.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fw="${1:-}"; shift || true
img="${1:-$ROOT/out/biosusb.img}"; [[ $# -gt 0 ]] && shift
[[ "${1:-}" == "--" ]] && shift

[[ -f "$img" ]] || { echo "no image: $img (run scripts/mkusb.sh --image $img)" >&2; exit 1; }

args=(
  -machine q35 -m "${QEMU_MEM:-4096}" -smp 2
  -device qemu-xhci,id=xhci
  -drive "if=none,id=stick,format=raw,file=$img"
  -device usb-storage,bus=xhci.0,drive=stick,bootindex=0
  -device usb-tablet
)
[[ -w /dev/kvm ]] && args+=(-enable-kvm -cpu host)

case "$fw" in
  bios) ;;
  uefi)
    code=/usr/share/OVMF/OVMF_CODE_4M.fd
    vars_tpl=/usr/share/OVMF/OVMF_VARS_4M.fd
    vars="$ROOT/out/ovmf-vars.fd"
    [[ -f "$vars" ]] || cp "$vars_tpl" "$vars"
    args+=(
      -drive "if=pflash,format=raw,readonly=on,file=$code"
      -drive "if=pflash,format=raw,file=$vars"
    )
    ;;
  *) echo "usage: $0 bios|uefi [IMAGE] [-- qemu args]" >&2; exit 2 ;;
esac

case "${QEMU_DISPLAY:-gtk}" in
  vnc)  args+=(-display none -vnc :0) ;;
  none) args+=(-display none) ;;
  *)    args+=(-display "${QEMU_DISPLAY:-gtk}") ;;
esac
[[ -n "${QEMU_SERIAL:-}" ]] && args+=(-serial "$QEMU_SERIAL")

exec qemu-system-x86_64 "${args[@]}" "$@"
