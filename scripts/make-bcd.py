#!/usr/bin/env python3
"""Write the BCD template that Glassboot uses to boot Windows from a VHD/VHDX file.

    make-bcd.py OUTPUT

A BCD store is a Windows registry hive. This script describes the store in plain Python
and writes the hive itself (regf format), so no Windows tool and no binary from Microsoft
is involved.

The store has one Windows entry whose device and osdevice are
"vhd=[<GLASSBOOT partition>]<path>,locate=custom:12000002": Windows mounts the VHD file
from the GLASSBOOT data partition and finds the partition inside it that holds
\\Windows\\system32\\winload.exe. This is the same encoding bcdboot writes for a VHD it has
set up for native boot (captured from bcdboot on Windows 11 24H2, see docs/ARCHITECTURE.md).

At boot the Glassboot GRUB module (gb_vhd_bcd) fills in, in place:
  * the VHD path (a fixed 520-byte UTF-16 field, so no length changes), found by the
    PATH_MARKER string;
  * the data partition's byte offset and the disk's MBR signature (or GPT GUIDs).
On UEFI, wimboot changes "winload.exe" to "winload.efi" as it serves the file.
"""
import struct
import sys

# ------------------------------------------------------------------ BCD contents

BOOTMGR = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}"      # {bootmgr} (well-known)
GLOBALSETTINGS = "{7ea2e1ac-2e61-4728-aaa3-896d9d0a9f0e}"
BOOTLOADERSETTINGS = "{6efb52bf-1766-41db-a6b3-0ee5eff72bd7}"
HYPERVISORSETTINGS = "{7ff607e0-4395-11db-b0de-0800200c9a66}"
DBGSETTINGS = "{4636856e-540f-4170-a130-a84776f4c654}"
EMSSETTINGS = "{0ce4991b-e6b3-4b16-b23c-5e0d9250e5d9}"
BADMEMORY = "{5189b25c-5558-4bf2-bca4-289b11bd29e2}"
OSLOADER = "{476c6173-7362-4f6f-8f74-564844000001}"     # ours: "Glassboot VHD"

PATH_MARKER = "\\GLASSBOOT-VHD-PATH"
PATH_FIELD = 520            # bytes: 259 UTF-16 characters + terminating NUL


def vhd_device():
    """Device element: vhd=[partition]path,locate=custom:12000002 (layout from bcdboot).

    Offsets are within the element. The partition record at 0x78 (Ventoy calls it
    ventoy_patch_vhd) is: offset or partition GUID [16], reserved [4], partition table
    type [4] (1 = MBR, 0 = GPT), MBR signature or disk GUID [16], reserved [16], then the
    path at 0xB0. GRUB fills the zeros in."""
    end = 0xB0 + PATH_FIELD
    b = bytearray(end)
    # 0x00: no additional options GUID
    struct.pack_into("<IIII", b, 0x10, 6, 0, end - 0x10, 0)          # outer device record
    b[0x20:0x30] = bytes.fromhex("6d80ec796fc07d45a74a7f5f405ee477")  # locate: custom
    struct.pack_into("<II", b, 0x30, 6, 0)
    b[0x38:0x48] = bytes.fromhex("5946d625e32c734ba77f417ad4ba511d")  # virtual disk (VHD)
    struct.pack_into("<IIII", b, 0x48, 0, 0, end - 0x48, 0)
    struct.pack_into("<III", b, 0x58, 5, 1, end - 0x5C)              # file device
    struct.pack_into("<III", b, 0x64, 5, 6, 0)                       # parent: partition
    struct.pack_into("<II", b, 0x70, 0x48, 0)
    struct.pack_into("<I", b, 0x8C, 1)                               # MBR (patched at boot)
    path = PATH_MARKER.encode("utf-16-le")
    b[0xB0:0xB0 + len(path)] = path
    return bytes(b)


def sz(s):
    return (1, (s + "\0").encode("utf-16-le"))


def multi(*items):
    return (7, ("\0".join(items) + "\0\0").encode("utf-16-le"))


def u64(v):
    return (3, struct.pack("<Q", v))


def boolean(v):
    return (3, bytes([1 if v else 0]))


def binary(b):
    return (3, b)


OBJECTS = {
    BOOTMGR: (0x10100002, {
        0x12000004: sz("Windows Boot Manager"),
        0x12000005: sz("en-US"),
        0x14000006: multi(GLOBALSETTINGS),
        0x23000003: sz(OSLOADER),                       # default
        0x24000001: multi(OSLOADER),                    # displayorder
        0x25000004: u64(0),                             # timeout
    }),
    OSLOADER: (0x10200003, {
        0x11000001: binary(vhd_device()),               # device
        0x12000002: sz("\\Windows\\system32\\winload.exe"),   # path
        0x12000004: sz("Windows (VHD, Glassboot)"),     # description
        0x12000005: sz("en-US"),
        0x14000006: multi(BOOTLOADERSETTINGS),
        0x17000077: binary(struct.pack("<Q", 0x15000075)),  # allowedinmemorysettings
        0x21000001: binary(vhd_device()),               # osdevice
        0x22000002: sz("\\Windows"),                    # systemroot
        0x25000020: u64(0),                             # nx OptIn
        0x250000C2: u64(1),                             # bootmenupolicy Standard
    }),
    GLOBALSETTINGS: (0x20100000, {0x14000006: multi(DBGSETTINGS, EMSSETTINGS, BADMEMORY)}),
    BOOTLOADERSETTINGS: (0x20200003, {0x14000006: multi(GLOBALSETTINGS, HYPERVISORSETTINGS)}),
    HYPERVISORSETTINGS: (0x20200003, {
        0x250000F3: u64(0), 0x250000F4: u64(1), 0x250000F5: u64(115200)}),
    DBGSETTINGS: (0x20100000, {0x15000011: u64(4)}),
    EMSSETTINGS: (0x20100000, {0x16000020: boolean(False)}),
    BADMEMORY: (0x20100000, {}),
}


def bcd_tree():
    """Registry tree: {name: (values, subkeys)}; values are {name: (type, data)}."""
    objects = {}
    for guid, (otype, elements) in OBJECTS.items():
        elems = {"%08x" % k: ({"Element": v}, {}) for k, v in elements.items()}
        objects[guid] = ({}, {
            "Description": ({"Type": (4, struct.pack("<I", otype))}, {}),
            "Elements": ({}, elems),
        })
    return ({}, {
        "Description": ({
            "KeyName": sz("BCD00000001"),
            "System": (4, struct.pack("<I", 1)),
            "TreatAsSystem": (4, struct.pack("<I", 1)),
        }, {}),
        "Objects": ({}, objects),
    })

# ------------------------------------------------------------------ regf writer


class Hive:
    """Minimal registry hive writer (format 1.5: "lh" subkey lists, one shared "sk")."""

    def __init__(self):
        # Offsets in a hive count from the start of the first hbin, whose cells begin
        # after its 32-byte header: reserve the header so offsets come out right.
        self.data = bytearray(32)

    def cell(self, payload):
        """Append an allocated cell; returns its offset (relative to the first hbin)."""
        size = (4 + len(payload) + 7) & ~7
        off = len(self.data)
        self.data += struct.pack("<i", -size) + payload + bytes(size - 4 - len(payload))
        return off

    def security(self):
        # Self-relative security descriptor: owner/group Administrators, DACL: Everyone
        # full access. (Unused at boot; the hive format needs one.)
        admins = bytes([1, 2, 0, 0, 0, 0, 0, 5]) + struct.pack("<II", 32, 544)
        everyone = bytes([1, 1, 0, 0, 0, 0, 0, 1]) + struct.pack("<I", 0)
        ace = struct.pack("<BBHI", 0, 0, 8 + len(everyone), 0x000F003F) + everyone
        acl = struct.pack("<BBHHH", 2, 0, 8 + len(ace), 1, 0) + ace
        hdr = 20
        sd = struct.pack("<BBHIIII", 1, 0, 0x8004, hdr, hdr + len(admins), 0,
                         hdr + 2 * len(admins)) + admins + admins + acl
        self.sk = len(self.data)
        payload = b"sk" + struct.pack("<HIIII", 0, self.sk, self.sk, 0, len(sd)) + sd
        self.cell(payload)

    def key(self, name, values, subkeys, parent, root=False):
        """Write a key (subkeys first, so their offsets are known); returns the nk offset."""
        nk = self.cell(bytes(0x4C + len(name)))          # reserve, filled in below
        # values
        voffs = []
        for vname, (vtype, vdata) in sorted(values.items(), key=lambda kv: kv[0].upper()):
            if len(vdata) <= 4:
                dsize, doff = len(vdata) | 0x80000000, struct.unpack("<I", vdata.ljust(4, b"\0"))[0]
            else:
                dsize, doff = len(vdata), self.cell(vdata)
            vn = vname.encode("ascii")
            voffs.append(self.cell(b"vk" + struct.pack("<HIIIHH", len(vn), dsize, doff, vtype, 1, 0) + vn))
        vlist = self.cell(b"".join(struct.pack("<I", o) for o in voffs)) if voffs else 0xFFFFFFFF
        # subkeys, sorted by upper-case name as Windows requires
        entries = []
        for sname in sorted(subkeys, key=str.upper):
            svals, ssubs = subkeys[sname]
            entries.append((self.key(sname, svals, ssubs, nk), sname))
        if entries:
            def lh_hash(s):
                h = 0
                for ch in s.upper():
                    h = (h * 37 + ord(ch)) & 0xFFFFFFFF
                return h
            slist = self.cell(b"lh" + struct.pack("<H", len(entries)) +
                              b"".join(struct.pack("<II", o, lh_hash(n)) for o, n in entries))
        else:
            slist = 0xFFFFFFFF
        nm = name.encode("ascii")
        max_sub = max((len(n) * 2 for _, n in entries), default=0)
        max_vname = max((len(n) * 2 for n in values), default=0)
        max_vdata = max((len(d) for _, d in values.values()), default=0)
        flags = 0x20 | (0x0C if root else 0)              # compressed name (+ hive root)
        # nk: flags, timestamp, access bits, parent, #subkeys, #volatile, subkey list,
        # volatile list, #values, value list, security, class, max subkey name, max class,
        # max value name, max value data, work var, name length, class length
        body = b"nk" + struct.pack("<HQ" + "I" * 15 + "HH", flags, 0, 0,
                                   parent if not root else 0, len(entries), 0, slist, 0xFFFFFFFF,
                                   len(voffs), vlist, self.sk, 0xFFFFFFFF,
                                   max_sub, 0, max_vname, max_vdata, 0, len(nm), 0) + nm
        self.data[nk + 4:nk + 4 + len(body)] = body
        return nk

    def save(self, path, tree):
        self.security()
        root = self.key("NewStoreRoot", tree[0], tree[1], 0, root=True)
        # Reference count of the shared security cell = number of keys.
        nkeys, i = 0, 32
        while i < len(self.data):
            size = -struct.unpack_from("<i", self.data, i)[0]
            if self.data[i + 4:i + 6] == b"nk":
                nkeys += 1
            i += size
        struct.pack_into("<I", self.data, self.sk + 16, nkeys)
        # One hbin, padded to a 4 KiB multiple with a free cell.
        hsize = (len(self.data) + 4095) & ~4095
        free = hsize - len(self.data)
        if free:
            self.data += struct.pack("<i", free) + bytes(free - 4)
        self.data[0:32] = b"hbin" + struct.pack("<IIQ", 0, hsize, 0) + bytes(12)
        base = bytearray(4096)
        struct.pack_into("<4sIIQIIIII", base, 0, b"regf", 1, 1, 0, 1, 5, 0, 1, root)
        struct.pack_into("<II", base, 0x28, hsize, 1)
        base[0x30:0x30 + 22] = "\\BCD (Glassboot)".encode("utf-16-le")[:22]
        csum = 0
        for k in range(0, 0x1FC, 4):
            csum ^= struct.unpack_from("<I", base, k)[0]
        struct.pack_into("<I", base, 0x1FC, csum)
        with open(path, "wb") as f:
            f.write(base + self.data)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make-bcd.py OUTPUT")
    Hive().save(sys.argv[1], bcd_tree())
