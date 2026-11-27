#!/usr/bin/env bash
# Build a native x86-64 package using the target distribution's toolchain.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
distro=${1:-}
if [[ $# -gt 2 || ! "$distro" =~ ^(arch|fedora|debian)$ ]]; then
  echo "Usage: bash scripts/package-native-linux.sh {arch|fedora|debian} [PAYLOAD_DIR]" >&2
  exit 2
fi
if [[ $(uname -m) != x86_64 ]]; then
  echo "Native Glassboot packages currently target x86-64." >&2
  exit 1
fi

export TZ=UTC LC_ALL=C SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1704067200}"
payload=${2:-${GLASSBOOT_OUT_DIR:-$root/out}/payload}
dist=${GLASSBOOT_DIST_DIR:-$root/dist}
version=$(python3 "$root/scripts/release.py" version)
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9.]+)?$ ]]; then
  echo "Unsupported Glassboot version: $version" >&2
  exit 1
fi
arch_version=${version/-/}
deb_rpm_version=${version/-/\~}

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
source_name="glassboot-$version"
source_dir="$work/$source_name"
recipe="$root/packaging/$distro"
mkdir -p "$source_dir"
tar -C "$root" --exclude='./Utility-Source/build*' --exclude='__pycache__' -cf "$work/source.tar" \
  ./Utility-Source ./boot ./hook ./win-helper ./scripts ./packaging ./versions.env
tar -C "$source_dir" -xf "$work/source.tar"
python3 "$root/scripts/release.py" stage-native-payload "$payload" "$source_dir/payload"

case "$distro" in
  arch|fedora)
    tar -C "$work" --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
      -czf "$work/$source_name.tar.gz" "$source_name"
    ;;
esac

case "$distro" in
  arch)
    checksum=$(sha256sum "$work/$source_name.tar.gz" | cut -d' ' -f1)
    sed -e "s/@PACKAGE_VERSION@/$arch_version/g" -e "s/@SOURCE_NAME@/$source_name/g" \
      -e "s/@SOURCE_SHA256@/$checksum/g" "$recipe/PKGBUILD" > "$work/PKGBUILD"
    (cd "$work" && PKGDEST="$work" makepkg --noconfirm --nosign)
    artifact="$work/glassboot-$arch_version-1-x86_64.pkg.tar.zst"
    ;;
  fedora)
    mkdir -p "$work/rpm/SOURCES"
    mv "$work/$source_name.tar.gz" "$work/rpm/SOURCES/"
    sed -e "s/@PACKAGE_VERSION@/$deb_rpm_version/g" -e "s/@SOURCE_NAME@/$source_name/g" \
      "$recipe/glassboot.spec" > "$work/glassboot.spec"
    rpmbuild -bb --define "_topdir $work/rpm" "$work/glassboot.spec"
    artifacts=("$work/rpm/RPMS/x86_64/glassboot-$deb_rpm_version"-1*.x86_64.rpm)
    artifact=${artifacts[0]}
    ;;
  debian)
    cp -r "$recipe" "$source_dir/debian"
    sed -e "s/@PACKAGE_VERSION@/$deb_rpm_version/g" \
      -e "s/@CHANGELOG_DATE@/$(date -R -d "@$SOURCE_DATE_EPOCH")/g" \
      "$recipe/changelog.in" > "$source_dir/debian/changelog"
    rm "$source_dir/debian/changelog.in"
    chmod +x "$source_dir/debian/rules"
    (cd "$source_dir" && dpkg-buildpackage -b -us -uc)
    artifact="$work/glassboot_${deb_rpm_version}-1_amd64.deb"
    ;;
esac

mkdir -p "$dist"
cp -- "$artifact" "$dist/"
echo "Built $dist/${artifact##*/}"
