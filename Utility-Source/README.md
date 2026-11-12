# Glassboot Installer

A Qt 6 desktop app (Windows and Linux) and a matching command-line tool that turn a USB
drive into a Glassboot stick.

The app is a single window:
* **A list of compatible drives.** A drive appears only if it is USB or removable, at
  least 321 MiB, uses 512-byte or 4096-byte sectors, and doesn't hold the running OS. Below the list,
  a note says how many other drives were hidden; hover over it to see why. The list
  refreshes on its own when drives are plugged in or removed, and a lone compatible
  drive is preselected.
* **An Install button.** After one "Format and install" confirmation, it formats the
  selected drive. A progress bar shows the current step and its percentage. It animates
  while the drive is being prepared, then fills step by step. "Show details" opens the log.
* **Update, for drives that already have Glassboot.** The "Glassboot" column shows the
  installed version. For those drives, **Update** refreshes only the boot files: the EFI
  partition, the BIOS boot code and the marker. The partition table and every ISO on the
  `GLASSBOOT` drive are kept, the same as Ventoy's *Update* button. Install becomes
  "Reinstall…", which erases the drive.
* **Settings…, for drives that already have Glassboot.** Edits the boot menu settings in
  `\glassboot\glassboot.cfg` on the mounted `GLASSBOOT` drive
  ([docs/CONFIG.md](../docs/CONFIG.md)), like VentoyPlugson:
  * **Menu tab:** timeout, default entry, list or tree layout, theme, and password (stored
    as a salted PBKDF2 hash, the format GRUB checks).
  * **Images tab:** every bootable file, in folders. Untick a file or folder to hide it,
    or give it a menu name, Linux kernel arguments, or a persistence file. On Linux,
    **Create persistence file…** makes the ext4 file with the right label for Ubuntu,
    Debian or Arch.
  * **glassboot.cfg tab:** previews the file as it will be saved.

  The file is updated in place: comments, ordering and unknown lines are kept. No
  administrator rights are needed. `glassboot-installer --settings DIR` opens only this
  dialog, for a drive mounted at `DIR`.

Clicking **Install** does all of the following:

1. Checks the bundled boot files (the *payload*) against their SHA-256 manifest.
2. Unmounts, locks and dismounts every volume on the chosen drive.
3. Clears the old partition tables, including a GPT backup header at the end of the disk.
4. Writes the 64 MiB FAT16 EFI system partition (GRUB for UEFI x64/ia32, modules, menu, wimboot).
5. Formats the rest of the drive as exFAT (`GLASSBOOT`) and creates `\glassboot\grubenv`.
6. Writes GRUB's BIOS boot code: `core.img` in the 1 MiB gap, a Glassboot marker sector
   (LBA 2047, holding the version), then the MBR with the partition table.
7. Reads it all back and compares it, which catches faulty or counterfeit drives.
8. Asks the OS to re-read the partition table so the `GLASSBOOT` drive appears.

The installer writes every byte itself. It doesn't call `diskpart`, `format`, `mkfs`,
`grub-install` or any other external tool, so a stick comes out the same on every OS.

## Layout

```
core/      Plain C++17, no Qt. Disk access, MBR, exFAT formatter, installer steps
  BlockDevice.*      raw device interface, image files, aligned buffers
  Disks_linux.cpp    sysfs/procfs enumeration, umount, O_EXCL raw I/O, BLKRRPART
  Disks_windows.cpp  \\.\PhysicalDriveN enumeration, volume lock/dismount, raw I/O
  Mbr.*              partition layout, MBR + GRUB boot.img/core.img patching
  ExFat.*            exFAT formatter (Microsoft exFAT specification)
  Payload.*          payload discovery and SHA-256 verification
  Installer.*        the install sequence above
  Config.*           glassboot.cfg parser/editor (keeps comments and unknown lines)
  Pbkdf2.*           SHA-512 + PBKDF2 for GRUB password hashes
cli/       glassboot-cli: list / install (scripting, CI, testing)
gui/       glassboot-installer: Qt 6 Widgets front end (MainWindow, ConfigDialog)
platform/windows/  manifests (GUI requests admin, CLI runs as the invoking user)
translations/      Qt Linguist .ts files (see translations/README.md)
tests/     image-test.sh: end-to-end install + fsck on a disk image
           core-test.cpp: SHA-512/PBKDF2 vectors and glassboot.cfg editing (ctest)
```

Keeping `core/` free of Qt means the Windows disk code can be compiled and tested from
Linux with MinGW. It also means the CLI ships as a single static `.exe`.

## The payload

The installer doesn't contain GRUB itself. It writes the files that
`scripts/build-payload.sh` (in the repo root) produces in `out/payload/`:

```
manifest.txt   version, layout constants, size + SHA-256 of each file
mbr.bin        GRUB boot.img
core.img       GRUB BIOS core image
esp.img        complete FAT16 EFI system partition
```

At runtime the payload is looked for in this order:
1. `--payload DIR`
2. `$GLASSBOOT_PAYLOAD`
3. `<exe dir>/payload`
4. `<exe dir>/../share/glassboot/payload`
5. `out/payload` in any parent directory (development builds)

## Building

First build GRUB and the payload (from the repo root, in Linux or WSL):

```sh
sudo scripts/install-deps.sh
scripts/build.sh
scripts/build-payload.sh
```

### Linux

```sh
cmake -S Utility-Source -B Utility-Source/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Utility-Source/build
Utility-Source/build/glassboot-installer       # offers to restart itself via pkexec
```

### Windows (MSVC + Qt 6)

Install Visual Studio 2022 (with the *Desktop development with C++* workload) and Qt 6
for MSVC 2022 64-bit, for example with the Qt Online Installer. Then, from a
*Developer PowerShell for VS 2022*:

```powershell
cmake -S Utility-Source -B Utility-Source\build -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64
cmake --build Utility-Source\build --config Release
# bundle Qt DLLs and the payload next to the exe
C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe Utility-Source\build\Release\glassboot-installer.exe
Copy-Item -Recurse out\payload Utility-Source\build\Release\payload
```

The GUI's manifest requests administrator rights, so Windows shows a UAC prompt when it starts.

### Windows CLI from Linux (cross-compile)

```sh
cmake -S Utility-Source -B /tmp/gb-win -G Ninja -DGLASSBOOT_BUILD_GUI=OFF \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/gb-win          # -> glassboot-cli.exe (static, no DLLs)
```

### Testing the GUI without a USB drive

`--test-image FILE [--test-size MiB]` is a hidden developer option. It adds a disk-image
file to the drive list, so you can go through the whole flow (select, confirm, progress,
verify, done) without real hardware. Writing a file needs no admin rights.

```sh
glassboot-installer --no-elevate --test-image /tmp/test.img --test-size 4096
```

## Command line

```sh
glassboot-cli list [--all]                          # compatible drives (--all: every disk + why it's excluded)
glassboot-cli install --device /dev/sdX             # asks you to type YES
glassboot-cli install --device '\\.\PhysicalDrive3' --yes
glassboot-cli install --image test.img --size 8G    # no admin needed
glassboot-cli update --device /dev/sdX                # refresh boot files, keep the ISOs
```

## Safety rules (enforced in core/, not just the GUI)

* One compatibility rule, `incompatibility()` in `core/Installer.cpp`, is used by both
  the GUI and the CLI.
* A disk holding the running OS is never writable. On Linux that means it has `/`,
  `/boot`, `/usr` or similar mounted, swap, or LVM/RAID/LUKS holders. On Windows it means
  it holds the Windows directory or a volume that spans several disks.
* The GUI only lists compatible (USB or removable) drives and always asks for confirmation
  before formatting. The CLI also accepts an internal disk, but only when you name it with `--device`.
* 512-byte sectors support MBR/BIOS and GPT/UEFI. 4Kn requires GPT/UEFI and the
  separately verified `esp4k.img` payload. Firmware compatibility still varies.

## Testing status

| What | How | Status |
|---|---|---|
| Install to image, Linux | `tests/image-test.sh` → `fsck.exfat`, `fsck.vfat`, kernel mount | passes |
| Install to image, Windows native | `glassboot-cli.exe --image`, then fsck from Linux | passes |
| Image boots: BIOS and UEFI | QEMU (SeaBIOS / OVMF), ISO menu + loopback | passes |
| GUI flow, Linux: list → confirm → progress → done | Xvfb + xdotool, `--test-image`, screenshots | passes |
| GUI failure path: error shown, bar resets | same, with an unwritable target | passes |
| Core unit tests (SHA-512, PBKDF2, cfg editing) | `ctest`; PBKDF2 also cross-checked against Python `hashlib` | passes |
| Settings dialog: load, edit, save keeps the rest of the file | Xvfb + xdotool, `--settings` | passes |
| Create persistence file on exFAT, then boot with it | GUI (Arch preset) → Arch boots with the file and keeps changes | passes |
| Password hash accepted by GRUB | hash from our PBKDF2 → GRUB `password_pbkdf2` in QEMU | passes |
| Translation loading | throwaway German `.ts`, `LANGUAGE=de` → German window title | passes |
| Real USB stick, Linux | `--device /dev/sdX` | not yet tested |
| Real USB stick, Windows (volume lock/dismount path) | GUI or CLI as admin | not yet tested |
| Windows accepts the exFAT volume (`chkdsk`) | | not yet tested |
| Qt GUI on Windows (MSVC build) | | not yet built |

## Not done yet

* **Installer packaging:** an MSI or zip for Windows, and an AppImage or `.deb` for Linux.
* **Persistence files on Windows:** they need an ext4 file system, and the installer
  can't create one there yet.
* **Additional translations:** Spanish is included; more languages can be added in `translations/`.
* **Fuller exFAT up-case table:** currently covers Latin, Greek and Cyrillic.
* **4Kn hardware validation:** image/filesystem and NVMe UEFI menu tests pass;
  physical USB hardware and Windows raw writes remain untested.

## GPT and reserved space

Linux filesystem choices, unattended profiles, preserve-data/NTFS shrink-and-add
and 4Kn setup are documented in [Storage and unattended installs](../docs/STORAGE-AND-UNATTENDED.md).

The GUI offers **Partition table** and **Reserve at end of disk** for fresh installs.
MBR remains the default and supports BIOS plus UEFI. GPT is **UEFI only**, supports
full capacity above 2 TiB, and uses primary and backup GPT tables with CRC checks.
MBR requires 512-byte logical sectors; GPT also supports 4096-byte logical sectors.
Secure Boot is not supported.

```sh
glassboot-cli install --image /tmp/glassboot-gpt.img --size 4G --gpt --reserve 512M
glassboot-cli install --device /dev/sdX --gpt --reserve 1G
```

These install commands erase the target. Reserved space is unallocated space after the
EFI partition; GPT's backup table stays at the physical end of the disk. `Update`
automatically detects either layout and preserves it, including reserved space and
additional partitions. Layout options apply to fresh installs, not updates.

Run the core and layout checks with `ctest --test-dir BUILD_DIR --output-on-failure`.
On Linux as root, `tests/platform-image-test.sh BUILD_DIR` also checks temporary MBR,
GPT and 3 TiB images, both filesystems, the Linux hook, and preservation during update.

`tests/menu-qemu-test.sh BUILD_DIR` boots BIOS/MBR, UEFI/MBR and a sparse 3 TiB
UEFI/GPT image and asserts menu behavior through serial output. See
[Phase 3/4 validation](../docs/PHASE3-4-VALIDATION.md) for scope and limitations.
