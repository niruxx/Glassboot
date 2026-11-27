%global _vpath_srcdir Utility-Source
Name:           glassboot
Version:        @PACKAGE_VERSION@
Release:        1%{?dist}
Summary:        Create and update Glassboot multiboot USB drives
License:        LicenseRef-Glassboot-Unspecified AND GPL-3.0-or-later AND GPL-2.0-or-later
URL:            https://github.com/niruxx/Glassboot
Source0:        @SOURCE_NAME@.tar.gz
ExclusiveArch:  x86_64

BuildRequires:  add-determinism
BuildRequires:  cmake
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  linkdupes
BuildRequires:  ninja-build
BuildRequires:  python3
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qttools-devel
BuildRequires:  desktop-file-utils
Requires:       qt6-qtbase-gui
Requires:       qt6-qtsvg
Requires:       qt6-qtwayland
Requires:       /usr/bin/pkexec
Requires:       util-linux
Requires:       e2fsprogs
Suggests:       ntfs-3g
Suggests:       xfsprogs
Suggests:       udftools

%description
Glassboot provides a graphical installer and command-line utility for creating
and updating multiboot USB drives. It includes the boot files used by both tools.

%prep
%autosetup -n @SOURCE_NAME@

%build
%cmake
%cmake_build

%install
%cmake_install
python3 scripts/release.py stage-native-payload payload "%{buildroot}%{_datadir}/glassboot/payload"

%check
%ctest
desktop-file-validate packaging/glassboot.desktop

%files
%license packaging/licenses/GPL-2.txt packaging/licenses/GPL-3.txt
%doc versions.env
%{_bindir}/glassboot-cli
%{_bindir}/glassboot-installer
%{_datadir}/applications/glassboot.desktop
%{_datadir}/icons/hicolor/scalable/apps/glassboot.svg
%{_datadir}/glassboot/
