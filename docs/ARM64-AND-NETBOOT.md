# ARM64 UEFI and network boot

## ARM64 USB boot

`scripts/build.sh` cross-builds GRUB `arm64-efi` and a static ARM64 Linux initrd
hook alongside the existing x86 components. `scripts/build-payload.sh` includes
`EFI/BOOT/BOOTAA64.EFI`, ARM64 GRUB modules and the hook in both ESP images.
The existing Windows/Linux installer writes this same payload; the installer
itself still runs on x86_64. Update an existing stick using the rebuilt payload.

Install dependencies with `sudo bash scripts/install-deps.sh`, then run:

```sh
bash scripts/build.sh all
bash scripts/build-payload.sh
sudo bash Utility-Source/tests/arm64-qemu-test.sh /path/to/linux-build
```

The test downloads the pinned Alpine 3.24.4 aarch64 ISO, checks its SHA-256,
creates a temporary GPT USB image, and boots it under QEMU AAVMF. The ARM64
hook maps the ISO from the data partition and Alpine reaches `localhost login:`.
The test does not install an OS or write to a physical disk.

EFI virtual CD/disk boot and installed-OS discovery select `BOOTAA64.EFI` and
ARM64 vendor loaders. Use ARM64 media: an ARM64 GRUB cannot execute x86 kernels
or EFI programs. ARM64 raw/VHD media uses its native EFI loader rather than the
x86 BSD RAM-root or Windows native VHD boot paths. Standalone Windows WIM boot
is refused on ARM64. Windows ARM64 ISO media uses its own EFI loader, but full
Windows setup, ISO reattachment, native VHD boot and the ARM WinPE helper are
not implemented or validated. Secure Boot and physical ARM hardware remain untested.

## Linux PXE server

The initial server is a command-line tool for Linux. It recursively discovers
ISOs, creates a sorted GRUB menu, extracts kernels/initrds and required live media,
and builds BIOS, x86_64 UEFI, ia32 UEFI and ARM64 UEFI network loaders using
Glassboot's source-built GRUB. Unsupported ISOs remain visible with an explanation.
This menu is generated from the source directory when `prepare` runs; it is not
a live scan and does not import USB `glassboot.cfg` aliases, passwords or themes.

Currently supported layouts:

| ISO layout | Network root transport |
| --- | --- |
| Ubuntu Casper: `/casper/vmlinuz`, `/casper/initrd*` | HTTP ISO download with `iso-url=` / `url=` |
| Debian Live: `/live/vmlinuz*`, `/live/initrd.img*`, `/live/filesystem.squashfs` | HTTP squashfs download with `fetch=` |

These use the distributions' own network boot features:
[Casper manual](https://manpages.ubuntu.com/manpages/noble/man7/casper.7.html) and
[Debian live-boot manual](https://manpages.debian.org/testing/live-boot-doc/live-boot.7.en.html).
Clients need enough RAM for the downloaded ISO or squashfs plus the running OS.
An ISO matching a layout is not a guarantee that every derivative has suitable
initramfs network drivers or the same boot parameters. Casper layered live images
may need distribution-specific arguments; broad Ubuntu validation remains.
Windows/WinPE, arbitrary Linux ISOs, BSD, persistence and local-disk hook injection
are not supported over PXE yet. This is initial Linux PXE support, not complete
iVentoy parity. Choose media matching each client's architecture.

Build GRUB first. On the Linux server, install `dnsmasq` for proxy-DHCP and TFTP.
Use the server's LAN IPv4 address and interface, replacing the example values:

```sh
python3 scripts/netboot.py prepare \
  --isos /srv/isos --output /srv/glassboot-pxe-v1 \
  --server 192.168.1.10 --interface enp3s0 --port 8080
python3 scripts/netboot.py serve \
  --root /srv/glassboot-pxe-v1 --bind 192.168.1.10 --port 8080
# In another terminal:
sudo dnsmasq --no-daemon --conf-file=/srv/glassboot-pxe-v1/dnsmasq.conf
```

Preparation needs `xorriso`, Python 3 and `out/grub`. It does not need root.
Use `--grub PATH` for another build location and `--platforms x86_64-efi arm64-efi`
to restrict loader generation. Output must be a new directory, separate from the
ISO source. Files are copied into a self-contained tree; allow space for the live
media. To refresh the menu, prepare a new tree and point both servers at it.
Preparation never starts a service or changes the host network configuration.

The generated dnsmasq file disables DNS, uses proxy-DHCP with the existing DHCP
server, and serves the architecture-specific first-stage loader over TFTP.
Use an isolated test LAN or a network where you administer PXE. Permit HTTP on
the selected TCP port and dnsmasq's DHCP/PXE/TFTP traffic. Select network boot
in the client's firmware. The generated configuration must be adjusted for routed
networks, DHCP relays or an existing PXE service. WSL NAT is unsuitable for serving
LAN DHCP broadcasts; run on a native Linux host or a bridged Linux VM.

The HTTP service is read-only and checks resolved paths against its root. It has
no authentication or TLS, so serve only intended boot media. Secure Boot is not
supported by these unsigned loaders.

## Validation

```sh
python3 Utility-Source/tests/netboot-test.py
bash Utility-Source/tests/netboot-qemu-test.sh bios /path/to/debian-live.iso
bash Utility-Source/tests/netboot-qemu-test.sh uefi /path/to/debian-live.iso
```

This integration test creates real fixture ISOs, builds all four network loaders,
checks generated GRUB syntax and supported/unsupported entries, refuses output
overwrite, and verifies HTTP file serving and traversal/symlink containment.
Verified locally: Alpine 3.24.4 aarch64 reaches login from the ARM64 USB payload;
Debian Live 13.7.0 amd64 reaches login over both BIOS and x86_64 UEFI PXE, after
fetching its kernel, initrd and squashfs over HTTP. Results are in `out/validation/`.
Casper boots, ARM64/ia32 PXE clients, physical LAN proxy-DHCP and physical ARM
hardware have not been validated yet.
Firmware architecture coverage, supported distro coverage and network transport
coverage are separate; do not infer full OS support from loader generation alone.
