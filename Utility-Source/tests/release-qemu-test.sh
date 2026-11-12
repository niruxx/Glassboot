#!/usr/bin/env bash
# Root/Linux: boot-file verification must accept clean ESPs and reject corruption.
set -euo pipefail
cd "$(dirname "$0")/../.."
cli="$(realpath "$1")/glassboot-cli"
work=$(mktemp -d /var/tmp/glassboot-integrity.XXXXXX)
loop=""; mounted=0
cleanup() {
  [[ "$mounted" == 0 ]] || umount "$work/mnt"
  [[ -z "$loop" ]] || losetup -d "$loop"
  rm -rf "$work"
}
trap cleanup EXIT
mkdir "$work/mnt"; mkdir -p out/validation
"$cli" install --image "$work/test.img" --size 512M --payload out/payload > "$work/install.log"
loop=$(losetup --find --show --partscan "$work/test.img")
mount "${loop}p2" "$work/mnt"; mounted=1
python3 - "$work/mnt/boot/grub/grub.cfg" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); p.write_text('insmod serial\nserial --speed=115200\nterminal_output --append serial\n'+p.read_text())
PY
python3 scripts/release.py esp-manifest "$work/mnt"
umount "$work/mnt"; mounted=0; losetup -d "$loop"; loop=""
for state in clean corrupt; do
  if [[ "$state" == corrupt ]]; then
    loop=$(losetup --find --show --partscan "$work/test.img")
    mount "${loop}p2" "$work/mnt"; mounted=1
    printf '\n# intentional test corruption\n' >> "$work/mnt/boot/grub/glassboot-search.cfg"
    umount "$work/mnt"; mounted=0; losetup -d "$loop"; loop=""
  fi
  for fw in bios uefi; do
    name="integrity-$state-$fw"
    QEMU_MEM=512 QEMU_SERIAL="$PWD/out/validation/$name.log" \
      bash scripts/qemu-script.sh "$fw" "$work/test.img" wait:12 "shot:$PWD/out/validation/$name.png"
    if [[ "$state" == clean ]]; then
      grep -Eq 'Glassboot: verified [0-9]+ boot files' "out/validation/$name.log"
      ! grep -q 'Boot-file verification failed' "out/validation/$name.log"
    else
      grep -Fq 'file checksum mismatch: boot/grub/glassboot-search.cfg' "out/validation/$name.log"
      grep -q 'Boot-file verification failed' "out/validation/$name.log"
    fi
    echo "PASS: $name"
  done
done
python3 - <<'PY'
from PIL import Image, ImageChops
from pathlib import Path
for fw in ['bios','uefi']:
    a=Image.open(Path('out/validation')/f'integrity-clean-{fw}.png').convert('RGB')
    b=Image.open(Path('out/validation')/f'integrity-corrupt-{fw}.png').convert('RGB')
    assert a.width>=640 and a.height>=400 and a.size==b.size
    assert ImageChops.difference(a,b).getbbox(), 'clean and failure screens must differ'
print('PASS: BIOS/UEFI success and failure screenshot assertions')
PY
