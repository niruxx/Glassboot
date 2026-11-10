set -euo pipefail
cd "$(dirname "$0")/../.."
root="$PWD"
# Usage: tests/menu-qemu-test.sh BUILD_DIR
# Root is needed for loop mounts. Only new temporary image files are formatted.
# Includes a sparse 3 TiB GPT image to exercise an ESP beyond the MBR address limit.
mkdir -p out/validation
work="$(mktemp -d /var/tmp/glassboot-menu-test.XXXXXX)"
loop=""; mnt=""; mounted=0
cleanup() {
  if [[ "$mounted" == 1 ]]; then umount "$mnt"; fi
  if [[ -n "$loop" ]]; then losetup -d "$loop"; fi
  if [[ -n "$mnt" ]]; then rmdir "$mnt"; fi
  rm -f "$work/mbr.img" "$work/gpt.img"
  rmdir "$work"
}
trap cleanup EXIT
build="$(realpath "$1")"
for style in mbr gpt; do
  args=(); [[ "$style" == gpt ]] && args+=(--gpt)
  size=1G; [[ "$style" == gpt ]] && size=3T
  "$build/glassboot-cli" install --image "$work/$style.img" --size "$size" --reserve 64M --payload out/payload "${args[@]}" > "out/validation/$style-install.log"
  loop="$(losetup --show -f -P "$work/$style.img")"
  mnt="$(mktemp -d)"
  mount "${loop}p1" "$mnt"; mounted=1
  mkdir -p "$mnt/Rescue"
  printf 'test ISO placeholder' > "$mnt/Rescue/Alpha.iso"
  printf 'test WIM placeholder' > "$mnt/Beta.wim"
  printf 'hidden ISO placeholder' > "$mnt/Hidden.iso"
  cat > "$mnt/glassboot/glassboot.cfg" <<'EOF'
language = es
view = tree
default = /Rescue/Alpha.iso
alias /Rescue/Alpha.iso = Rescue Toolkit
class /Rescue/Alpha.iso = debian
hide /Hidden.iso
EOF
  cat > "$mnt/glassboot/runtime-test.cfg" <<'EOF'
insmod serial
serial --port=0x3f8 --speed=115200
terminal_output --append serial
echo GB_TEST_START
gb_scan "($gb_data)" "TOOLKIT"
echo FILTER_COUNT=$gb_iso_count
echo DEFAULT_ID=$gb_default_id
cat (proc)/glassboot_menu
gb_scan "($gb_data)" "["
echo LITERAL_COUNT=$gb_iso_count
gb_scan "($gb_data)" ""
echo ALL_COUNT=$gb_iso_count
echo $"Reboot"
echo GB_TEST_DONE
EOF
  umount "$mnt"; mounted=0; losetup -d "$loop"; loop=""; rmdir "$mnt"; mnt=""
done
for spec in bios:mbr uefi:mbr uefi:gpt; do
  fw="${spec%:*}"; style="${spec#*:}"; name="$fw-$style"
  QEMU_MEM=512 QEMU_SERIAL="$root/out/validation/$name-serial.log" bash scripts/qemu-script.sh "$fw" "$work/$style.img" \
    wait:10 "shot:$root/out/validation/$name-menu.png" \
    key:f3 key:ret 'type:TOOLKIT' key:ret wait:2 "shot:$root/out/validation/$name-search.png" \
    key:esc wait:1 key:esc wait:1 key:c wait:1 'type:source ($gb_data)/glassboot/runtime-test.cfg' key:ret wait:8 "shot:$root/out/validation/$name-runtime.png"
  python3 - "$root/out/validation/$name-serial.log" <<'PY'
import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text(errors='replace')
for expected in ['GB_TEST_START','FILTER_COUNT=1','DEFAULT_ID=gbdir:/Rescue>gb:/Rescue/Alpha.iso','--class "debian"','LITERAL_COUNT=0','ALL_COUNT=2','Reiniciar','GB_TEST_DONE']:
    assert expected in s,(sys.argv[1],expected,s[-3000:])
print('PASS:',sys.argv[1])
PY
done
