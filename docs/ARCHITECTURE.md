# Glassboot architecture

Glassboot is a multi-ISO boot stick: you copy ISO files onto it and pick one from a menu
at boot. It does the same job as Ventoy, with one hard rule: **every byte that executes
at boot is built from auditable upstream or in-repo source by `scripts/build.sh`.** No
prebuilt binaries are committed or downloaded.

## Components

| Component | Source | License | Role |
|---|---|---|---|
| GRUB 2 (`i386-pc`, `x86_64-efi`, `i386-efi`) | ftp.gnu.org, SHA-256 + GPG verified | GPLv3+ | Boot menu, filesystem drivers (exFAT, NTFS, ISO9660, UDF), ISO loopback, Linux boot |
| wimboot | github.com/ipxe/wimboot, pinned commit | GPLv2+ | Boots a Windows `boot.wim` straight from the ISO and injects our helper files |
| syslinux `memdisk` | kernel.org syslinux 6.03 tarball, SHA-256 verified, built with `make bios` | GPLv2+ | Legacy BIOS: boots `.img`/`.vhd` files and small ISOs as a RAM disk |
| `fonts/unicode.pf2` | GNU Unifont (distribution package), converted by our `grub-mkfont` | GPLv2+ with font exception | Font that ISO menus load with `loadfont unicode` |
| `boot/grub/*.cfg` | this repo | — | ISO scanning and per-ISO boot logic |
| `boot/win/*` | this repo | — | WinPE helper that reattaches the ISO so Windows Setup can find `install.wim` |
| `boot/grub-module/glassboot.c` | this repo | GPLv3+ | GRUB module: ISO scan, linux/initrd hook, virtual UEFI file system for wimboot |
| `hook/glassboot-hook.c` | this repo | GPLv3+ | Static early-boot helper that maps the ISO file as a block device |
| `win-helper/glassboot-attach.c` | this repo | GPLv3+ | WinPE helper that mounts the ISO as a virtual DVD |
| `Utility-Source/` | this repo | — | Qt 6 installer and CLI that write the payload to a USB drive |

The exact versions are pinned in [`versions.env`](../versions.env).

## Payload

`scripts/build-payload.sh` turns the GRUB build into four files in `out/payload/`. These
are the exact bytes every installer writes:

* `mbr.bin`: GRUB `boot.img`.
* `core.img`: GRUB `i386-pc` core image. It has a small embedded config that sets
  `prefix=(<boot drive>,msdos2)/boot/grub` and falls back to a search by the
  `GLASS_EFI` label.
* `esp.img`: a complete 64 MiB FAT16 ESP. It holds `BOOTX64.EFI` and `BOOTIA32.EFI`
  (built with `grub-mkimage -p /boot/grub`), the modules for all three platforms, our
  configs, `fonts/unicode.pf2`, wimboot, memdisk, the initrd hook and the WinPE helper.
* `manifest.txt`: SHA-256 checksums plus the layout constants.

The installer (`Utility-Source/`) checks those checksums, writes the files, formats exFAT
itself, and patches only these things:
* the `boot.img` drive-check bytes, the same way `grub-bios-setup` does
* the `core.img` blocklist (the rest of `core.img` starts at LBA 2)
* the ESP's hidden-sectors field and volume serial
* the MBR disk signature

So no `grub-install` is needed, and a stick made on Windows matches one made on Linux.

## Disk layout (MBR)

```
LBA 0          MBR: GRUB boot.img (BIOS) + partition table
LBA 1..2047    GRUB core.img (BIOS), embedded in the 1 MiB alignment gap
p1  exFAT  "GLASSBOOT"  rest of the disk   ISO files, plus \glassboot\grubenv
p2  FAT16  "GLASS_EFI"  64 MiB, type 0xEF  /EFI/BOOT/BOOTX64.EFI, BOOTIA32.EFI,
                                           /boot/grub (modules + cfg), /boot/glassboot
```

* **Legacy BIOS:** firmware runs the MBR, GRUB `boot.img`, `core.img`, then `/boot/grub` on p2.
* **UEFI:** firmware finds the type-0xEF partition and runs `/EFI/BOOT/BOOTX64.EFI` (GRUB).
* The data partition comes first so that every OS, including older Windows versions that
  only mount the first partition on removable media, shows it as the drive.

Both modes load the same `grub.cfg`.


## How an ISO is booted

### Finding ISOs

`grub.cfg` loads our GRUB module (`boot/grub-module/glassboot.c`, compiled into GRUB by
`scripts/build.sh`). Its `gb_scan` command searches the whole data partition for bootable
files, recursing up to 8 folders deep. It skips `System Volume Information`,
`$RECYCLE.BIN`, dot-folders and `glassboot/`. It sorts the results, root files first,
and publishes them as a menu script at `(proc)/glassboot_menu`, one entry per file with
its size:

| File | Menu entry calls | Firmware |
|---|---|---|
| `*.iso` | `gb_boot_iso`: mounts it as `(loop)` (below) | BIOS + UEFI |
| `*.wim` | `gb_boot_wim`: wimboot boots the WinPE image as-is | BIOS + UEFI |
| `*.efi` | `gb_boot_efi`: chainload | UEFI |
| `*.img`, `*.vhd` (fixed) | `gb_boot_img`: virtual disk (below) on UEFI; `linux16 memdisk harddisk raw` on BIOS | BIOS + UEFI |

Before scanning, `gb_config` reads `\glassboot\glassboot.cfg` ([CONFIG.md](CONFIG.md)).
Global settings become `gb_cfg_*` variables that `grub.cfg` applies: timeout, default
entry (each entry has a stable `--id "gb:<path>"`), theme and password (`superusers` plus
`password_pbkdf2`). Per-file settings are kept in the module: `gb_scan` uses aliases and
hidden paths, and `gb_hook_enable` uses extra arguments and persistence files. With
`view = tree`, each folder becomes a `submenu` (`--id "gbdir:<path>"`).

### Virtual CD / disk on UEFI (`gb_efi_vdisk_boot`)

For ISOs with no boot menu GRUB can read (BSDs and other non-Linux systems), and for
`.img`/`.vhd` files, the module installs a read-only **`EFI_BLOCK_IO_PROTOCOL`** device
backed by the file on the data partition. ISOs get 2048-byte sectors, disk images get
512. It then calls `ConnectController`, so the firmware's **own** drivers bind to it:
the partition driver parses El Torito, MBR or GPT, and the FAT driver mounts the EFI
system partition. Finally it loads `\EFI\BOOT\BOOTX64.EFI` (or `BOOTIA32.EFI`) from that
file system. The image's boot loader can keep reading through the device, with GRUB
serving the reads, until `ExitBootServices`. Fixed-size VHDs are recognised by their
`conectix` footer; dynamic VHDs and VHDX are refused with a message.

Limit: the *kernel* of a non-Linux system still has to find its root file system after
`ExitBootServices`. FreeBSD's loader and kernel boot this way, but the kernel stops at
`mountroot>` (see the roadmap).

### Injection

* **Linux:** `gb_hook_enable` gets the data device and appends to every `initrd`
  command, after the hook, every file in `\glassboot\inject\linux\` and then
  `<iso path>.cpio` if that file exists. The kernel unpacks them in order, so later
  archives overwrite files from earlier ones (cpio, optionally compressed).
* **Windows:** every file in `\glassboot\inject\windows\` is passed to wimboot
  (`gb_wimboot_efi --inject DIR` on UEFI, `newc:` entries on BIOS) and lands in
  `X:\Windows\System32`. If one of them is `glassboot-user.cmd`, the WinPE helper runs it
  right before Setup, passing the ISO drive and the data drive. Every `.inf` under
  `\glassboot\drivers\` is loaded with `drvload` before the ISO is mounted.

### ISOs

Picking an ISO calls `gb_boot_iso PATH` (in `glassboot-detect.cfg`), which mounts the
ISO as `(loop)`.

### Windows ISOs (`/sources/boot.wim`)

Picking a Windows ISO opens an options menu (`glassboot-windows.cfg`):
* plain **Windows Setup**;
* **skip the Windows 11 hardware checks** (TPM, Secure Boot, CPU, RAM, storage);
* **skip the checks and allow a local account** (no Microsoft account or internet);
* **one entry per answer file** in `\glassboot\unattend\*.xml`.

GRUB saves the ISO path and the chosen options in `\glassboot\grubenv` on p1 with
`save_env`, then starts wimboot. wimboot boots the ISO's `boot.wim` (WinPE) and injects
our files into `X:\Windows\System32`: `winpeshl.ini`, `glassboot.cmd`,
`glassboot-attach.exe` and `glassboot-localacct.xml`.

* **Legacy BIOS:** `linux16 wimboot` + `initrd16 newc:...`, wimboot's normal mode.
* **UEFI:** upstream GRUB can't pass files to wimboot (it no longer has the EFI handover
  entry). So the GRUB module's `gb_wimboot_efi` does what iPXE does. It installs a
  read-only **virtual UEFI file system** (`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`) whose root
  directory serves BCD, `boot.sdi`, `boot.wim`, `bootx64.efi` and our files, read from
  the ISO on demand. It then starts wimboot with that as its device. wimboot and Windows
  Boot Manager read through it until `ExitBootServices`.

In WinPE, `glassboot.cmd`:
1. finds p1 and reads the options;
2. mounts the ISO as a read-only virtual DVD with **`glassboot-attach.exe`**
   (`win-helper/`, our C source built with MinGW). That uses the Virtual Disk API
   (`AttachVirtualDisk`, ISO storage type), because `diskpart`'s vdisk commands only
   handle VHD/VHDX;
3. sets the `HKLM\SYSTEM\Setup\LabConfig` `Bypass*` values if the bypass was chosen;
4. starts `sources\setup.exe`, with `/unattend:` when an answer file or the
   local-account option was chosen.

**Tool mode** (WinPE discs with no `install.wim`, such as Hiren's BootCD PE): the disc's
own start-up has to run, because it sets up the desktop. So GRUB passes
`winpeshl-tool.ini`, which runs three steps in order:
1. `glassboot.cmd mountonly`, minimised: mounts the ISO, reads the disc's original
   `winpeshl.ini` from its `boot.wim` (DISM, read-only), runs the user hook, then writes
   a marker file;
2. `wpeinit`;
3. `glassboot.cmd toolfinish`: waits for the marker, then runs the disc's original
   `LaunchApps` entries, or its `startnet.cmd`.

On Legacy BIOS, wimboot loads `boot.wim` into memory below 4 GB, so very large images
(Hiren's, 1.9 GB) don't start there.

### Everything else: the ISO's own boot menu, plus the Glassboot hook

This is how Glassboot boots ISOs it knows nothing about, the same idea as Ventoy, written
in the open:

1. `gb_hook_enable` (GRUB module) wraps GRUB's `linux`/`initrd` commands (and the
   `linuxefi`/`initrdefi`/`linux16`/`initrd16` variants). From then on, every kernel
   started gets these extra arguments:

       rdinit=/glassboot/init glassboot.iso=/path/to/file.iso glassboot.part=<disksig>-01
       live-media=/dev/glassboot

   (`live-media` is for Debian live-boot and Ubuntu casper, whose device scans skip
   loop devices; other distributions ignore it.)

   Every `initrd` also gets `glassboot-hook-x86_64.cpio` appended, a ~0.8 MB static
   binary. The kernel unpacks every initrd it's given, so the hook lands next to the
   distribution's files.
2. The ISO's **own, unmodified** boot menu runs. Glassboot tries `grub.cfg` first, then
   its isolinux/syslinux config (through GRUB's `syslinux_configfile`), then
   `loopback.cfg`, then, on UEFI, the virtual CD. `iso_path` is emptied while the ISO's
   `grub.cfg` runs, because some (Debian live-build) add `findiso=${iso_path}`, which
   would send the distribution looking for the ISO file instead of using our device.
3. The kernel starts `/glassboot/init` (`hook/glassboot-hook.c`) as PID 1. It forks a
   worker and immediately `exec`s the distribution's real `/init`, so boot continues as
   normal. The worker runs in a **private mount namespace**, so the distribution never
   sees its mounts. It:
   * waits for the disk whose MBR signature matches `glassboot.part`; it appears once
     the distribution loads its USB drivers;
   * reads the exFAT file system directly, finds the ISO, and follows its cluster chain;
   * maps those sectors as a read-only block device. For a contiguous file (the normal
     case) it uses a **loop device** with an offset and size limit on the USB disk. The
     generic udev rules in every distribution create `/dev/disk/by-label|by-uuid` links
     for loop devices, and a loop device doesn't hold the disk exclusively, so the
     distribution can still mount the data partition. A **fragmented** file gets a
     **device-mapper linear** table instead, which stitches the pieces together. Its
     resume event carries libdevmapper's `DM_UDEV_PRIMARY_SOURCE` cookie flag, so lvm2's
     udev rules treat it as genuine;
   * links **`/dev/glassboot`** to the device;
   * sends udev `change` events for 30 seconds, so `/dev/disk/by-label` and similar
     links appear even if udev starts later than the hook.

   The distribution then finds its media (by label, UUID or by scanning devices) as if
   it had been burned to a disc. Everything is logged to the kernel log:
   `dmesg | grep glassboot`.

   **Persistence:** if `glassboot.cfg` gives the ISO a `persist` file, GRUB also passes
   `glassboot.persist=PATH`. The worker then maps that file **read-write** as
   `/dev/glassboot-persist`, always through **device-mapper**, because live-boot and
   casper skip loop devices when they look for persistence. Many initramfs images (e.g.
   Arch's) have no lvm2 udev rules, so udev never labels a dm device. The worker therefore
   reads the ext superblock and creates `/dev/disk/by-label/<label>` and
   `/dev/disk/by-uuid/<uuid>` itself, which archiso (`cow_label=`) and dracut wait for.
   The dm table holds the data partition, so the live system can't mount the `GLASSBOOT`
   drive while persistence is in use.

The main menu's **"More boot modes"** submenu offers alternatives for difficult ISOs:
* **ISO as a virtual CD** (UEFI): skip the ISO's GRUB/syslinux menus and start its own EFI
  loader with the ISO presented as a CD (see above).
* **ISO's `loopback.cfg`:** runs `loopback.cfg` first, so the distribution locates the
  ISO file itself (its initrd needs an exFAT driver for that).
* **Compatibility mode:** the Phase 0 methods. These are known layouts with kernel
  arguments that tell the distribution where the ISO file is (casper `iso-scan`,
  live-boot `findiso`, archiso `img_loop`, dracut `iso-scan`), with no hook.
* **ISO in RAM** (BIOS): memdisk loads the whole ISO into memory and emulates a CD.

## Other menu tools

* **Boot an installed operating system** (`gb_scan_os`, F4). On **UEFI** it works through
  the firmware rather than GRUB's disk list, because firmware in fast-boot mode connects
  only the boot device, and GRUB builds its disk list once at start-up. The module runs
  `ConnectController` on every handle. It then checks each `EFI_SIMPLE_FILE_SYSTEM` that
  isn't on the Glassboot drive for `\EFI\Microsoft\Boot\bootmgfw.efi` and
  `\EFI\<vendor>\{shim,grub,systemd-boot}x64.efi`, falling back to `\EFI\BOOT\BOOTX64.EFI`.
  A choice starts with `LoadImage(device path + file)`, as in the firmware's boot menu
  (`gb_efi_boot_os`). On **BIOS** every other disk with a boot signature is offered:
  `drivemap` swaps it to the first BIOS disk, then `chainloader +1` starts it.
* **Checksums** (F5): GRUB's `hashsum` over the file on the data partition.

## Installer and updates

The installer writes a marker sector at LBA 2047, the last sector of the boot gap
(`GLASSBOOT-MARKER`, version, ESP size). It uses the marker to recognise a Glassboot
drive. **Update** rewrites only the ESP, `core.img`, the marker and the MBR boot code. It
keeps the partition table, the disk signature (so `glassboot.part` doesn't change) and
the whole data partition with the user's ISOs. Drives made before the marker existed are
recognised by their layout and the `GLASS_EFI` label.

See [ROADMAP.md](../ROADMAP.md) for what comes next.
