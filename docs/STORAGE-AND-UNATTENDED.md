# Storage options and Linux unattended installs

## Linux answer files

Add one of these per ISO to `/glassboot/glassboot.cfg`:

```ini
kickstart /Fedora.iso = /answers/server.ks
preseed /Debian.iso = /answers/preseed.cfg
autoyast /SUSE.iso = /answers/autoinst.xml
autoinstall /Ubuntu.iso = /answers/cidata.img
```

Kickstart, preseed and AutoYaST files are added to the initrd with the corresponding
kernel arguments. Missing files stop the automatic boot path instead of silently
starting an interactive installer. Answer file syntax and target-disk selection are
the responsibility of the profile author.

Ubuntu uses a NoCloud VFAT seed disk so its files survive the live system's root
switch. Create it on Linux, then copy it onto the data partition:

```sh
bash scripts/make-autoinstall-seed.sh user-data cidata.img
# Or supply your own meta-data file:
bash scripts/make-autoinstall-seed.sh user-data cidata.img meta-data
```

The optional `meta-data` argument is a filename. If omitted, Glassboot supplies
an instance ID. `user-data` must contain a valid
cloud-config `autoinstall` profile. The seed is exposed read-only with the CIDATA
label, with `autoinstall ds=nocloud` on the command line. The helper refuses to
overwrite an existing output. It needs dosfstools and mtools.

The hooks apply in normal automatic Linux boot mode, including the ISO's own
GRUB/syslinux menu. Compatibility/loopback modes and Windows use separate paths.
A custom kernel test verifies file injection and arguments; complete unattended
installations with each distribution remain to be validated.

References: [NoCloud](https://docs.cloud-init.io/en/latest/reference/datasources/nocloud.html),
[Kickstart boot options](https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/7/html/installation_guide/sect-kickstart-howto),
[AutoYaST](https://documentation.suse.com/sles/15-SP5/html/SLES-all/Invoking.html).

## Data filesystems

exFAT remains the cross-platform default. The Linux GUI and CLI additionally offer
NTFS, ext4, XFS and UDF:

```sh
glassboot-cli install --image test.img --size 8G --filesystem ext4 --payload out/payload
```

Linux uses system formatter tools on a bounded loop view of the new partition, then
mounts it to create the `glassboot` files. Required packages: ntfs-3g, e2fsprogs,
xfsprogs or udftools, plus util-linux. XFS needs at least 300 MiB for data. Formatting
tools are checked before disk writes. The Windows installer currently formats only
exFAT; it can update existing supported layouts without reformatting their data.

Windows installer ISOs and native Windows VHD boots still need a host filesystem
that Windows itself can read; use exFAT or NTFS for those images. ext4/XFS data
partitions target Linux boot images.

GRUB reads these filesystems with its own drivers. For non-exFAT ISO/seed files it
records physical extents and includes them in the initrd, so the kernel does not
need the host filesystem driver merely to read the ISO. This scans the complete
file before boot and can take time on slow USB media. Sparse, compressed, resident
and excessively fragmented files are rejected when their mapping cannot be proven.
Do not modify the source image while booted from it.

Fragmented maps require `dm-mod`. For Alpine, Glassboot preloads the matching
module from the ISO's own `modloop-lts`; it also prepares storage before Alpine's
early media scan. A kernel filesystem mount provides a fallback when direct
mapping is unavailable, provided the required filesystem driver is in the initrd.

Writable persistence on these filesystems uses a kernel filesystem mount and a
file-backed loop to preserve COW/reflink semantics. It therefore needs the matching
filesystem driver in the distribution's initrd and distribution support for loop
persistence. exFAT retains its existing sector-mapping persistence path.

## Add Glassboot while preserving files

The CLI supports one existing MBR NTFS/exFAT partition starting at LBA 2048, with
MBR slots 2–4 empty and an unused boot gap. It adds the ESP as partition 2:

```sh
glassboot-cli install --image existing.img --preserve-data --payload out/payload
glassboot-cli install --device /dev/sdX --preserve-data --payload out/payload
```

This mode requires unallocated tail space and never formats, moves or shrinks the
existing filesystem. Tests compare every byte of the preserved data partition.
The volume label and disk signature are retained; GRUB uses partition 1 without
requiring a `GLASSBOOT` label. Create a `glassboot` folder there for configuration;
existing files are not seeded or changed. Windows ISO selection without a grubenv
file follows the existing interactive helper fallback.

On Linux, explicitly opt into NTFS shrinking when free tail space is unavailable:

```sh
glassboot-cli install --image existing.img --shrink-ntfs --payload out/payload
```

This uses `ntfsresize`, first in no-action mode, retaining its consistency,
hibernation and bad-sector checks. The filesystem is shrunk before its partition
entry is shortened. Windows filesystem checks should run afterward, as requested
by ntfsresize. Automatic exFAT shrinking, GPT adoption and occupied boot gaps are
not supported. These operations change boot code and partition metadata; keep a
backup before applying them to a real disk. Tests use disposable images only.

## 4Kn and large disks

GPT/UEFI installations support 512-byte and 4096-byte logical sectors. Installer
layout offsets remain in 512-byte internal units; GPT serializes physical-sector
LBAs. The payload includes a separately checksummed 4096-byte-sector FAT16 ESP
(`esp4k.img`), and exFAT uses the target sector size. Aligned I/O preserves adjacent
bytes when updating small metadata fields. The hook converts GPT LBAs to bytes
using the disk's actual logical sector size.

For a 4Kn image, explicitly specify its sector size on both install and update:

```sh
glassboot-cli install --image 4kn.img --size 8G --gpt --sector-size 4096 --payload out/payload
glassboot-cli update --image 4kn.img --sector-size 4096 --payload out/payload
```

Physical disks are detected automatically, and the GUI selects GPT for 4Kn drives.
Legacy BIOS and preserve-data adoption remain limited to 512-byte sectors. Old
payloads without `esp4k.img` cannot install/update 4Kn. Sparse 3 TiB layouts are
checked for both geometries. A sparse 3 TiB 4Kn image also passed filesystem checks,
hook lookup and update. QEMU/OVMF booted Alpine from 4Kn through NVMe; its emulated
4Kn USB path did not discover the GPT partitions. Real firmware/USB hardware and
Windows raw 4Kn writes remain untested.

## Validation commands

Build the payload and Linux installer first. As root, run:

```sh
ctest --test-dir /path/build --output-on-failure
bash Utility-Source/tests/storage-options-test.sh /path/build
bash Utility-Source/tests/filesystem-menu-test.sh /path/build
bash Utility-Source/tests/unattended-qemu-test.sh /path/build /path/Alpine-standard.iso
bash Utility-Source/tests/linux-image-boot-test.sh /path/build /path/Alpine-standard.iso
```

Logs are written under `out/validation`. Storage tests cover formatting and seeded
files for all five filesystems, updates, 4Kn filesystem checks and hook lookup,
byte-for-byte preservation and NTFS shrink-and-add. Menu tests compare GRUB's
generated extents against the original file and exercise a true 4096-byte logical
NVMe device. Transport tests boot a real kernel with injected files/arguments;
they do not execute destructive installation profiles. Alpine 3.24 reached its login
prompt from NTFS, ext4, XFS and UDF data partitions and from 4Kn exFAT through NVMe.
