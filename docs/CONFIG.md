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
| `view = list` / `tree` | `list` shows every file with its folder path; `tree` shows folders as sub-menus. |
| `theme = PATH` | A [GRUB theme](https://www.gnu.org/software/grub/manual/grub/html_node/Theme-file-format.html). Every `.pf2` font in the theme's folder is loaded. |
| `password = HASH` | Every menu entry asks for user `glassboot` and this password. Set it with **Settings… > Password** in the installer, or create the hash with `grub-mkpasswd-pbkdf2`. |

## Per-file settings

| Setting | Effect |
|---|---|
| `alias PATH = NAME` | Show `NAME` in the menu instead of the file path. |
| `hide PATH` | Leave a file, or a whole folder, out of the menu. |
| `args PATH = ARGS` | Add kernel arguments when booting this Linux ISO (on top of the ISO's own). |
| `persist PATH = FILE` | Give this Linux ISO a persistence file (see below). |

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

## Folders Glassboot reads

| Folder / file | Effect |
|---|---|
| `glassboot\glassboot.cfg` | this configuration |
| `glassboot\inject\linux\*` | cpio archives added to every Linux ISO's initrd |
| `<iso file>.cpio` (next to the ISO) | cpio archive added to that ISO only |
| `glassboot\inject\windows\*` | files copied into WinPE's `System32`; `glassboot-user.cmd` runs before Setup |
| `glassboot\drivers\**\*.inf` | Windows drivers loaded in WinPE before Setup |
| `glassboot\unattend\*.xml` | answer files offered in the Windows options menu |

## Built-in tools

* **Boot an installed operating system...** (**F4**): finds Windows Boot Manager and
  Linux boot loaders on the computer's other disks (UEFI), or offers each other bootable
  disk (BIOS).
* **Tools > Verify a file's checksum** (**F5**): SHA-256, SHA-512, SHA-1 or MD5 of any
  image on the drive, to compare with the checksum its publisher lists.
* **More boot modes** (**F6**): other ways to start an ISO that doesn't boot normally.
