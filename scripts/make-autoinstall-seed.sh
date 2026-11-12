#!/usr/bin/env bash
# Build a NoCloud VFAT seed for `autoinstall ISO = /path/cidata.img`.
# Usage: make-autoinstall-seed.sh USER_DATA OUTPUT.img [META_DATA]
set -euo pipefail
[[ $# -ge 2 && $# -le 3 ]] || { echo "usage: $0 USER_DATA OUTPUT.img [META_DATA]" >&2; exit 2; }
[[ -f "$1" ]] || { echo "user-data not found" >&2; exit 1; }
[[ ! -e "$2" ]] || { echo "output already exists: $2" >&2; exit 1; }
command -v mkfs.vfat >/dev/null
command -v mcopy >/dev/null
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$1" "$work/user-data"
if [[ $# == 3 ]]; then cp "$3" "$work/meta-data"; else
  printf 'instance-id: glassboot-autoinstall\n' > "$work/meta-data"
fi
bytes=$(( $(stat -c %s "$work/user-data") + $(stat -c %s "$work/meta-data") ))
size=$(( ((bytes + 1048575) / 1048576 + 4) * 1024 ))
mkfs.vfat -C -n CIDATA "$work/seed.img" "$size" >/dev/null
mcopy -i "$work/seed.img" "$work/user-data" "$work/meta-data" ::/
# noclobber prevents overwriting an existing output even if it appeared during build.
(set -o noclobber; cat "$work/seed.img" > "$2")
echo "Created $2 (CIDATA; user-data and meta-data)"
