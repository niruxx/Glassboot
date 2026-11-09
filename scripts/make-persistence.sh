#!/usr/bin/env bash
# Create a persistence file for a live Linux ISO booted from Glassboot.
#
#   scripts/make-persistence.sh LABEL SIZE OUTPUT_FILE
#
#   LABEL  file system label the distribution looks for: casper-rw (Ubuntu family),
#          persistence (Debian live / Kali), or any label you pass with
#          rd.live.overlay=LABEL=... (Fedora) / cow_label=... (Arch)
#   SIZE   e.g. 4G
#
# The file holds an ext4 file system. For the "persistence" label, a persistence.conf
# with "/ union" is added, as Debian's live-boot requires. See docs/CONFIG.md.
set -euo pipefail
[[ $# -eq 3 ]] || { sed -n '2,12p' "$0"; exit 2; }
label="$1" size="$2" out="$3"
[[ -e "$out" ]] && { echo "$out already exists; refusing to overwrite" >&2; exit 1; }

# fallocate keeps the file in one piece where possible (Glassboot maps it directly).
fallocate -l "$size" "$out" 2>/dev/null || truncate -s "$size" "$out"
mkfs.ext4 -q -F -L "$label" "$out"

if [[ "$label" == "persistence" ]]; then
  if command -v debugfs >/dev/null; then
    tmp=$(mktemp)
    echo "/ union" > "$tmp"
    debugfs -w -R "write $tmp persistence.conf" "$out" >/dev/null
    rm -f "$tmp"
  else
    echo "warning: debugfs not found; add persistence.conf ('/ union') yourself" >&2
  fi
fi
echo "Created $out ($size, ext4, label $label)."
