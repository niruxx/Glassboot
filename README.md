# Glassboot

An open-source multi-ISO boot stick. Format a USB drive once, then copy `.iso` files onto
it and pick one from a menu at boot. It works with Legacy BIOS, UEFI (x86_64) and 32-bit UEFI.

It does the same job as [Ventoy](https://www.ventoy.net/), with one rule: **everything that
runs at boot is built from source in this repo or from pinned, signature-checked upstream
releases.** There are no opaque binaries. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

> **Status: early development.** Not ready for production use yet.

## What boots

Copy files anywhere on the `GLASSBOOT` drive (any folder depth) and they appear in the menu.

| Image | How | Tested in QEMU |
|---|---|---|
| **Linux live / installer ISOs** | The ISO's own boot menu runs unchanged; the Glassboot hook maps the ISO file as a disk for the distribution | UEFI: Alpine 3.24, Arch 2026.11, Debian Live 13, Fedora KDE 44. BIOS: Debian Live 13 |
| **Windows installers** | wimboot + WinPE helper; options: skip Windows 11 hardware checks, local account, unattended `.xml` files | Windows 11 24H2 and Windows Server 2025 reach Setup on UEFI and BIOS; bypass verified on a VM without TPM |
| **WinPE tool discs** (Hiren's BootCD PE and similar) | wimboot; the disc's own start-up runs with the ISO mounted | Hiren's BootCD PE desktop on UEFI. On BIOS, very large `boot.wim` files (Hiren's, 1.9 GB) don't fit |
| **Windows PE images (`.wim`)** | wimboot | Windows 11 `boot.wim` on UEFI |
| **Other ISOs** (BSDs, other OSes, tools) on UEFI | The ISO is presented to the firmware as a virtual CD and its own EFI loader starts | FreeBSD 15.1: loader and kernel boot; the kernel can't mount its root yet (stops at `mountroot>`) |
| **Disk images (`.img`, fixed `.vhd`)** | UEFI: virtual disk, the image's EFI loader starts. BIOS: memdisk RAM disk | FreeBSD 15.1 memstick on both |
| **`.efi` tools** (UEFI) | Chainloaded | yes |
| Fallbacks | "More boot modes" (F6): the ISO's `loopback.cfg`, compatibility mode (casper `iso-scan`, live-boot `findiso`, archiso `img_loop`, dracut), the virtual CD (UEFI), or the ISO in RAM via memdisk (BIOS) | |
| BSD root file systems, VHDX, Secure Boot | | planned, see [ROADMAP.md](ROADMAP.md) |

**Menu extras:** boot an installed OS from the computer's own disks (F4), checksum any
image (F5), password protection, GRUB themes, and Linux persistence files.

**Settings** go in `glassboot\glassboot.cfg`: default entry, timeout, tree or list view,
menu names, hidden files, kernel arguments, persistence, password and theme. Edit it with
**Settings…** in the installer, or with a text editor; see [docs/CONFIG.md](docs/CONFIG.md).
Folders on the `GLASSBOOT` drive customise boots further:

| Folder / file | Effect |
|---|---|
| `glassboot\inject\linux\*` | cpio archives added to every Linux ISO's initrd |
| `<iso file>.cpio` (next to the ISO) | cpio archive added to that ISO only |
| `glassboot\inject\windows\*` | files copied into WinPE's `System32`; `glassboot-user.cmd` runs before Setup |
| `glassboot\drivers\**\*.inf` | Windows drivers loaded in WinPE before Setup |
| `glassboot\unattend\*.xml` | answer files offered in the Windows options menu |

## Building (Windows + WSL2, or any Debian/Ubuntu)

```sh
# inside WSL (Ubuntu 24.04), from the repo directory
sudo scripts/install-deps.sh      # distro packages only
scripts/build.sh                  # GRUB (+ glassboot module), wimboot, hook, WinPE helper -> out/
scripts/build-payload.sh          # boot files the installer writes -> out/payload/
```

## Making a USB stick

Use the **Glassboot Installer** (Qt 6, Windows and Linux) in [Utility-Source/](Utility-Source/).
Pick the drive and click *Install*. It partitions, formats and installs the boot code itself,
then verifies everything it wrote. See [Utility-Source/README.md](Utility-Source/README.md)
for how to build it.

There is also a command-line version that works the same way:

```sh
sudo scripts/mkusb.sh --device /dev/sdX   # Linux; asks you to type YES
glassboot-cli.exe install --device \\.\PhysicalDrive3   # Windows, admin shell
glassboot-cli update --device /dev/sdX                  # refresh boot files, keep ISOs
```

When it's done, the stick shows up as a **GLASSBOOT** drive. Drag ISO files onto it, into
any folder. To upgrade Glassboot later, use **Update** in the installer: your ISOs stay.

## Testing in QEMU

```sh
sudo scripts/mkusb.sh --image out/glassboot.img --size 8G
sudo scripts/add-iso.sh out/glassboot.img ~/isos/*.iso
scripts/test-qemu.sh bios         # Legacy BIOS (SeaBIOS)
scripts/test-qemu.sh uefi         # UEFI (OVMF)
scripts/screenshot.sh uefi out/glassboot.img shot.png 12   # headless
```

## Repository layout

```
boot/grub/grub.cfg               menu: finds the partitions, lists the ISOs
boot/grub/glassboot-detect.cfg   per-ISO boot logic (hook, Windows, compatibility modes)
boot/grub/glassboot-windows.cfg  Windows options menu
boot/grub-module/glassboot.c     GRUB module: ISO scan, linux/initrd hook, UEFI virtual FS
hook/glassboot-hook.c            early-boot helper that maps the ISO file as a disk
boot/win/                        WinPE helper script + answer file injected by wimboot
win-helper/glassboot-attach.c    WinPE tool that mounts the ISO as a virtual DVD
scripts/                         deps, build, payload, mkusb, QEMU test drivers
Utility-Source/                  Qt 6 installer app + glassboot-cli
versions.env                     pinned upstream versions and hashes
docs/ARCHITECTURE.md             design
ROADMAP.md                       feature plan vs Ventoy and others
```
