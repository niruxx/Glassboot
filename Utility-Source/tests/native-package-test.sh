#!/usr/bin/env bash
# Exercise the installed package from outside the source tree.
set -euo pipefail
version=${1:?Usage: bash native-package-test.sh VERSION}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cd "$work"
unset GLASSBOOT_PAYLOAD
export QT_QPA_PLATFORM=offscreen

test "$(glassboot-cli --version)" = "Glassboot $version"
test "$(glassboot-installer --version)" = "Glassboot Installer $version"
desktop-file-validate /usr/share/applications/glassboot.desktop
test -s /usr/share/icons/hicolor/scalable/apps/glassboot.svg
for tool in pkexec mkfs.ext4 losetup mount umount; do
    command -v "$tool" >/dev/null
done

status=0
timeout --kill-after=2s 3s glassboot-installer --no-elevate || status=$?
if [[ $status -ne 124 ]]; then
  echo "Installed Glassboot GUI exited during startup (status $status)" >&2
  exit 1
fi

glassboot-cli install --image bios.img --size 512M
glassboot-cli update --image bios.img
glassboot-cli install --image uefi-4kn.img --size 512M --gpt --sector-size 4096
glassboot-cli update --image uefi-4kn.img --sector-size 4096
