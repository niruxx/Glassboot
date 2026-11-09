# BIOS-USB

An open-source multi-ISO boot stick. Format a USB drive once, then copy `.iso` files onto
it and pick one from a menu at boot. It works with Legacy BIOS, UEFI (x86_64) and 32-bit UEFI.

It does the same job as [Ventoy](https://www.ventoy.net/), with one rule: **everything that
runs at boot is built from source in this repo or from pinned, signature-checked upstream
releases.** There are no opaque binaries. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

> **Status: early development.** Not ready for production use yet.

## What boots

| ISO type | Method | Status |
|---|---|---|
| ISOs that ship `loopback.cfg` (Ubuntu family, Mint, Arch, Kali, ...) | distro's own loop-boot menu | implemented |
| Ubuntu-style casper ISOs | `iso-scan/filename=` | implemented |
| Debian live | `findiso=` | implemented |
| Arch (archiso) | `img_dev=` / `img_loop=` | implemented |
| Fedora / openSUSE live (dracut) | `iso-scan/filename=` | implemented |
| Windows 10/11 installers | wimboot + WinPE helper | implemented, needs real-ISO testing |
| Any other Linux ISO | device-mapper hook | planned |
| BSDs, other EFI tools | UEFI virtual disk driver | planned |
| Secure Boot | shim + MOK | planned |

## Building (Windows + WSL2, or any Debian/Ubuntu)

```sh
# inside WSL (Ubuntu 24.04), from the repo directory
sudo scripts/install-deps.sh      # distro packages only
scripts/build.sh                  # GRUB + wimboot from verified source -> out/
```

## Making a test image and booting it in QEMU

```sh
sudo scripts/mkusb.sh --image out/biosusb.img --size 8G
sudo scripts/add-iso.sh out/biosusb.img ~/isos/*.iso
scripts/test-qemu.sh bios         # Legacy BIOS (SeaBIOS)
scripts/test-qemu.sh uefi         # UEFI (OVMF)
```

## Making a real USB stick

WSL can't see USB drives by default. Attach the stick with
[usbipd-win](https://github.com/dorssel/usbipd-win), then in an **admin** PowerShell run:

```powershell
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

Then, inside WSL:

```sh
lsblk                                   # find the stick, e.g. /dev/sdd
sudo scripts/mkusb.sh --device /dev/sdd # asks you to type the name to confirm
```

You can also write `out/biosusb.img` to the stick with any raw image writer. In that case
the data partition is only as big as the image.

When it's done, the stick shows up in Windows as a **BIOSUSB** drive. Drag ISO files onto
it, either at the top level or one folder deep.

## Repository layout

```
boot/grub/grub.cfg            menu: finds the partitions, lists the ISOs
boot/grub/biosusb-detect.cfg  per-ISO detection and boot methods
boot/win/                     WinPE helper injected by wimboot for Windows ISOs
scripts/                      deps, build, mkusb, QEMU test
versions.env                  pinned upstream versions and hashes
docs/ARCHITECTURE.md          design and roadmap
```
