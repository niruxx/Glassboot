# BIOS-USB architecture

BIOS-USB is a multi-ISO boot stick: you copy ISO files onto it and pick one from a menu
at boot. It does the same job as Ventoy, with one hard rule: **every byte that executes
at boot is built from auditable upstream or in-repo source by `scripts/build.sh`.** No
prebuilt binaries are committed or downloaded.

## Components

| Component | Source | License | Role |
|---|---|---|---|
| GRUB 2 (`i386-pc`, `x86_64-efi`, `i386-efi`) | ftp.gnu.org, SHA-256 + GPG verified | GPLv3+ | Boot menu, filesystem drivers (exFAT, NTFS, ISO9660, UDF), ISO loopback, Linux boot |
| wimboot | github.com/ipxe/wimboot, pinned commit | GPLv2+ | Boots a Windows `boot.wim` straight from the ISO and injects our helper files |
| `boot/grub/*.cfg` | this repo | — | ISO scanning and per-ISO boot logic |
| `boot/win/*` | this repo | — | WinPE helper that reattaches the ISO so Windows Setup can find `install.wim` |

The exact versions are pinned in [`versions.env`](../versions.env).

## Disk layout (MBR)

```
LBA 0          MBR: GRUB boot.img (BIOS) + partition table
LBA 1..2047    GRUB core.img (BIOS), embedded in the 1 MiB alignment gap
p1  exFAT  "BIOSUSB"      rest of the disk   ISO files, plus \biosusb\grubenv
p2  FAT16  "BIOSUSB_EFI"  64 MiB, type 0xEF  /EFI/BOOT/BOOTX64.EFI, BOOTIA32.EFI,
                                             /boot/grub (modules + cfg), /boot/biosusb
```

* **Legacy BIOS:** firmware runs the MBR, GRUB `boot.img`, `core.img`, then `/boot/grub` on p2.
* **UEFI:** firmware finds the type-0xEF partition and runs `/EFI/BOOT/BOOTX64.EFI` (GRUB).
* The data partition comes first so that every OS, including older Windows versions that
  only mount the first partition on removable media, shows it as the drive.

Both modes load the same `grub.cfg`.

## How an ISO is booted

`grub.cfg` finds every `*.iso` file in the top two folder levels of p1 and adds a menu
entry for each one. When you pick an entry, `bu_boot_iso` (in `biosusb-detect.cfg`)
mounts the ISO as `(loop)` and tries these methods in order:

1. **Windows** (`/sources/boot.wim`). wimboot loads `bootmgr`, BCD, `boot.sdi` and
   `boot.wim` directly from the ISO. It also injects `winpeshl.ini` and `biosusb.cmd`
   into `X:\Windows\System32`. GRUB first saves the ISO's path into
   `\biosusb\grubenv` on p1 with `save_env`. Once WinPE is up, `biosusb.cmd` finds p1,
   reads that path, attaches the ISO with `diskpart attach vdisk readonly`, and starts
   `setup.exe` from it.
2. **`/boot/grub/loopback.cfg`**. Many distros (Ubuntu and its flavours, Linux Mint, Arch,
   Kali and others) ship this file for exactly this purpose. We set `iso_path` and hand
   over to it.
3. **Known live layouts:** casper (`iso-scan/filename=`), Debian live-boot (`findiso=`),
   archiso (`img_dev=` + `img_loop=`), and dracut live for Fedora and openSUSE
   (`iso-scan/filename=`).
4. **Fallbacks:** the ISO's own `grub.cfg`, or, on UEFI, chainloading its `bootx64.efi`.
   This works for self-contained tools such as memtest86+.

## Roadmap

1. **Universal Linux boot (dm-linear).** Methods 2 and 3 only work when the distro's
   initrd knows how to loop-mount an ISO from a path. The universal fix is the same idea
   Ventoy uses, implemented in the open:
   * A small GRUB module records the ISO file's physical extents on the USB (GRUB can
     already produce blocklists).
   * It appends a tiny cpio "hook" initrd after the distro's initrd. GRUB concatenates
     initrds, and the kernel unpacks all of them.
   * The hook runs early and calls a statically built `dmsetup` to create a linear
     device-mapper target over those extents. It then exposes that device the way the
     distro expects (for example `/dev/sr0` or `by-label`), so the distro's normal
     "find the CD" logic succeeds.

   Requirement: the ISO file must be contiguous on disk. Copying an ISO onto a freshly
   formatted exFAT drive almost always gives this. We will check it at boot.
2. **UEFI virtual disk driver.** A small EFI driver that installs a `BlockIo` protocol
   over the ISO's extents, so an unmodified `bootx64.efi` on the ISO can read its own
   media. This covers BSDs, unusual distros and recovery tools.
3. **Windows ISO selection without `save_env`.** Generate a cpio entry in GRUB that
   holds the ISO path, so no writable file on the data partition is needed.
4. **Secure Boot:** shim plus our own key, enrolled once through MOK.
5. **Native Windows/macOS installer** for the stick. For now, WSL + usbipd is required.
