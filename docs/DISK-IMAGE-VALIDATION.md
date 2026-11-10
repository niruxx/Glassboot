# Disk image and FreeBSD boot validation

The disk reader exposes raw images, fixed/dynamic VHD and fixed/dynamic VHDX
read-only through GRUB's `(gbvd)` device and the UEFI virtual disk. Dynamic VHD
sector bitmaps are honored; unallocated sectors read as zeros. Container offsets,
VHD footer/header checksums, VHDX header/region CRCs and table coverage are checked.
The second VHDX region table is used if the first copy is corrupt.

Differencing images, VHDX partially present blocks, unreplayed VHDX logs and VHDX
4096-byte logical sectors are rejected. Attach and cleanly detach a dirty VHDX in
Windows before retrying. Generic dynamic containers still cannot use BIOS memdisk.
The existing Windows native VHD boot route is separate; these reader checks do not
establish that Windows successfully boots or supports a particular host filesystem.

FreeBSD images with `/boot/kernel/kernel` and `/boot/loader.conf` now have a RAM-root
route. ISO images use `cd9660:/dev/md0`; raw memstick images probe GPT partitions
1–8 and MBR slices 1–4, including their first BSD-label partition, for UFS roots.
GRUB preloads the complete image as `type=md_image`. Firmware ACPI pointers are
passed through `acpi.rsdp` and the older compatibility hint, needed on UEFI.

This requires enough RAM for the complete image, kernel and running system. It
targets stock FreeBSD GENERIC media, bypasses the FreeBSD loader menu and does not
interpret custom `loader.conf` module lists. Other BSDs, ZFS roots and FreeBSD roots
inside VHD/VHDX containers remain unfinished. Explicit alternate ISO modes retain
the original EFI-loader path.

## Reproduce

On Linux with GCC, qemu-img and QEMU installed, from the repository root:

```sh
python3 Utility-Source/tests/vimg-test.py
bash Utility-Source/tests/freebsd-qemu-test.sh /path/FreeBSD-bootonly.iso /path/FreeBSD-mini-memstick.img bios
bash Utility-Source/tests/freebsd-qemu-test.sh /path/FreeBSD-bootonly.iso /path/FreeBSD-mini-memstick.img uefi
```

The reader test extracts the actual production C functions, compiles them with
ASan/UBSan, and compares every virtual byte against a known raw image, including
unaligned reads across block boundaries. Negative fixtures cover VHD checksum
failure, dirty VHDX logs and both corrupt region tables; a single corrupt region
copy must recover. A VHD bitmap fixture retains stale payload bytes while clearing
the sector-present bit, and verifies zero output.

The boot test requires the built GRUB tools/modules in `out/grub`, xorriso, fonts-unifont
and OVMF for UEFI. It creates temporary rescue media and attaches no host disks.
It runs production boot functions with serial console enabled, checks the selected
RAM root and requires the FreeBSD welcome prompt without `mountroot>` or a panic.
Logs are written to `out/validation/freebsd-{iso,img}-{bios,uefi}.log`.

Validated with FreeBSD 15.1 amd64 bootonly and mini-memstick media in QEMU.
The memstick fixture uses an MBR slice with a BSD label; GPT probing is implemented
but has not been validated with a real GPT FreeBSD image. Physical USB hardware,
other FreeBSD releases and a full native Windows VHD boot were not tested here.

References: [GRUB image preloading](https://www.gnu.org/software/grub/manual/grub/html_node/Loopback-booting.html),
[FreeBSD memory disks](https://docs.freebsd.org/en/books/handbook/disks/),
[FreeBSD ACPI environment change](https://reviews.freebsd.org/D35634).
