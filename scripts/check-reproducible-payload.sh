#!/usr/bin/env bash
# Compare two assemblies from the SAME compiled components/toolchain and epoch.
# This does not claim independently rebuilt toolchains produce identical binaries.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="${GLASSBOOT_OUT_DIR:-$ROOT/out}"
work=$(mktemp -d /var/tmp/glassboot-repro.XXXXXX)
trap 'rm -rf "$work"' EXIT
mkdir "$work/out"
for component in grub wimboot hook winhelper memdisk; do ln -s "$OUT/$component" "$work/out/$component"; done
for run in 1 2; do
  GLASSBOOT_OUT_DIR="$work/out" bash "$ROOT/scripts/build-payload.sh" > "$work/build-$run.log"
  cp "$work/out/payload/manifest.txt" "$work/manifest-$run.txt"
done
cmp "$work/manifest-1.txt" "$work/manifest-2.txt"
mkdir -p "$OUT/validation"
cp "$work/manifest-1.txt" "$OUT/validation/reproducible-payload-manifest.txt"
echo 'PASS: two payload assemblies are byte-identical (same inputs/toolchain/epoch)'
