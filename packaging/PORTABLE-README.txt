Glassboot portable development build

Windows: extract the complete ZIP, then run glassboot-installer.exe. Keep payload,
DLLs and plugin folders beside it. The GUI requests administrator rights to write
USB drives. No MSI, Qt installer or VC runtime installer is required.

Linux x86_64: make the AppImage executable and run it. This build targets the
Ubuntu 24.04 runtime baseline. If FUSE is unavailable, pass
--appimage-extract-and-run. Pass --cli --help to use the bundled command line.
Example: ./Glassboot-VERSION-linux-x86_64.AppImage --appimage-extract-and-run --cli --version

The boot payload is bundled and verified by the installer. The portable file
inventory is files.sha256; release artifact hashes are in the outer SHA256SUMS.
These are unsigned development builds unless a trusted maintainer separately
signs SHA256SUMS. A detached GPG signature is not Windows Authenticode or Secure Boot.

Installing normally formats the selected target and erases its existing contents.
Use a disposable disk/image when testing. No physical disk was used to validate
these packages. See the project README, ROADMAP.md and docs/RELEASING.md for scope.
