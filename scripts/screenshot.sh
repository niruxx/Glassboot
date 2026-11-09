#!/usr/bin/env bash
# Headless smoke test: boot an image, wait, save a screenshot of the screen.
#   scripts/screenshot.sh bios|uefi IMAGE OUT.png [SECONDS] [KEYS...]
# KEYS are QEMU sendkey names sent one per second after the wait (e.g. down ret).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fw="$1" img="$2" png="$3" wait="${4:-8}"; shift 4 || shift $#
{
  sleep "$wait"
  for k in "$@"; do echo "sendkey $k"; sleep 1; done
  [[ $# -gt 0 ]] && sleep 3
  echo "screendump $png -f png"
  sleep 1
  echo quit
} | QEMU_DISPLAY=none "$ROOT/scripts/test-qemu.sh" "$fw" "$img" -- -monitor stdio >/dev/null
