#!/usr/bin/env bash
# Root, Linux/QEMU. Verify GRUB-generated extents against every byte of a fixture.
set -euo pipefail
cd "$(dirname "$0")/../.."
cli="$(realpath "$1")/glassboot-cli"
work=$(mktemp -d /var/tmp/glassboot-fsmenu.XXXXXX)
loop=""; mounted=0
cleanup() {
  [[ "$mounted" == 0 ]] || umount "$work/mnt"
  [[ -z "$loop" ]] || losetup -d "$loop"
  rm -rf "$work"
}
trap cleanup EXIT
mkdir "$work/mnt"; mkdir -p out/validation
python3 - "$work/fixture" <<'PY'
import sys
from pathlib import Path
Path(sys.argv[1]).write_bytes(bytes(range(256))*4096)
PY
tr -d '\r' < scripts/qemu-script.sh > "$work/qemu.sh"
for fs in ${GLASSBOOT_TEST_FS:-ntfs ext4 xfs udf 4kn}; do
  opts=(); sector=512; kind="$fs"; bus=usb
  if [[ "$fs" == 4kn ]]; then opts=(--gpt --sector-size 4096); sector=4096; kind=exfat; bus=nvme; fi
  "$cli" install --image "$work/test.img" --size 1G --filesystem "$kind" "${opts[@]}" --payload out/payload > "$work/install.log"
  loop=$(losetup --find --show --partscan --sector-size "$sector" "$work/test.img")
  mount -t "${kind/ntfs/ntfs-3g}" "${loop}p1" "$work/mnt"; mounted=1
  cp "$work/fixture" "$work/mnt/Test.iso"
  cat > "$work/mnt/glassboot/runtime.cfg" <<'EOF'
serial --speed=115200
terminal_output serial
echo GB_FS_START
echo DATA_FS=$gb_data_fs
if gb_hook_enable /Test.iso "$gb_partuuid" "$gb_res/glassboot-hook-x86_64.cpio" "($gb_data)"; then
  echo GB_MAP_START
  cat (proc)/glassboot_iso_map
  echo GB_MAP_END
  echo GB_FS_DONE
fi
EOF
  umount "$work/mnt"; mounted=0; losetup -d "$loop"; loop=""
  QEMU_MEM=512 QEMU_DISK_BUS="$bus" QEMU_SECTOR_SIZE="$sector" QEMU_SERIAL="$PWD/out/validation/fs-menu-$fs.log" \
    bash "$work/qemu.sh" uefi "$work/test.img" wait:8 key:c wait:1 \
      'type:source ($gb_data)/glassboot/runtime.cfg' key:ret wait:8
  python3 - "$work/test.img" "$work/fixture" "out/validation/fs-menu-$fs.log" "$fs" <<'PY'
import re,sys
from pathlib import Path
image,fixture,log,kind=sys.argv[1:]
s=Path(log).read_text(errors='replace')
assert 'GB_FS_DONE' in s, s[-4000:]
if kind!='4kn':
    text=s.split('GB_MAP_START')[-1].split('GB_MAP_END')[0]
    text=re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', text).strip()
    lines=[line.strip() for line in text.splitlines() if line.strip()]
    magic,size,n=lines[0].split()
    assert magic=='GBMAP1' and int(size)==Path(fixture).stat().st_size
    output=bytearray()
    with open(image,'rb') as f:
        for line in lines[1:1+int(n)]:
            off,length=map(int,line.split()); f.seek(1048576+off); output.extend(f.read(length))
    assert output==Path(fixture).read_bytes(), (kind,'incorrect physical extents')
print('PASS:',kind,'UEFI menu and file-map bytes' if kind!='4kn' else '4Kn UEFI menu and hook enable')
PY
done
