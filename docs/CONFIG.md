# Configuring Glassboot

Glassboot works with no configuration: copy files onto the `GLASSBOOT` drive and boot.
To change how the menu looks or how particular images boot, use **Settings…** in the
Glassboot Installer (select the drive first), or create **`\glassboot\glassboot.cfg`** on
the drive with any text editor. The installer edits the same file and keeps your comments.
(`glassboot-installer --settings E:\` opens only the settings, for a drive mounted at `E:\`.)

```
# Lines starting with # are comments. One setting per line.

# ---- Menu
timeout = 10                       # seconds before the default entry boots (-1 = wait)
default = /Linux/ubuntu-24.04.iso  # entry selected when the menu opens
view    = tree                     # "tree": folders become sub-menus. Default: "list"
theme   = /glassboot/themes/mine/theme.txt   # GRUB theme (fonts: *.pf2 next to it)
password = grub.pbkdf2.sha512.10000.ABCD...  # ask for a password before booting anything

# ---- Per file (paths on the GLASSBOOT drive; \ or /; not case-sensitive)
alias   /Linux/ubuntu-24.04.iso = Ubuntu 24.04 LTS Desktop
hide    /Archive                   # leave a folder (or one file) out of the menu
args    /Linux/ubuntu-24.04.iso = persistent quiet splash
persist /Linux/ubuntu-24.04.iso = /glassboot/persistence/ubuntu.dat
```

> Comments must be on their own line: `timeout = 10   # seconds` would make the value
> `10   # seconds`. (The example above shows them inline only for brevity.)

## Menu settings

| Setting | Effect |
|---|---|
| `timeout = N` | Boot the default entry after N seconds. `-1` (the default) waits forever. |
| `default = PATH` | Pre-select this file's entry (works in list and tree view). |
| `language = en` / `es` | Boot-menu language (English by default). The installer follows the system language; Spanish is included. Technical diagnostics may remain in English. |
| `view = list` / `tree` | `list` shows every file with its folder path; `tree` shows folders as sub-menus. |
| `theme = PATH` | A [GRUB theme](https://www.gnu.org/software/grub/manual/grub/html_node/Theme-file-format.html). Every `.pf2` font in the theme's folder is loaded. |
| `password = HASH` | Every menu entry asks for user `glassboot` and this password. Set it with **Settings… > Password** in the installer, or create the hash with `grub-mkpasswd-pbkdf2`. |

## Per-file settings

| Setting | Effect |
|---|---|
| `class PATH = NAME` | Override the entry's GRUB theme class, e.g. `class /Linux/debian.iso = debian`. Put the matching `debian.png` in the theme's `icons` folder and enable the theme's icon display. Without an override, entries use `iso`, `efi`, `disk` or `windows`. |
| `alias PATH = NAME` | Show `NAME` in the menu instead of the file path. |
| `hide PATH` | Leave a file, or a whole folder, out of the menu. |
| `args PATH = ARGS` | Add kernel arguments when booting this Linux ISO (on top of the ISO's own). |
| `persist PATH = FILE` | Give this Linux ISO a persistence file (see below). |
| `dud PATH = FILE` | Give this Linux installer a driver update disk (see below). May be repeated. |

## Persistence (keep changes across reboots)

Live Linux ISOs normally forget everything when you shut down. A **persistence file** is
an image file holding a file system that the live system uses to store changes. Glassboot
maps it read-write as a disk, so the distribution finds it by its *label*, like a
partition. Each distribution expects a particular label and kernel argument:

| Distribution family | File system label | `args` to add | Extra | Tested |
|---|---|---|---|---|
| Debian live, Kali (live-boot) | `persistence` | `persistence` | file `persistence.conf` containing `/ union` | ✅ Debian 13 |
| Arch (archiso) | any, e.g. `arch-rw` | `cow_label=arch-rw` | — | ✅ Arch 2026.11 |
| Ubuntu, Mint, Pop!_OS (casper) | `casper-rw` | `persistent` | — | not yet |

**Easiest:** in the installer, open **Settings… > Images**, select the ISO, and click
**Create persistence file…**. Choose the distribution family and a size, then click
**Save**. This works on Linux. On Windows the button is disabled, because the file holds
a Linux (ext4) file system; create it on Linux once, after which the drive works anywhere.

By hand on Linux (example: 4 GB for Ubuntu):

```sh
scripts/make-persistence.sh casper-rw 4G /media/$USER/GLASSBOOT/glassboot/persistence/ubuntu.dat
```

Then add to `glassboot.cfg`:

```
persist /Linux/ubuntu-24.04.iso = /glassboot/persistence/ubuntu.dat
args    /Linux/ubuntu-24.04.iso = persistent
```

How it works: the file is mapped through device-mapper (live-boot and casper ignore loop
devices when they look for persistence), and Glassboot adds the `/dev/disk/by-label` link
that archiso and dracut wait for. The file may be fragmented. While a persistence file is
in use, the live system can't also mount the `GLASSBOOT` drive itself.

## Driver update disks (Linux installers)

Installers based on Anaconda (Fedora, RHEL, Rocky, AlmaLinux, CentOS Stream) accept
*driver update disks*: an ISO or RPM with extra drivers, e.g. for a RAID or network
controller the installer doesn't support. Keep them in `\glassboot\dud\` (that folder
doesn't appear in the menu) and add one line per disk:

```
dud /Linux/Rocky-9-x86_64-dvd.iso = /glassboot/dud/megaraid-dd.iso
```

Glassboot adds the file to the installer's initramfs and passes
`inst.dd=/glassboot-dud/0.iso`, so it is applied at once, without a second USB drive.
Tested with Fedora 44 netinst. (Kernel modules for other distributions can be added with
`glassboot\inject\linux\`.)

## Disk images and Windows in a VHD

`.img`, `.vhd` and `.vhdx` files are listed in the menu like ISOs:

| Image | UEFI | Legacy BIOS |
|---|---|---|
| raw `.img`, fixed `.vhd` | the image's own EFI boot loader starts (virtual disk) | loaded into RAM (memdisk) |
| dynamic `.vhd`, `.vhdx` | same | not supported |
| **Windows installed in a `.vhd`/`.vhdx`** | native VHD boot | native VHD boot |

**Windows in a VHD/VHDX** ("Windows To Go" style) starts directly from the file on the
`GLASSBOOT` drive and keeps running from it, read-write. Requirements:

* Windows 10 version 1809 or later inside (older versions can't use a VHD on exFAT).
* Windows must have completed its **first start** once elsewhere: create the VHD in a
  virtual machine such as Hyper-V, or apply an image and boot it once with `bcdboot`.
  A freshly applied, never-started image can't finish its first start from Glassboot
  (Windows looks for the computer's system partition then), and Glassboot says so
  instead of starting it.
* A dynamic VHD grows to its full size when Windows starts from it, so leave that much
  free space on the drive.
* The path may contain only ASCII characters and be at most 259 characters long.

Differencing VHDs (with a parent) aren't supported.

VHDX requires 512-byte logical sectors and a cleanly detached image. Images with
unreplayed logs are refused; attach and cleanly detach them in Windows first.
The virtual disk is read-only and is available to the firmware/boot loader only.
An operating system needs its own way to retain access after firmware services end.

Stock FreeBSD ISOs and raw UFS memstick images instead preload the complete image
into RAM as an `md` root. This requires enough memory for the image and operating
system and bypasses FreeBSD's loader menu. Other BSDs, custom loader module lists,
ZFS roots and FreeBSD inside VHD/VHDX remain unsupported by this RAM-root path.
See [disk image validation](DISK-IMAGE-VALIDATION.md) for coverage and commands.

## Folders Glassboot reads

Linux unattended answer files can be selected per ISO with `kickstart`, `preseed`,
`autoyast` and `autoinstall`. The latter selects a NoCloud CIDATA disk image.
See [storage and unattended installs](STORAGE-AND-UNATTENDED.md) for examples,
filesystem options, preserve-data installs and 4Kn coverage.

| Folder / file | Effect |
|---|---|
| `glassboot\glassboot.cfg` | this configuration |
| `glassboot\inject\linux\*` | cpio archives added to every Linux ISO's initrd |
| `<iso file>.cpio` (next to the ISO) | cpio archive added to that ISO only |
| `glassboot\inject\windows\*` | files copied into WinPE's `System32`; `glassboot-user.cmd` runs before Setup |
| `glassboot\drivers\**\*.inf` | Windows drivers loaded in WinPE before Setup |
| `glassboot\unattend\*.xml` | answer files offered in the Windows options menu |
| `glassboot\dud\` | a place for Linux driver update disks (not listed in the menu) |

## Built-in tools

* **Boot an installed operating system...** (**F4**): finds Windows Boot Manager and
  Linux boot loaders on the computer's other disks (UEFI), or offers each other bootable
  disk (BIOS).
* **Tools > Verify a file's checksum** (**F5**): SHA-256, SHA-512, SHA-1 or MD5 of any
  image on the drive, to compare with the checksum its publisher lists.
* **Compatibility / alternate boot options** (**F6**), directly below the image list:
  select another boot method, then select the same ISO from the discovered files.
  On UEFI, the first option starts the ISO's own EFI loader as a virtual CD,
  including Windows ISOs. Other options retain GRUB/Syslinux menus with the hook,
  loopback.cfg, and known-distribution compatibility boot. BIOS offers memdisk.

Automatic boot on UEFI prefers EFI-specific GRUB menus, then generic GRUB menus,
then loopback.cfg, then the native EFI loader instead of a BIOS-oriented Syslinux
menu. Windows retains its UEFI wimboot setup options and FreeBSD its RAM-root path.
The USB's MBR/GPT partition layout does not select the firmware boot mode. To use
EFI loaders, start the USB's UEFI entry in the computer's firmware boot menu;
a running BIOS GRUB cannot switch the computer to UEFI.

## Search

Press **F3**, select the search entry, then type part of a filename, folder path or
menu alias and press Enter. Matching ignores ASCII letter case and treats the input
as literal text (no wildcards or regular expressions). An empty query shows all files.
Hidden files remain hidden. Results keep the configured list/tree view; Escape returns
to the previous menu. Search does not change `glassboot.cfg`.
