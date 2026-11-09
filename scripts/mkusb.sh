#!/usr/bin/env bash
# Create a Glassboot drive or disk image from the command line (Linux).
#
#   scripts/mkusb.sh --image out/glassboot.img [--size 8G]
#   scripts/mkusb.sh --device /dev/sdX            # DESTROYS ALL DATA ON /dev/sdX
#
# Thin wrapper around glassboot-cli (Utility-Source/), so the command line, the GUI and
# this script all write sticks the same way. Builds the payload and the CLI if needed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${GLASSBOOT_OUT_DIR:-$ROOT/out}"
CLI_BUILD="${GLASSBOOT_CLI_BUILD:-/var/tmp/glassboot-cli-build}"

if [[ ! -f "$OUT/payload/manifest.txt" ]]; then
  "$ROOT/scripts/build-payload.sh"
fi
if [[ ! -x "$CLI_BUILD/glassboot-cli" ]]; then
  cmake -S "$ROOT/Utility-Source" -B "$CLI_BUILD" -DGLASSBOOT_BUILD_GUI=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
fi
cmake --build "$CLI_BUILD" --target glassboot-cli >/dev/null

args=() size="8G" image=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)  image="$2"; args+=(--image "$2"); shift 2 ;;
    --device) args+=(--device "$2"); shift 2 ;;
    --size)   size="$2"; shift 2 ;;
    --yes)    args+=(--yes); shift ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$image" ]] && args+=(--size "$size")

exec "$CLI_BUILD/glassboot-cli" install --payload "$OUT/payload" "${args[@]}"
