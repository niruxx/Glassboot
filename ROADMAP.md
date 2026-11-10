# Glassboot roadmap

The goal is a drive you treat exactly like a Ventoy stick: install once, then copy ISO
files onto the `GLASSBOOT` partition and boot them, with no per-ISO setup. The one hard
rule stays: everything that runs at boot is built from auditable source.

This roadmap compares Glassboot with [Ventoy](https://www.ventoy.net/) and other
multi-boot tools (Easy2Boot, YUMI, GLIM, Rufus, MultiBootUSB, Super GRUB2 Disk), then
puts the gaps into phases. Each phase leaves the project in a usable state.

Legend: ✅ done · 🔨 in progress · ⬜ planned

---

## Phase 0: Foundation ✅

| Feature | Notes |
|---|---|
| ✅ Legacy BIOS + UEFI x86_64 + UEFI ia32 boot from one drive | MBR, GRUB `core.img` in the gap, FAT16 ESP |
| ✅ Drag-and-drop exFAT data partition | Files over 4 GB supported, readable on Windows, macOS and Linux |
| ✅ Reproducible, verified build of GRUB and wimboot from source | SHA-256 + GPG for GRUB, pinned commit for wimboot |
| ✅ Qt 6 installer (Windows/Linux) + CLI | Lists compatible drives, formats, verifies by reading back |
| ✅ Known-layout Linux boot | `loopback.cfg`, casper, live-boot, archiso, dracut |
| ✅ Windows installer ISOs via wimboot + WinPE helper | Tested with real ISOs in Phase 2 |

## Phase 1: "Drop in any ISO" (Ventoy core parity) ✅

The single feature that makes Ventoy Ventoy: almost any Linux ISO boots without
per-distro support, because the ISO file itself is presented to the OS as a disk.

| Feature | Ventoy equivalent | Notes |
|---|---|---|
| ✅ **Universal Linux boot.** The ISO's own boot menu runs unchanged. A GRUB module adds a small init hook to whatever kernel it starts, and the hook maps the ISO file's sectors as a read-only block device: a loop device, or device-mapper for fragmented files. | Normal mode + `ventoy.ko`/dm patch | Open C source: `boot/grub-module/` and `hook/`. Tested on UEFI: Alpine, Arch, Debian Live, Fedora KDE; on BIOS: Debian Live |
| ✅ ISO's own menu, from `grub.cfg` (EFI or BIOS), `isolinux.cfg`/`syslinux.cfg`, or `loopback.cfg` | GRUB2 mode / syslinux parsing | Uses GRUB's built-in `syslinux_configfile` |
| ✅ Recursive ISO scan of the whole data partition, sorted, with sizes | ListView | Skips `$RECYCLE.BIN`, `System Volume Information`, dot-folders |
| ✅ Fragmented ISO files handled | Ventoy needs contiguous files | The hook follows the exFAT cluster chain and builds a multi-segment device-mapper table |
| ✅ Alternative boot modes per ISO (`loopback.cfg` first; Phase 0 compatibility mode) | F-key boot modes | "More boot modes" menu |
| ✅ **Installer "Update"**: refresh the boot files without touching the ISOs | Ventoy2Disk *Update* | GUI + CLI; marker sector records the installed version |

## Phase 2: More image types and Windows 🔨

| Feature | Ventoy / others | Notes |
|---|---|---|
| ✅ Windows 11 installer ISO boots on UEFI and Legacy BIOS and reaches Windows Setup | Ventoy | GRUB module serves wimboot's files through a virtual UEFI file system; `glassboot-attach.exe` (our source) mounts the ISO in WinPE |
| ✅ Windows 11 requirement bypass (TPM, Secure Boot, CPU, RAM, storage) as a menu option | Ventoy `VTOY_WIN11_BYPASS_CHECK`, Rufus | Verified: without it Setup says "This PC can't run Windows 11"; with it Setup continues |
| ✅ Local account option (no Microsoft account / internet during setup) | Ventoy `VTOY_WIN11_BYPASS_NRO`, Rufus | `BypassNRO` via an answer file; not yet tested through a full install |
| ✅ Unattended Windows installs: every `\glassboot\unattend\*.xml` appears as a menu entry | Ventoy *auto_install* plugin | Passed to Setup with `/unattend` |
| ✅ `.efi` applications on the drive (UEFI) | Ventoy | Listed and chainloaded |
| ✅ Windows Server and WinPE-based tool discs | Ventoy | Tested: Windows Server 2025 Setup (UEFI + BIOS); Hiren's BootCD PE full desktop with its ISO mounted (UEFI). Discs without `install.wim` run in *tool mode*: the disc's own WinPE start-up runs, and Glassboot only mounts the ISO first. Limit: on Legacy BIOS, WinPE images too large for wimboot's memory (Hiren's 1.9 GB `boot.wim`) don't start |
| ⬜ Windows 10 installer ISO | Ventoy | Same path as Windows 11 and Server; not tested yet |
| ⬜ Linux unattended installs: kickstart, preseed, autoinstall, autoyast | Ventoy *auto_install* plugin | Extra kernel arguments through the hook |
| ✅ **Universal UEFI boot for non-Linux ISOs**: an EFI BlockIo driver presents the ISO as a read-only CD; the firmware's El Torito/FAT drivers find its EFI loader | Ventoy x64 EFI driver | `gb_efi_vdisk_boot` in the GRUB module. Automatic fallback for ISOs with no GRUB/syslinux menu, and a "More boot modes" option. Tested: FreeBSD 15.1 loader + kernel boot from the ISO |
| ✅ `.img` (raw disk image) and fixed-size `.vhd` boot on UEFI | Ventoy | Same virtual disk (512-byte sectors; firmware parses MBR/GPT). Tested: FreeBSD `mini-memstick.img` |
| ✅ `.wim` (Windows PE images) on UEFI and BIOS | Ventoy | wimboot; BCD and `boot.sdi` are taken from inside the image |
| ✅ File injection: Linux `\glassboot\inject\linux\*` (all ISOs) and `<iso>.cpio` (one ISO) appended to the initrd; Windows `\glassboot\inject\windows\*` copied into WinPE `System32`, plus an optional `glassboot-user.cmd` hook | Ventoy *injection* plugin | Linux archives overlay the distribution's initrd |
| ✅ Windows driver injection: every `.inf` under `\glassboot\drivers\` is loaded with `drvload` before Setup | Ventoy *dud* (Windows) | Not yet tested with a real driver |
| 🔨 BSD kernels finding their root file system inside the ISO/IMG file | Ventoy (BSD-specific handling) | FreeBSD ISO and raw UFS memstick RAM roots implemented (`md_image`), with UEFI ACPI handoff. FreeBSD 15.1 amd64 ISO/MBR memstick tested in QEMU. Other BSDs, ZFS, GPT validation and roots inside VHD containers remain. See [validation](docs/DISK-IMAGE-VALIDATION.md) |
| ✅ `.img` / `.vhd` and memdisk on **Legacy BIOS** (DOS, firmware updaters, old tools) | Ventoy F1 | syslinux 6.03 `memdisk`, built from the verified source tarball. `.img` files boot as a RAM disk; "More boot modes" offers "Load the ISO into RAM" for small ISOs. Tested: FreeBSD `mini-memstick.img`, ISO in RAM |
| 🔨 `.vhdx`, dynamic `.vhd`, and Windows native VHD boot (Windows-To-Go style) | Ventoy *vhdboot* | Fixed/dynamic VHD/VHDX readers implemented and byte-tested with ASan/UBSan; UEFI virtual disk and native Windows BCD route implemented. Native Windows end-to-end validation remains. Differencing disks, dirty logs and 4K logical VHDX sectors are refused. See [validation](docs/DISK-IMAGE-VALIDATION.md) |
| ✅ Linux driver update disks | Ventoy *dud* (Linux) | `dud ISO = FILE` in `glassboot.cfg`. The file is added to the initramfs (GRUB `newc:`) and passed as `inst.dd=` (Anaconda). Tested: Fedora 44 netinst processes it. SUSE `dud=` not done |

## Phase 3: Configuration and usability ✅

| Feature | Ventoy / others | Notes |
|---|---|---|
| ✅ `glassboot.cfg` on the data partition: default entry, timeout, menu names (aliases), hidden files and folders, per-ISO kernel arguments | `ventoy.json` | Plain key/value lines read by the GRUB module; no JSON parser at boot. See [docs/CONFIG.md](docs/CONFIG.md) |
| ✅ Menu classes / icons per entry | `ventoy.json` *menu_class* | `class PATH = NAME`; editable in Settings. Uses the theme's `icons/NAME.png` |
| ✅ Tree view (folders as sub-menus) and F-key shortcuts: F4 installed OSes, F5 tools, F6 more boot modes | Ventoy F2–F6 | `view = tree` |
| ✅ Search / filter the menu | Ventoy F3 | F3: case-insensitive literal search of paths and aliases; list/tree results, hidden files excluded |
| ✅ Themes, high-resolution menus, fonts | Ventoy *theme* plugin | GRUB `gfxmenu`; every `.pf2` next to the theme is loaded. Tested with GRUB's *starfield* theme. `unicode.pf2` is included, so ISO menus that load it (Debian) render correctly |
| ✅ Boot-time checksum (MD5/SHA-1/SHA-256/SHA-512) | Ventoy F5 tools | GRUB `hashsum`; matches `sha256sum` |
| ✅ Persistence (`.dat` overlay files per ISO) | Ventoy *persistence* plugin, Rufus | The hook maps the file read-write through device-mapper and adds `/dev/disk/by-label` links. Tested: Debian live (live-boot) and Arch (archiso). Ubuntu (casper) untested |
| ✅ Menu password protection | Ventoy *password* plugin | GRUB `password_pbkdf2`; the installer creates the hash |
| ✅ Boot an installed OS | Ventoy F4, Super GRUB2 Disk | UEFI: finds Windows Boot Manager and `\EFI\<vendor>` loaders on other disks through the firmware, then starts them with LoadImage. BIOS: chainloads other disks |
| ✅ Settings editor in the installer | VentoyPlugson | Qt dialog that edits `glassboot.cfg` and keeps comments. Creates persistence files on Linux; on Windows they still need Linux |
| ✅ Translations | Ventoy | Spanish installer catalog (123 messages) selected by system language; boot-menu gettext catalog via `language = es`. ISO-provided menus and low-level diagnostics retain their own language |

## Phase 4: Platform coverage 🔨

| Feature | Ventoy / others | Notes |
|---|---|---|
| ⬜ **Secure Boot**: shim + Glassboot key, one-time MOK enrolment | Ventoy (via its own shim/MOK chain) | Our key and signed GRUB; reproducible signing ceremony |
| ⬜ ARM64 UEFI | Ventoy | GRUB `arm64-efi` |
| ✅ GPT layout option (needed by some UEFI-only firmware) | Ventoy | GUI + CLI `--gpt`; UEFI only, 512-byte logical sectors. CRC-checked primary/backup tables; update preserves layout. MBR remains default |
| ⬜ NTFS / ext4 / XFS / UDF data partition options | Ventoy | exFAT stays default |
| ⬜ Non-destructive install (add Glassboot to a drive with data) | Ventoy 1.0.80+ | Shrink-and-add on NTFS / exFAT |
| ✅ Reserve space at end of disk (extra partitions) | Ventoy | GUI MiB field / CLI `--reserve SIZE`, MBR and GPT; updates preserve additional partitions |
| 🔨 4Kn disks, disks over 2 TiB | | GPT supports full capacity over 2 TiB (3 TiB image tested). 4Kn remains unsupported; requires filesystem/payload/I/O work |
| ⬜ macOS installer | Ventoy (unofficial) | Same `core/`, raw disk via `/dev/rdiskN` |

## Phase 5: Trust and distribution

| Feature | Notes |
|---|---|
| ⬜ Bit-for-bit reproducible payload, published hashes per release | Anyone can rebuild and compare |
| ⬜ Signed releases (minisign/GPG) + SBOM | |
| ⬜ CI: build everything, boot a matrix of ISOs in QEMU (BIOS + UEFI) with screenshot assertions | Builds on `scripts/qemu-script.sh` (scripted keys + screenshots) |
| ⬜ Packages: Windows installer (MSI) and portable zip, AppImage / Flatpak / `.deb` | |
| ⬜ Boot-time self-check: verify ESP files against the payload manifest | Detects a tampered stick |
| ⬜ PXE / network boot server (like iVentoy) | Same menu, served over the network |

---

## Known design differences from Ventoy

* **No closed or prebuilt components.** Ventoy ships prebuilt binaries (EFI drivers, a
  modified GRUB, helper tools) whose builds can't easily be reproduced. Glassboot builds
  every boot component from source in this repo or from pinned, verified upstream.
* **Upstream GRUB plus one small module,** not a large GRUB fork. This keeps security
  updates easy to take.
* **Fragmentation-tolerant.** Ventoy requires ISO files to be contiguous. The Glassboot
  hook maps fragmented files too.
