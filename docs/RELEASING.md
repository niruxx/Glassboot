# Portable builds and Phase 5 validation

Release outputs go to the repository's ignored `dist/` directory:

* `Glassboot-VERSION-linux-x86_64.AppImage`
* `Glassboot-VERSION-windows-x86_64.zip`, plus its unpacked portable directory
* Per-artifact `.sbom.json` (CycloneDX 1.6) and `.provenance.json` files
* `SHA256SUMS`

The Windows directory contains the GUI, CLI, Qt plugins/libraries, app-local MSVC
runtime and verified boot payload. Extract the complete ZIP and run
`glassboot-installer.exe`; no MSI or runtime installer is needed. The GUI still
needs administrator permission to write physical disks. Windows 10/11 x64 is the
target; native Windows GUI/CLI builds and portable CLI image creation were checked.

The AppImage includes the Qt/runtime dependency closure and the same payload. It
is built on the Ubuntu 24.04 x86_64 runtime baseline, not guaranteed for older
glibc systems. It can use FUSE or extract itself:

```sh
chmod +x dist/*.AppImage
dist/*.AppImage --appimage-extract-and-run
dist/*.AppImage --appimage-extract-and-run --cli --help
```

Linux GUI startup/version reporting and CLI image creation were checked in extract
mode. Physical USB installation and the FUSE mount path were not validated here.
Elevation relaunches the outer AppImage in extract mode, giving root its own
runtime and bundled payload instead of using a user's FUSE mount.

## Build

First build the boot components and payload on Linux/WSL:

```sh
sudo bash scripts/install-deps.sh
bash scripts/build.sh all
bash scripts/build-payload.sh
```

For Linux packaging, also install `libtool-bin libfuse3-dev libzstd-dev
squashfs-tools patchelf`. Then:

```sh
bash scripts/package-linux.sh
```

`GLASSBOOT_LINUX_BUILD_DIR` defaults to `/var/tmp/glassboot-release-linux`.
The AppImage type-2 runtime and Squashfuse are built from pinned source using
`packaging/versions.env`; the runtime uses static distro libraries and the normal
C allocator. No prebuilt AppImage runtime is fetched. The source-build helper is
`scripts/build-appimage-runtime.sh`. Its cached binary/hash/license records live
under `out/appimage/`. Qt shared libraries come from the installed Linux SDK.

For Windows, install Visual Studio 2022 C++ Build Tools, CMake, Python 3.10+ and Qt
6.8.3 for MSVC 2022 x64 including Linguist tools. From PowerShell:

```powershell
./scripts/package-windows.ps1 -QtDir C:/Qt/6.8.3/msvc2022_64
# Explicitly replace an existing generated portable directory:
./scripts/package-windows.ps1 -QtDir C:/Qt/6.8.3/msvc2022_64 -Replace
```

The default Windows build directory is `build/release-windows`. The script uses
`windeployqt` and copies the MSVC CRT DLLs locally. It verifies the payload before
packaging and refuses directory/junction escapes during replacement.

## Integrity, inventories and signing

`files.sha256` inside each bundle covers its content, including the SBOM and
provenance. Verify an extracted package with:

```sh
python3 scripts/release.py verify /path/to/extracted/bundle
(cd dist && sha256sum --strict -c SHA256SUMS)
```

On Windows, `Get-FileHash dist/*.zip -Algorithm SHA256` prints artifact hashes for
comparison with the trusted checksum list. The CycloneDX SBOM inventories bundled
file hashes and identifies Qt and pinned boot/runtime components. It is not a full
transitive source-license audit; dependency licenses are not guessed. Notices and
available license texts are included. Provenance records the source commit,
working-tree modification flag, upstream pins and normalized build epoch. Publish
from a reviewed clean commit, not an uncommitted development checkout.

Local packages are **unsigned**. A maintainer with an existing trusted GPG key can
sign the complete checksum list after both platform artifacts have been collected:

```sh
python3 scripts/release.py hashes dist
bash scripts/sign-release.sh FULL_SIGNING_KEY_FINGERPRINT
(cd dist && gpg --verify SHA256SUMS.asc SHA256SUMS && sha256sum --strict -c SHA256SUMS)
```

The script never creates a signing identity. A downloaded public key must have its
fingerprint authenticated separately. GPG signatures do not provide Windows
Authenticode, SmartScreen reputation, or a Secure Boot chain. Signing was tested
with an isolated disposable key; that key/signature is not shipped in `dist/`.

## Reproducibility boundary

`SOURCE_DATE_EPOCH` defaults to `1704067200`. Payload assembly fixes locale/timezone,
normalizes FAT16 directory timestamps on both sector geometries, and uses invariant
FAT formatting. Hook archives have fixed timestamps; MinGW helper builds disable
PE timestamps. ZIPs use sorted entries and fixed dates, and SquashFS timestamps
are normalized.

```sh
bash scripts/check-reproducible-payload.sh
```

Two assemblies from the same compiled components/toolchain/epoch compare byte for
byte, including both ESP images and manifests. This does **not** establish that
independent clean toolchain/source builds produce identical components. Compiler,
Qt, distro library and font versions still need a hermetic/snapshot-pinned build
environment before claiming that stronger guarantee. No release hashes are
published automatically; local hashes are in `dist/SHA256SUMS`.

## Boot self-check and CI

`boot/glassboot/esp.sha256` records the staged ESP files. GRUB checks it before
loading the data-partition configuration or ISO menu and stops on mismatch.
Installer personalization changes FAT BPB fields, not those file contents.

This detects corruption; it is **not an authenticity root**. GRUB, the verifier and
the manifest share the writable ESP, and initial code has already run. An attacker
who changes both code and hashes can bypass it. Secure Boot remains separate work.
User settings/themes/persistence on the data partition are not measured.

The pinned-action workflow `.github/workflows/release.yml` builds boot sources,
runs unit and checksum tests, builds both portable formats and collects artifacts.
It does not publish a release or access a signing key. Locally checked commands:

```sh
sudo bash Utility-Source/tests/release-qemu-test.sh /path/to/linux/build
sudo bash Utility-Source/tests/release-iso-test.sh /path/to/linux/build
```

The first checks clean/corrupt ESPs on BIOS/UEFI using serial assertions and
different success/failure screenshots. The second SHA-256-pins Alpine 3.24.4 and
requires its live system to reach userspace on both firmwares. Screenshots/logs
are under `out/validation/`. CI expands this initial firmware matrix; Debian,
Fedora, BSD and legally supplied Windows fixtures remain to be added. The workflow
itself has not been run on GitHub in this session. PXE/network boot is still planned.
