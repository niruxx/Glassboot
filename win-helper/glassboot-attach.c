/* glassboot-attach: attach an ISO file as a read-only virtual DVD drive in Windows PE.
 *
 *   glassboot-attach.exe X:\path\to\file.iso
 *
 * Prints the drive letter of the attached ISO (e.g. "E:") on success and exits 0.
 * diskpart's "attach vdisk" only handles VHD/VHDX; the Virtual Disk API also handles
 * ISO images (VIRTUAL_STORAGE_TYPE_DEVICE_ISO), which is what this uses.
 *
 * Built from source with MinGW-w64 by scripts/build.sh; injected into WinPE by wimboot.
 * Copyright (C) 2026 Glassboot contributors. GPL-3.0-or-later.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <virtdisk.h>
#include <stdio.h>
#include <wchar.h>

/* VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT; MinGW declares it but doesn't define it. */
static const GUID kVendorMicrosoft = {0xec984aec, 0xa0f9, 0x47e9, {0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b}};

/* Find a drive letter whose volume lives on the given physical path (\\.\CdRomN). */
static int find_letter(const wchar_t *physical, wchar_t *letter_out) {
    for (int tries = 0; tries < 40; tries++) {
        DWORD mask = GetLogicalDrives();
        for (wchar_t c = L'C'; c <= L'Z'; c++) {
            if (!(mask & (1u << (c - L'A')))) continue;
            wchar_t dev[8], target[512];
            swprintf(dev, 8, L"%c:", c);
            if (!QueryDosDeviceW(dev, target, 512)) continue;
            /* physical is \\.\CdRomN; the volume's target is \Device\CdRomN */
            const wchar_t *p = wcsrchr(physical, L'\\');
            const wchar_t *t = wcsrchr(target, L'\\');
            if (p && t && _wcsicmp(p, t) == 0) {
                *letter_out = c;
                return 1;
            }
        }
        Sleep(250);
    }
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        fwprintf(stderr, L"usage: glassboot-attach FILE.iso\n");
        return 2;
    }

    VIRTUAL_STORAGE_TYPE type = {VIRTUAL_STORAGE_TYPE_DEVICE_ISO, kVendorMicrosoft};
    OPEN_VIRTUAL_DISK_PARAMETERS op;
    ZeroMemory(&op, sizeof op);
    op.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    HANDLE vd = NULL;
    DWORD rc = OpenVirtualDisk(&type, argv[1], VIRTUAL_DISK_ACCESS_READ | VIRTUAL_DISK_ACCESS_ATTACH_RO,
                               OPEN_VIRTUAL_DISK_FLAG_NONE, &op, &vd);
    if (rc != ERROR_SUCCESS) {
        fwprintf(stderr, L"OpenVirtualDisk failed: %lu\n", rc);
        return 1;
    }

    ATTACH_VIRTUAL_DISK_PARAMETERS ap;
    ZeroMemory(&ap, sizeof ap);
    ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    rc = AttachVirtualDisk(vd, NULL, ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY | ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME,
                           0, &ap, NULL);
    if (rc != ERROR_SUCCESS) {
        fwprintf(stderr, L"AttachVirtualDisk failed: %lu\n", rc);
        CloseHandle(vd);
        return 1;
    }

    wchar_t physical[512];
    ULONG size = sizeof physical;
    wchar_t letter = 0;
    if (GetVirtualDiskPhysicalPath(vd, &size, physical) == ERROR_SUCCESS && find_letter(physical, &letter)) {
        wprintf(L"%c:\n", letter);
    } else {
        fwprintf(stderr, L"attached, but no drive letter found\n");
    }
    CloseHandle(vd); /* PERMANENT_LIFETIME: stays attached */
    return letter ? 0 : 1;
}
