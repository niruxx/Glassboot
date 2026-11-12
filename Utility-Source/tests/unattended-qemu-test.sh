#!/usr/bin/env bash
# Root/Linux: boot a real Linux kernel with all four configured answer transports.
# Usage: unattended-qemu-test.sh BUILD_DIR ALPINE_STANDARD_ISO
set -euo pipefail
cd "$(dirname "$0")/../.."
cli="$(realpath "$1")/glassboot-cli"; iso="$(realpath "$2")"
work=$(mktemp -d /var/tmp/glassboot-answers.XXXXXX)
loop=""; mounted=0; iso_mounted=0
cleanup() {
  [[ "$mounted" == 0 ]] || umount "$work/mnt"
  [[ "$iso_mounted" == 0 ]] || umount "$work/iso"
  [[ -z "$loop" ]] || losetup -d "$loop"
  rm -rf "$work"
}
trap cleanup EXIT
mkdir -p "$work/mnt" "$work/iso" "$work/initrd/proc" out/validation
cat > "$work/init.c" <<'C'
#include <sys/mount.h>
#include <sys/reboot.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void) {
 mount("proc","/proc","proc",0,0);
 char cmd[16384]={0}; FILE *f=fopen("/proc/cmdline","r"); if(f){fgets(cmd,sizeof cmd,f);fclose(f);}
 const char *args[]={"inst.ks=file:/glassboot/ks.cfg","auto=true","priority=critical","preseed/file=/preseed.cfg",
                     "autoyast=file:///autoinst.xml","autoinstall","ds=nocloud","glassboot.seed=/answers/cidata.img"};
 int ok=1;
 for(unsigned i=0;i<sizeof args/sizeof args[0];i++) if(!strstr(cmd,args[i])) {printf("MISSING_ARG %s\n",args[i]);ok=0;}
 const char *paths[]={"/glassboot/ks.cfg","/preseed.cfg","/autoinst.xml"};
 for(unsigned i=0;i<3;i++){char buf[64]={0};f=fopen(paths[i],"r");if(f){fgets(buf,sizeof buf,f);fclose(f);}
  if(strcmp(buf,"ANSWER_FIXTURE\n")){printf("MISSING_FILE %s\n",paths[i]);ok=0;}}
 puts(ok?"GB_UNATTENDED_PASS":"GB_UNATTENDED_FAIL"); fflush(stdout); sleep(1); reboot(RB_POWER_OFF);for(;;)pause();
}
C
gcc -O2 -static -s -o "$work/initrd/init" "$work/init.c"
(cd "$work/initrd"; find . | cpio --quiet -o -H newc) > "$work/test.cpio"
mount -o loop,ro "$iso" "$work/iso"; iso_mounted=1
kernel=$(find "$work/iso/boot" -maxdepth 1 -name 'vmlinuz-*' | head -1)
cp "$kernel" "$work/kernel"
umount "$work/iso"; iso_mounted=0
"$cli" install --image "$work/test.img" --size 1G --filesystem "${GLASSBOOT_TEST_FILESYSTEM:-exfat}" --payload out/payload > "$work/install.log"
loop=$(losetup --find --show --partscan "$work/test.img")
mount "${loop}p1" "$work/mnt"; mounted=1
cp "$work/kernel" "$work/mnt/kernel"; cp "$work/test.cpio" "$work/mnt/test.cpio"
dd if=/dev/zero of="$work/mnt/Test.iso" bs=1048576 count=1 status=none
mkdir "$work/mnt/answers"
for file in ks.cfg preseed.cfg autoinst.xml; do printf 'ANSWER_FIXTURE\n' > "$work/mnt/answers/$file"; done
printf '#cloud-config\nautoinstall:\n  version: 1\n' > "$work/user-data"
tr -d '\r' < scripts/make-autoinstall-seed.sh > "$work/make-seed.sh"
bash "$work/make-seed.sh" "$work/user-data" "$work/mnt/answers/cidata.img"
cat > "$work/mnt/glassboot/glassboot.cfg" <<'EOF'
kickstart /Test.iso = /answers/ks.cfg
preseed /Test.iso = /answers/preseed.cfg
autoyast /Test.iso = /answers/autoinst.xml
autoinstall /Test.iso = /answers/cidata.img
EOF
cat > "$work/mnt/glassboot/runtime.cfg" <<'EOF'
serial --speed=115200
terminal_output serial
if gb_hook_enable /Test.iso "$gb_partuuid" "$gb_res/glassboot-hook-x86_64.cpio" "($gb_data)"; then
  linux "($gb_data)/kernel" console=ttyS0,115200 panic=-1
  initrd "($gb_data)/test.cpio"
  boot
fi
EOF
umount "$work/mnt"; mounted=0; losetup -d "$loop"; loop=""
tr -d '\r' < scripts/qemu-script.sh > "$work/qemu.sh"
QEMU_MEM=512 QEMU_SERIAL="$PWD/out/validation/unattended-serial.log" \
  bash "$work/qemu.sh" bios "$work/test.img" wait:8 key:c wait:1 \
    'type:source ($gb_data)/glassboot/runtime.cfg' key:ret wait:15 || true
grep -q GB_UNATTENDED_PASS out/validation/unattended-serial.log
echo 'PASS: real Linux kernel received Kickstart, preseed, AutoYaST files and NoCloud kernel arguments'
