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
| Feature | Ventoy / others | Notes |
|---|---|---|
| ⬜ **Secure Boot**: shim + Glassboot key, one-time MOK enrolment | Ventoy (via its own shim/MOK chain) | Our key and signed GRUB; reproducible signing ceremony |
| ⬜ Windows 10 installer ISO | Ventoy | Same path as Windows 11 and Server; not tested yet |
| 🔨 Linux unattended installs: kickstart, preseed, autoinstall, autoyast | Ventoy *auto_install* plugin | Per-ISO answer injection and kernel arguments; NoCloud CIDATA seed helper. Real-kernel transport tests pass; full distribution installations remain unverified. See [storage and unattended](do
| 🔨 Bit-for-bit reproducible payload, published hashes per release | Two assemblies from identical built components/toolchain/epoch compare byte-for-byte. FAT timestamps normalized for both sector sizes; local release hashes in `dist/SHA256SUMS`. Independent clean toolchain rebuilds and publication remain. See [releasing](docs/RELEASING.md) |
| 🔨 Signed releases (minisign/GPG) + SBOM | CycloneDX bundled-file inventories, component pins, provenance, notices and GPG checksum-signing helper implemented. Temporary-key signing test passes. Local builds remain unsigned; production signing requires a maintainer key and license review |
| 🔨 CI: build everything, boot a matrix of ISOs in QEMU (BIOS + UEFI) with screenshot assertions | Pinned-action workflow builds both packages and boot sources. Local clean/corrupt ESP screenshot tests and pinned Alpine BIOS/UEFI boots pass. GitHub execution and broader distro/Windows fixtures remain |
| 🔨 PXE / network boot server (like iVentoy) | Linux server prepares an ISO menu, BIOS/x86 UEFI/ARM64 UEFI loaders, HTTP media and proxy-DHCP/TFTP configuration. Debian Live reaches login over BIOS and x86_64 UEFI PXE; initial Casper / Debian Live profiles. Arbitrary ISO and Windows network boot remain. See [ARM64 and PXE](docs/ARM64-AND-NETBOOT.md) |

---

## Known design differences from Ventoy

* **No closed or prebuilt components.** Ventoy ships prebuilt binaries (EFI drivers, a
  modified GRUB, helper tools) whose builds can't easily be reproduced. Glassboot builds
  every boot component from source in this repo or from pinned, verified upstream.
* **Upstream GRUB plus one small module,** not a large GRUB fork. This keeps security
  updates easy to take.
* **Fragmentation-tolerant.** Ventoy requires ISO files to be contiguous. The Glassboot
  hook maps fragmented files too.
