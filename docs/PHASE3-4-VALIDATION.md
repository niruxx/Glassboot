# Phase 3 completion and Phase 4 start

Implemented in this pass:

- Per-entry GRUB theme classes (`class PATH = NAME`), with an icon-class column in Settings.
- F3 literal substring search over paths and aliases, including tree results and hidden-file handling.
- Spanish installer translations (123 messages) and boot-menu gettext translations.
- Optional GPT installs, UEFI only, using 512-byte logical sectors and full 64-bit partition offsets.
- Reserved tail space in the GUI and CLI for both layouts.
- GPT detection and updates that preserve partition tables, data and extra tail partitions.
- GPT partition-GUID matching in the Linux initrd hook.

Secure Boot and the macOS installer were not changed in this baseline. Later work
on data filesystems, non-destructive installation and 4Kn is covered in
[Storage and unattended installs](STORAGE-AND-UNATTENDED.md). ARM64 remains planned.

## Validation

- Linux Qt installer and CLI build; Windows core/CLI cross-build with MinGW.
- GRUB module built for i386-pc, x86_64-efi and i386-efi; payload assembled from those outputs.
- CTest: core configuration/password tests, sparse large-layout/CRC tests, translation completeness/placeholders.
- MBR, GPT and sparse 3 TiB installs: fsck.exfat, fsck.vfat, GPT verification, hook file lookup.
- Updates preserve ISO checksums and partition layouts, including a third GPT partition in reserved space.
- QEMU BIOS/MBR, UEFI/MBR and UEFI/GPT: menu loads, alias search, tree default IDs,
  custom classes, hidden-file exclusion, literal search and Spanish gettext output.
- Spanish Qt main window and Settings inspected using Xvfb screenshots.

Reproduce the image and menu checks on Linux as root (temporary image files only):

```sh
cmake -S Utility-Source -B /var/tmp/glassboot-installer
cmake --build /var/tmp/glassboot-installer -j4
ctest --test-dir /var/tmp/glassboot-installer --output-on-failure
bash Utility-Source/tests/platform-image-test.sh /var/tmp/glassboot-installer
bash Utility-Source/tests/menu-qemu-test.sh /var/tmp/glassboot-installer
```

The menu test includes a sparse 3 TiB GPT image on the Linux filesystem. Menu
screenshots and serial assertion output are written to `out/validation/`. These checks
use placeholder images to test the menu; they do not establish compatibility with new
Linux/Windows distributions. Physical USB hardware and a Windows Qt GUI build were
not tested in this pass. ISO-provided menus and low-level diagnostics retain their own
language. Ubuntu/casper persistence retains its earlier unverified status.
