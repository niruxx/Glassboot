#!/usr/bin/env python3
"""Exercise the actual GRUB disk reader against qemu-img fixtures (Linux, gcc)."""
import pathlib
import shutil
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHIM = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <endian.h>
typedef uint8_t grub_uint8_t;
typedef uint16_t grub_uint16_t;
typedef uint32_t grub_uint32_t;
typedef uint64_t grub_uint64_t;
typedef size_t grub_size_t;
typedef ssize_t grub_ssize_t;
typedef int grub_err_t;
typedef FILE *grub_file_t;
#define GRUB_ERR_NONE 0
#define GRUB_ERR_BAD_FILE_TYPE 1
#define GRUB_ERR_FILE_READ_ERROR 2
#define GRUB_ERR_OUT_OF_RANGE 3
#define GRUB_FILE_TYPE_LOOPBACK 0
#define GRUB_FILE_TYPE_NO_DECOMPRESS 0
#define GRUB_MAX_ERRMSG 512
#define ALIGN_UP(x,a) (((x)+(a)-1)&~((a)-1))
#define grub_memcpy memcpy
#define grub_memcmp memcmp
#define grub_memset memset
#define grub_strncpy strncpy
#define grub_free free
#define grub_malloc malloc
#define grub_calloc calloc
#define grub_be_to_cpu32 be32toh
#define grub_be_to_cpu64 be64toh
#define grub_le_to_cpu16 le16toh
#define grub_le_to_cpu32 le32toh
#define grub_le_to_cpu64 le64toh
static int grub_errno;
static char grub_errmsg[512];
static int grub_error(int code, const char *fmt, ...) {
 va_list ap; va_start(ap,fmt); vsnprintf(grub_errmsg,512,fmt,ap); va_end(ap);
 return grub_errno=code;
}
static uint16_t grub_get_unaligned16(const void *p) {uint16_t x;memcpy(&x,p,2);return x;}
static uint32_t grub_get_unaligned32(const void *p) {uint32_t x;memcpy(&x,p,4);return x;}
static uint64_t grub_get_unaligned64(const void *p) {uint64_t x;memcpy(&x,p,8);return x;}
static void grub_set_unaligned32(void *p,uint32_t x) {memcpy(p,&x,4);}
static uint64_t grub_divmod64(uint64_t n,uint64_t d,uint64_t *r) {if(r)*r=n%d;return n/d;}
static FILE *grub_file_open(const char *p,int flags) {(void)flags;return fopen(p,"rb");}
static uint64_t grub_file_size(FILE *f) {off_t p=ftello(f);fseeko(f,0,SEEK_END);off_t n=ftello(f);fseeko(f,p,SEEK_SET);return n;}
static void grub_file_seek(FILE *f,uint64_t p) {fseeko(f,p,SEEK_SET);}
static ssize_t grub_file_read(FILE *f,void *p,size_t n) {return fread(p,1,n,f);}
#define grub_file_close fclose
static int has_suffix(const char *s,const char *suffix) {
 size_t a=strlen(s),b=strlen(suffix);return a>=b&&!strcasecmp(s+a-b,suffix);
}
'''
MAIN = r'''
int main(int argc,char **argv) {
 struct vimg v; unsigned char actual[8191], expected[8191];
 if(argc<3)return 2;
 if(vimg_open(&v,argv[1])) {
   fprintf(stderr,"%s\n",grub_errmsg); return !strcmp(argv[2],"reject")?0:1;
 }
 if(!strcmp(argv[2],"reject")){vimg_close(&v);return 1;}
 FILE *raw=fopen(argv[2],"rb"); if(!raw)return 2;
 if(v.size!=grub_file_size(raw))return 1;
 for(uint64_t off=0;off<v.size;) {
   size_t n=v.size-off<sizeof(actual)?v.size-off:sizeof(actual);
   if(vimg_read(&v,off,n,actual) || fread(expected,1,n,raw)!=n || memcmp(actual,expected,n)) {
     fprintf(stderr,"mismatch at %llu: %s\n",(unsigned long long)off,grub_errmsg);return 1;
   }
   off+=n;
 }
 if(!vimg_read(&v,v.size,1,actual))return 1;
 vimg_close(&v);fclose(raw);return 0;
}
'''


def run(*args):
    subprocess.run(args, check=True)


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


with tempfile.TemporaryDirectory(prefix="glassboot-vimg-") as work:
    work = pathlib.Path(work)
    source = (ROOT / "boot/grub-module/glassboot.c").read_text()
    reader = source[source.index("enum { VIMG_RAW"):source.index("/*\n * gb_vdisk FILE")]
    (work / "reader.c").write_text(SHIM + reader + MAIN)
    run("gcc", "-O1", "-g", "-fsanitize=address,undefined", "-o", str(work / "reader"), str(work / "reader.c"))
    raw = work / "raw.img"
    # Holes, sectors with data, and data crossing allocation boundaries.
    with raw.open("wb") as f:
        f.truncate(12 * 1024 * 1024)
        for off in [0, 511, 4093, 2 * 1024 * 1024 - 257, 8 * 1024 * 1024 + 31]:
            f.seek(off)
            f.write(bytes(range(256)) * 17)
    run(str(work / "reader"), str(raw), str(raw))
    for name, fmt, opts in [("fixed.vhd", "vpc", "subformat=fixed,force_size=on"),
                            ("dynamic.vhd", "vpc", "subformat=dynamic,force_size=on"),
                            ("dynamic.vhdx", "vhdx", "subformat=dynamic,block_size=1048576"),
                            ("fixed.vhdx", "vhdx", "subformat=fixed,block_size=1048576")]:
        image = work / name
        run("qemu-img", "convert", "-f", "raw", "-O", fmt, "-o", opts, str(raw), str(image))
        run(str(work / "reader"), str(image), str(raw))
        if name == "dynamic.vhd":
            expected = work / "bitmap-expected.img"
            shutil.copyfile(raw, expected)
            with expected.open("r+b") as f:
                f.write(bytes(512))
            with image.open("r+b") as f:
                f.seek(512 + 16)
                table = struct.unpack(">Q", f.read(8))[0]
                f.seek(table)
                block = struct.unpack(">I", f.read(4))[0] * 512
                f.seek(block)
                bits = f.read(1)[0]
                f.seek(block)
                f.write(bytes([bits & 0x7F]))
            run(str(work / "reader"), str(image), str(expected))
        if name == "fixed.vhd":
            with image.open("r+b") as f:
                f.seek(-512 + 64, 2)
                f.write(bytes(4))
            run(str(work / "reader"), str(image), "reject")
        if name == "fixed.vhdx":
            # Set a nonzero log GUID, preserving valid header CRCs.
            with image.open("r+b") as f:
                for off in [65536, 131072]:
                    f.seek(off)
                    header = bytearray(f.read(4096))
                    header[48] = 1
                    header[4:8] = bytes(4)
                    struct.pack_into("<I", header, 4, crc32c(header))
                    f.seek(off)
                    f.write(header)
            run(str(work / "reader"), str(image), "reject")
        if name == "dynamic.vhdx":
            # Primary region table corruption must recover through the second copy.
            with image.open("r+b") as f:
                f.seek(192 * 1024)
                f.write(b"BAD!")
            run(str(work / "reader"), str(image), str(raw))
            with image.open("r+b") as f:
                f.seek(256 * 1024)
                f.write(b"BAD!")
            run(str(work / "reader"), str(image), "reject")
    print("PASS: raw, fixed/dynamic VHD/VHDX, sector bitmap, checksums, dirty log, redundant regions, bounds (ASan/UBSan)")
