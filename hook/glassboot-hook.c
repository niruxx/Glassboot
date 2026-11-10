/* glassboot-hook: early-boot helper that makes an ISO file on the Glassboot data
 * partition appear as a block device, so a Linux distribution booted from that ISO finds
 * its installation media exactly as if it had been burned to a disc.
 *
 * How it runs
 * -----------
 * GRUB (boot/grub-module/glassboot.c) appends this program to the distribution's initrd
 * and adds to the kernel command line:
 *
 *     rdinit=/glassboot/init glassboot.iso=/path/to/file.iso glassboot.part=1a2b3c4d-01
 *
 * The kernel then starts us as PID 1 instead of the distribution's /init. We fork a
 * worker and immediately exec the real /init, so the distribution boots normally. The
 * worker, in its own mount namespace (it never touches the distribution's mounts):
 *
 *   1. waits for the USB disk whose MBR signature/partition number or GPT partition GUID matches
 *      glassboot.part (it appears once the distribution's udev loads the USB drivers),
 *   2. reads the exFAT file system on that partition and finds the ISO's clusters,
 *   3. maps those sectors as a read-only block device: a loop device with offset + size
 *      limit when the file is contiguous (the normal case; works with every distro's
 *      generic udev rules and leaves the USB disk usable), otherwise a device-mapper
 *      "linear" device that stitches the fragments together,
 *   4. links /dev/glassboot to it and nudges udev with "change" events so
 *      /dev/disk/by-label etc. are created.
 *
 * Everything is logged to the kernel log (dmesg | grep glassboot).
 *
 * Copyright (C) 2026 Glassboot contributors. GPL-3.0-or-later.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/loop.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef MODULE_INIT_COMPRESSED_FILE
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

#define BASE "/glassboot"
#define DEV BASE "/dev"
#define SYS BASE "/sys"
#define PROC BASE "/proc"
#define DM_NAME "glassboot"
#define WAIT_SECONDS 120

/* ------------------------------------------------------------------ logging */

static void logmsg(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(buf, sizeof buf, "<5>glassboot: ");
    vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    va_end(ap);
    int fd = open(DEV "/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, buf, strlen(buf)) < 0) { /* nothing useful to do */ }
        close(fd);
    }
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static int read_text(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return 0;
}

static void write_text(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, s, strlen(s)) < 0) { /* ignore */ }
        close(fd);
    }
}

/* ------------------------------------------------------------------ command line */

static char opt_iso[4096];
static char opt_persist[4096]; /* optional persistence file (glassboot.cfg "persist") */
static uint32_t opt_sig;
static unsigned opt_partnum;
static uint8_t opt_guid[16];
static int opt_gpt;

/* UUID strings use big-endian fields; GPT stores the first three little-endian. */
static int parse_guid(const char *text, uint8_t out[16]) {
    static const unsigned order[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    if (strlen(text) != 36) return -1;
    unsigned at = 0;
    for (unsigned i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i++] != '-') return -1;
        } else {
            if (!isxdigit((unsigned char)text[i]) || !isxdigit((unsigned char)text[i+1])) return -1;
            char hex[3] = {text[i], text[i+1], 0};
            out[order[at++]] = (uint8_t)strtoul(hex, NULL, 16);
            i += 2;
        }
    }
    return 0;
}

static void percent_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static int parse_cmdline(void) {
    static char cmd[8192];
    if (read_text(PROC "/cmdline", cmd, sizeof cmd) != 0) return -1;
    int have_iso = 0, have_part = 0;
    for (char *tok = strtok(cmd, " "); tok; tok = strtok(NULL, " ")) {
        if (!strncmp(tok, "glassboot.iso=", 14)) {
            snprintf(opt_iso, sizeof opt_iso, "%s", tok + 14);
            percent_decode(opt_iso);
            have_iso = 1;
        } else if (!strncmp(tok, "glassboot.persist=", 18)) {
            snprintf(opt_persist, sizeof opt_persist, "%s", tok + 18);
            percent_decode(opt_persist);
        } else if (!strncmp(tok, "glassboot.part=", 15)) {
            const char *uuid = tok + 15;
            if (parse_guid(uuid, opt_guid) == 0) {
                opt_gpt = 1;
                have_part = 1;
            } else if (strlen(uuid) == 11 && uuid[8] == '-') {
                int valid = 1;
                for (int i = 0; i < 11; i++)
                    if (i != 8 && !isxdigit((unsigned char)uuid[i])) valid = 0;
                if (valid) {
                    opt_sig = (uint32_t)strtoul(uuid, NULL, 16);
                    opt_partnum = (unsigned)strtoul(uuid + 9, NULL, 16);
                    opt_gpt = 0;
                    have_part = opt_partnum >= 1 && opt_partnum <= 4;
                }
            }
        }
    }
    return (have_iso && have_part) ? 0 : -1;
}

/* ------------------------------------------------------------------ kernel modules */

static int module_loaded(const char *name) {
    char p[256];
    snprintf(p, sizeof p, SYS "/module/%s", name);
    struct stat st;
    return stat(p, &st) == 0;
}

/* Find <file>.ko[.xz|.zst|.gz] under DIR (recursive). */
static int find_module(const char *dir, const char *file, char *out, size_t outlen, int depth) {
    if (depth > 8) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    size_t flen = strlen(file);
    while (found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            found = find_module(p, file, out, outlen, depth + 1);
        } else if (!strncmp(e->d_name, file, flen) && e->d_name[flen] == '.' &&
                   !strncmp(e->d_name + flen, ".ko", 3)) {
            snprintf(out, outlen, "%s", p);
            found = 0;
        }
    }
    closedir(d);
    return found;
}

/* Load a module from the distribution's initrd. Uses in-kernel decompression for
 * compressed modules (Linux 5.17+). Returns 0 if the module is available afterwards. */
static int load_module(const char *sysname, const char *file) {
    if (module_loaded(sysname)) return 0;
    struct utsname u;
    if (uname(&u) != 0) return -1;
    const char *roots[] = {"/lib/modules", "/usr/lib/modules"};
    char path[1024];
    for (size_t i = 0; i < 2; i++) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/%s", roots[i], u.release);
        if (find_module(dir, file, path, sizeof path, 0) != 0) continue;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        int compressed = strstr(path, ".ko.") != NULL;
        long r = syscall(SYS_finit_module, fd, "", compressed ? MODULE_INIT_COMPRESSED_FILE : 0);
        int err = errno;
        close(fd);
        if (r == 0 || err == EEXIST) {
            logmsg("loaded %s\n", path);
            return 0;
        }
        logmsg("could not load %s: %s\n", path, strerror(err));
    }
    return module_loaded(sysname) ? 0 : -1;
}

/* ------------------------------------------------------------------ finding the disk */

struct disk {
    char name[64];
    int fd;
    dev_t devnum;
    uint64_t part_start; /* in 512-byte sectors */
    uint64_t part_len;
};

static int open_devnode(const char *name, int flags) {
    char p[256];
    snprintf(p, sizeof p, DEV "/%s", name);
    return open(p, flags | O_CLOEXEC);
}

static uint64_t le64(const uint8_t *p);
static uint32_t le32(const uint8_t *p);
static int preadn(int fd, void *buf, size_t len, uint64_t off);
static uint32_t gpt_crc(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
/* Match the unique partition GUID, never the disk GUID or a filesystem label. */
static int gpt_partition(int fd, const uint8_t *guid, unsigned pn, uint64_t *start, uint64_t *len) {
    uint8_t header[512], entries[128 * 128];
    if (preadn(fd, header, sizeof header, 512) != 0 || memcmp(header, "EFI PART", 8) ||
        le32(header + 12) != 92 || le64(header + 24) != 1 || le64(header + 72) != 2 ||
        le32(header + 80) != 128 || le32(header + 84) != 128) return -1;
    uint32_t sum = le32(header + 16);
    memset(header + 16, 0, 4);
    if (gpt_crc(header, 92) != sum || preadn(fd, entries, sizeof entries, 1024) != 0 ||
        gpt_crc(entries, sizeof entries) != le32(header + 88)) return -1;
    for (unsigned i = 0; i < 128; i++) {
        const uint8_t *e = entries + i * 128;
        if (guid ? memcmp(e + 16, guid, 16) != 0 : i + 1 != pn) continue;
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (first < le64(header + 40) || last < first || last > le64(header + 48)) return -1;
        *start = first; *len = last - first + 1;
        return 0;
    }
    return -1;
}

static int find_disk(struct disk *out) {
    DIR *d = opendir(SYS "/block");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while (found && (e = readdir(d))) {
        const char *n = e->d_name;
        if (n[0] == '.' || !strncmp(n, "loop", 4) || !strncmp(n, "ram", 3) || !strncmp(n, "dm-", 3) ||
            !strncmp(n, "zram", 4) || !strncmp(n, "sr", 2) || !strncmp(n, "fd", 2))
            continue;
        int fd = open_devnode(n, O_RDONLY);
        if (fd < 0) continue;
        uint8_t mbr[512];
        if (opt_gpt) {
            uint64_t start, len;
            struct stat st;
            if (gpt_partition(fd, opt_guid, 0, &start, &len) == 0 && fstat(fd, &st) == 0) {
                snprintf(out->name, sizeof out->name, "%s", n);
                out->fd = fd; out->devnum = st.st_rdev;
                out->part_start = start; out->part_len = len;
                found = 0;
                continue;
            }
            close(fd);
            continue;
        }
        if (pread(fd, mbr, 512, 0) == 512 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
            uint32_t sig = (uint32_t)mbr[0x1B8] | (uint32_t)mbr[0x1B9] << 8 | (uint32_t)mbr[0x1BA] << 16 |
                           (uint32_t)mbr[0x1BB] << 24;
            if (sig == opt_sig && opt_partnum >= 1 && opt_partnum <= 4) {
                const uint8_t *pe = mbr + 0x1BE + 16 * (opt_partnum - 1);
                uint32_t start = (uint32_t)pe[8] | (uint32_t)pe[9] << 8 | (uint32_t)pe[10] << 16 | (uint32_t)pe[11] << 24;
                uint32_t len = (uint32_t)pe[12] | (uint32_t)pe[13] << 8 | (uint32_t)pe[14] << 16 | (uint32_t)pe[15] << 24;
                struct stat st;
                if (start && len && fstat(fd, &st) == 0) {
                    snprintf(out->name, sizeof out->name, "%s", n);
                    out->fd = fd;
                    out->devnum = st.st_rdev;
                    out->part_start = start;
                    out->part_len = len;
                    found = 0;
                    continue;
                }
            }
        }
        close(fd);
    }
    closedir(d);
    return found;
}

/* ------------------------------------------------------------------ exFAT */

struct exfat {
    int fd;
    uint64_t base;          /* partition start, bytes */
    unsigned bps_shift;     /* bytes per sector */
    unsigned spc_shift;     /* sectors per cluster */
    uint64_t fat_off;       /* bytes from partition start */
    uint64_t heap_off;      /* bytes from partition start */
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint64_t cluster_bytes;
    /* FAT read cache */
    uint8_t *cache;
    uint64_t cache_first;   /* first FAT entry in cache */
    uint32_t cache_entries;
};

#define FAT_CACHE_ENTRIES (256 * 1024)

static uint64_t le64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int preadn(int fd, void *buf, size_t len, uint64_t off) {
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= (size_t)n; off += (uint64_t)n;
    }
    return 0;
}

static int exfat_open(struct exfat *fs, int fd, uint64_t part_start_sectors) {
    uint8_t bs[512];
    memset(fs, 0, sizeof *fs);
    fs->fd = fd;
    fs->base = part_start_sectors * 512;
    if (preadn(fd, bs, 512, fs->base) != 0) return -1;
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0) { logmsg("data partition is not exFAT\n"); return -1; }
    fs->bps_shift = bs[108];
    fs->spc_shift = bs[109];
    if (fs->bps_shift < 9 || fs->bps_shift > 12 || fs->spc_shift > 25 - fs->bps_shift) return -1;
    fs->fat_off = (uint64_t)le32(bs + 80) << fs->bps_shift;
    fs->heap_off = (uint64_t)le32(bs + 88) << fs->bps_shift;
    fs->cluster_count = le32(bs + 92);
    fs->root_cluster = le32(bs + 96);
    fs->cluster_bytes = 1ULL << (fs->bps_shift + fs->spc_shift);
    fs->cache = malloc((size_t)FAT_CACHE_ENTRIES * 4);
    return fs->cache ? 0 : -1;
}

static int fat_next(struct exfat *fs, uint32_t c, uint32_t *next) {
    if (!fs->cache_entries || c < fs->cache_first || c >= fs->cache_first + fs->cache_entries) {
        uint64_t total = (uint64_t)fs->cluster_count + 2;
        fs->cache_first = c;
        fs->cache_entries = (uint32_t)((total - c) < FAT_CACHE_ENTRIES ? (total - c) : FAT_CACHE_ENTRIES);
        if (preadn(fs->fd, fs->cache, (size_t)fs->cache_entries * 4, fs->base + fs->fat_off + (uint64_t)c * 4) != 0)
            return -1;
    }
    *next = le32(fs->cache + (size_t)(c - fs->cache_first) * 4);
    return 0;
}

static uint64_t cluster_offset(const struct exfat *fs, uint32_t c) {
    return fs->heap_off + (uint64_t)(c - 2) * fs->cluster_bytes; /* from partition start */
}

/* A run of contiguous clusters. */
struct run { uint64_t off; uint64_t len; }; /* bytes, relative to partition start */

struct runs { struct run *v; size_t n, cap; };

static int runs_add(struct runs *r, uint64_t off, uint64_t len) {
    if (r->n && r->v[r->n - 1].off + r->v[r->n - 1].len == off) { r->v[r->n - 1].len += len; return 0; }
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 16;
        struct run *v = realloc(r->v, cap * sizeof *v);
        if (!v) return -1;
        r->v = v; r->cap = cap;
    }
    r->v[r->n++] = (struct run){off, len};
    return 0;
}

/* Clusters of a file/directory as runs, trimmed to `length` bytes. */
static int file_runs(struct exfat *fs, uint32_t first, int no_fat_chain, uint64_t length, struct runs *out) {
    out->n = 0;
    if (!first || !length) return 0;
    if (no_fat_chain) return runs_add(out, cluster_offset(fs, first), length);
    uint64_t left = length;
    uint32_t c = first;
    uint64_t guard = (uint64_t)fs->cluster_count + 1;
    while (left) {
        if (c < 2 || c >= fs->cluster_count + 2 || !guard--) { logmsg("broken cluster chain\n"); return -1; }
        uint64_t n = left < fs->cluster_bytes ? left : fs->cluster_bytes;
        if (runs_add(out, cluster_offset(fs, c), n) != 0) return -1;
        left -= n;
        if (!left) break;
        if (fat_next(fs, c, &c) != 0) return -1;
    }
    return 0;
}

static int read_runs(struct exfat *fs, const struct runs *r, uint8_t *buf) {
    uint64_t pos = 0;
    for (size_t i = 0; i < r->n; i++) {
        if (preadn(fs->fd, buf + pos, (size_t)r->v[i].len, fs->base + r->v[i].off) != 0) return -1;
        pos += r->v[i].len;
    }
    return 0;
}

/* UTF-8 path component -> UTF-16 with ASCII case folding for comparison. */
static size_t utf8_to_utf16_upper(const char *s, uint16_t *out, size_t max) {
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < max) {
        uint32_t cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = (uint32_t)(*p & 0x1F) << 6 | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = (uint32_t)(*p & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = (uint32_t)(*p & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            p += 4;
            if (n + 2 > max) break;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            continue;
        } else return 0;
        out[n++] = (uint16_t)((cp >= 'a' && cp <= 'z') ? cp - 32 : cp);
    }
    return n;
}

struct dirent_info { uint32_t first; int no_fat_chain; uint64_t length; int is_dir; };

/* Look up `name` in the directory whose clusters are `dir`. */
static int dir_lookup(struct exfat *fs, const struct runs *dir, uint64_t dir_len, const char *name,
                      struct dirent_info *res) {
    uint16_t want[256];
    size_t wlen = utf8_to_utf16_upper(name, want, 255);
    if (!wlen) return -1;
    uint8_t *buf = malloc((size_t)dir_len);
    if (!buf) return -1;
    if (read_runs(fs, dir, buf) != 0) { free(buf); return -1; }
    int found = -1;
    for (uint64_t i = 0; i + 32 <= dir_len && found; i += 32) {
        uint8_t *e = buf + i;
        if (e[0] == 0x00) break;    /* end of directory */
        if (e[0] != 0x85) continue; /* only in-use File entries start a set */
        unsigned secondary = e[1];
        if (secondary < 2 || i + 32 * (uint64_t)(secondary + 1) > dir_len) continue;
        uint8_t *st = e + 32;
        if (st[0] != 0xC0) continue;
        unsigned name_len = st[3];
        if (name_len != wlen) { i += 32 * (uint64_t)secondary; continue; }
        uint16_t got[256];
        size_t g = 0;
        for (unsigned k = 0; k + 2 <= secondary && g < name_len; k++) {
            uint8_t *ne = e + 64 + 32 * k;
            if (ne[0] != 0xC1) break;
            for (int j = 0; j < 15 && g < name_len; j++) {
                uint16_t ch = le16(ne + 2 + 2 * j);
                got[g++] = (ch >= 'a' && ch <= 'z') ? (uint16_t)(ch - 32) : ch;
            }
        }
        if (g == wlen && !memcmp(got, want, wlen * 2)) {
            res->is_dir = (le16(e + 4) & 0x10) != 0;
            res->no_fat_chain = (st[1] & 0x02) != 0;
            res->first = le32(st + 20);
            res->length = le64(st + 24);
            found = 0;
        }
        i += 32 * (uint64_t)secondary;
    }
    free(buf);
    return found;
}

/* Resolve an absolute path to the file's runs. */
static int exfat_find(struct exfat *fs, const char *path, struct runs *out, uint64_t *size) {
    /* Root directory: FAT chain, length unknown -> walk the chain to measure it. */
    struct runs dir = {0};
    uint64_t dir_len = 0;
    {
        uint32_t c = fs->root_cluster;
        uint64_t guard = 65536;
        while (c >= 2 && c < fs->cluster_count + 2 && guard--) {
            if (runs_add(&dir, cluster_offset(fs, c), fs->cluster_bytes) != 0) return -1;
            dir_len += fs->cluster_bytes;
            if (fat_next(fs, c, &c) != 0) return -1;
        }
    }
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", path);
    char *comps[128];
    size_t ncomp = 0;
    char *save = NULL;
    for (char *c = strtok_r(copy, "/", &save); c && ncomp < 128; c = strtok_r(NULL, "/", &save)) comps[ncomp++] = c;

    struct dirent_info info = {0};
    int ok = ncomp ? 0 : -1;
    for (size_t i = 0; i < ncomp && ok == 0; i++) {
        if (dir_lookup(fs, &dir, dir_len, comps[i], &info) != 0) { logmsg("not found: %s\n", comps[i]); ok = -1; break; }
        int last = i + 1 == ncomp;
        if (last) { if (info.is_dir) ok = -1; break; }
        if (!info.is_dir) { ok = -1; break; }
        if (file_runs(fs, info.first, info.no_fat_chain, info.length, &dir) != 0) { ok = -1; break; }
        dir_len = info.length;
    }
    free(dir.v);
    if (ok != 0) return -1;
    *size = info.length;
    return file_runs(fs, info.first, info.no_fat_chain, info.length, out);
}

/* ------------------------------------------------------------------ device-mapper */

static int dm_ioctl_call(int ctl, unsigned long cmd, struct dm_ioctl *io) {
    io->version[0] = DM_VERSION_MAJOR;
    io->version[1] = 0;
    io->version[2] = 0;
    return ioctl(ctl, cmd, io);
}

/* Returns the dm minor number, or -1. */
/* NAME: device-mapper name; WHAT: file path for logs; WRITABLE: read-write mapping. */
static int dm_create(const struct disk *disk, const struct runs *r, uint64_t size, const char *name,
                     const char *what, int writable) {
    int ctl = open(DEV "/mapper/control", O_RDWR | O_CLOEXEC);
    if (ctl < 0) {
        /* devtmpfs normally creates it; fall back to mknod from sysfs. */
        char devno[32];
        if (read_text(SYS "/class/misc/device-mapper/dev", devno, sizeof devno) == 0) {
            unsigned ma, mi;
            if (sscanf(devno, "%u:%u", &ma, &mi) == 2) {
                mkdir(DEV "/mapper", 0755);
                mknod(DEV "/mapper/control", S_IFCHR | 0600, makedev(ma, mi));
                ctl = open(DEV "/mapper/control", O_RDWR | O_CLOEXEC);
            }
        }
    }
    if (ctl < 0) { logmsg("no device-mapper control node\n"); return -1; }

    /* Table: one linear target per run, in 512-byte sectors. */
    size_t cap = sizeof(struct dm_ioctl) + r->n * (sizeof(struct dm_target_spec) + 64) + 64;
    uint8_t *buf = calloc(1, cap);
    if (!buf) { close(ctl); return -1; }
    struct dm_ioctl *io = (struct dm_ioctl *)buf;
    snprintf(io->name, sizeof io->name, "%s", name);
    snprintf(io->uuid, sizeof io->uuid, "GLASSBOOT-%s", name);
    io->data_size = sizeof *io;
    if (dm_ioctl_call(ctl, DM_DEV_CREATE, io) != 0) {
        logmsg("DM_DEV_CREATE: %s\n", strerror(errno));
        free(buf); close(ctl); return -1;
    }

    memset(buf, 0, cap);
    snprintf(io->name, sizeof io->name, "%s", name);
    io->data_start = sizeof *io;
    io->flags = writable ? 0 : DM_READONLY_FLAG;
    size_t pos = sizeof *io;
    uint64_t sector = 0, left = (size + 511) / 512;
    for (size_t i = 0; i < r->n && left; i++) {
        struct dm_target_spec *t = (struct dm_target_spec *)(buf + pos);
        uint64_t len = r->v[i].len / 512;
        if (len > left) len = left;
        t->sector_start = sector;
        t->length = len;
        snprintf(t->target_type, sizeof t->target_type, "linear");
        char *params = (char *)(t + 1);
        int pl = snprintf(params, 64, "%u:%u %llu", major(disk->devnum), minor(disk->devnum),
                          (unsigned long long)(disk->part_start + (r->v[i].off / 512)));
        size_t next = sizeof *t + (size_t)pl + 1;
        next = (next + 7) & ~(size_t)7;
        t->next = (uint32_t)next;
        pos += next;
        sector += len;
        left -= len;
        io->target_count++;
    }
    io->data_size = (uint32_t)pos;
    if (dm_ioctl_call(ctl, DM_TABLE_LOAD, io) != 0) {
        logmsg("DM_TABLE_LOAD: %s\n", strerror(errno));
        free(buf); close(ctl); return -1;
    }

    memset(buf, 0, sizeof *io);
    snprintf(io->name, sizeof io->name, "%s", name);
    io->data_size = sizeof *io;
    /* udev cookie flags (libdevmapper's encoding: flags in the top 16 bits of event_nr).
     * DM_UDEV_PRIMARY_SOURCE_FLAG makes lvm2's 10-dm.rules treat the resume event as
     * genuine, so blkid runs and /dev/disk/by-label etc. are created for the device. */
    io->event_nr = 0x0040u << 16;
    if (dm_ioctl_call(ctl, DM_DEV_SUSPEND, io) != 0) { /* resume */
        logmsg("DM_DEV_SUSPEND (resume): %s\n", strerror(errno));
        free(buf); close(ctl); return -1;
    }
    int dm_minor = (int)minor(io->dev);
    logmsg("mapped %s as /dev/dm-%d (%zu segment%s, %llu bytes, %s)\n", what, dm_minor, r->n,
           r->n == 1 ? "" : "s", (unsigned long long)size, writable ? "read-write" : "read-only");
    free(buf);
    close(ctl);
    return dm_minor;
}

/* ------------------------------------------------------------------ loop fallback */

static int loop_create(const struct disk *disk, const struct runs *r, uint64_t size, const char *what,
                       int writable) {
    if (r->n != 1) { logmsg("%s is fragmented (%zu pieces) and dm-mod is unavailable\n", what, r->n); return -1; }
    int ctl = open(DEV "/loop-control", O_RDWR | O_CLOEXEC);
    if (ctl < 0) { logmsg("no loop-control\n"); return -1; }
    int n = ioctl(ctl, LOOP_CTL_GET_FREE);
    close(ctl);
    if (n < 0) return -1;
    char name[64];
    snprintf(name, sizeof name, DEV "/loop%d", n);
    int lfd = -1;
    for (int i = 0; i < 20 && lfd < 0; i++) { /* devtmpfs may lag a moment */
        lfd = open(name, O_RDWR | O_CLOEXEC);
        if (lfd < 0) sleep_ms(50);
    }
    char diskpath[128];
    snprintf(diskpath, sizeof diskpath, DEV "/%s", disk->name);
    int bfd = open(diskpath, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (lfd < 0 || bfd < 0) { if (lfd >= 0) close(lfd); if (bfd >= 0) close(bfd); return -1; }

    struct loop_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fd = (uint32_t)bfd;
    cfg.info.lo_offset = disk->part_start * 512 + r->v[0].off;
    cfg.info.lo_sizelimit = size;
    cfg.info.lo_flags = writable ? 0 : LO_FLAGS_READ_ONLY;
    snprintf((char *)cfg.info.lo_file_name, LO_NAME_SIZE, "glassboot:%s", what);
    int rc = ioctl(lfd, LOOP_CONFIGURE, &cfg);
    if (rc != 0) { /* kernels before 5.8 */
        rc = ioctl(lfd, LOOP_SET_FD, bfd);
        if (rc == 0) rc = ioctl(lfd, LOOP_SET_STATUS64, &cfg.info);
    }
    close(bfd);
    close(lfd);
    if (rc != 0) { logmsg("loop setup failed: %s\n", strerror(errno)); return -1; }
    logmsg("mapped %s as /dev/loop%d (%s)\n", what, n, writable ? "read-write" : "read-only");
    return n;
}

/* ------------------------------------------------------------------ worker */

static void nudge_udev(char sysblocks[][32], int count) {
    /* Repeat for a while: udevd may not be running yet when we create the devices. */
    static const int at[] = {0, 500, 1000, 2000, 3000, 5000, 8000, 12000, 20000, 30000};
    int prev = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        sleep_ms(at[i] - prev);
        prev = at[i];
        for (int k = 0; k < count; k++) {
            char p[256];
            snprintf(p, sizeof p, SYS "/block/%s/uevent", sysblocks[k]);
            write_text(p, "change");
        }
    }
}

/* /dev/disk/by-label/LABEL and by-uuid/UUID for an ext2/3/4 file system on SYSBLOCK.
 * udev makes these for loop devices everywhere, but for device-mapper only with lvm2's
 * rules, which many initramfs images (e.g. Arch's) lack, and their init waits for the
 * link (archiso cow_label=, dracut rd.live.overlay=LABEL=). devtmpfs is one instance, so
 * the links are visible in the distribution's /dev. Escaping follows udev. */
static void fs_links(const char *sysblock) {
    uint8_t sb[1024];
    int fd = open_devnode(sysblock, O_RDONLY);
    if (fd < 0) return;
    int ok = preadn(fd, sb, sizeof sb, 1024) == 0;
    close(fd);
    if (!ok || le16(sb + 0x38) != 0xEF53) return;

    char label[16 * 4 + 1] = "", uuid[37];
    size_t o = 0;
    for (int i = 0; i < 16 && sb[0x78 + i]; i++) {
        uint8_t c = sb[0x78 + i];
        if (isalnum(c) || strchr("#+-.:=@_", c)) label[o++] = (char)c;
        else o += (size_t)snprintf(label + o, sizeof label - o, "\\x%02x", c);
    }
    label[o] = 0;
    const uint8_t *u = sb + 0x68;
    snprintf(uuid, sizeof uuid, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);

    char target[64], p[256];
    snprintf(target, sizeof target, "../../%s", sysblock);
    mkdir(DEV "/disk", 0755);
    mkdir(DEV "/disk/by-label", 0755);
    mkdir(DEV "/disk/by-uuid", 0755);
    if (label[0]) {
        snprintf(p, sizeof p, DEV "/disk/by-label/%s", label);
        if (symlink(target, p) == 0) logmsg("/dev/disk/by-label/%s -> %s\n", label, sysblock);
    }
    snprintf(p, sizeof p, DEV "/disk/by-uuid/%s", uuid);
    if (symlink(target, p) != 0 && errno != EEXIST) logmsg("cannot link %s: %s\n", p, strerror(errno));
}

/* Map a file's runs as a block device: a loop device when contiguous, otherwise
 * device-mapper. Fills SYSBLOCK ("loop0" / "dm-1"); returns 0 on success.
 *
 * Loop is preferred for contiguous files (the normal case): generic udev rules create
 * /dev/disk/by-label|by-uuid links for loop devices in every distribution, and a loop
 * device does not hold the USB disk exclusively, so the distribution can still mount the
 * data partition. Device-mapper handles fragmented files, but claims the whole disk and
 * needs lvm2's udev rules for the by-* links.
 *
 * PREFER_DM reverses the order. Persistence files need it: live-boot and casper look for
 * persistence by scanning /sys/block and skip every loop device. */
static int map_file(const struct disk *disk, const struct runs *runs, uint64_t size, const char *dmname,
                    const char *what, int writable, int prefer_dm, char sysblock[32]) {
    int n = -1, pass;
    for (pass = 0; pass < 2 && n < 0; pass++) {
        int use_dm = (pass == 0) == (prefer_dm != 0);
        if (use_dm) {
            if (load_module("dm_mod", "dm-mod") == 0) {
                n = dm_create(disk, runs, size, dmname, what, writable);
                if (n >= 0) snprintf(sysblock, 32, "dm-%d", n);
            }
        } else if (runs->n == 1) {
            load_module("loop", "loop");
            n = loop_create(disk, runs, size, what, writable);
            if (n >= 0) snprintf(sysblock, 32, "loop%d", n);
        }
    }
    return n < 0 ? -1 : 0;
}

static void worker(void) {
    /* Private mounts: nothing we do is visible to the distribution's init. */
    if (unshare(CLONE_NEWNS) != 0) _exit(1);
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    mkdir(PROC, 0755); mkdir(SYS, 0755); mkdir(DEV, 0755);
    if (mount("proc", PROC, "proc", 0, NULL) != 0 || mount("sysfs", SYS, "sysfs", 0, NULL) != 0 ||
        mount("devtmpfs", DEV, "devtmpfs", 0, NULL) != 0)
        _exit(1);

    if (parse_cmdline() != 0) { logmsg("missing glassboot.iso / glassboot.part\n"); _exit(1); }
    logmsg("looking for %s (%s partition identifier)\n", opt_iso, opt_gpt ? "GPT" : "MBR");

    struct disk disk;
    int tries = 0;
    while (find_disk(&disk) != 0) {
        if (++tries > WAIT_SECONDS * 4) { logmsg("Glassboot USB drive not found\n"); _exit(1); }
        sleep_ms(250);
    }
    logmsg("found drive /dev/%s, data partition at sector %llu\n", disk.name,
           (unsigned long long)disk.part_start);

    struct exfat fs;
    struct runs runs = {0};
    uint64_t size = 0;
    if (exfat_open(&fs, disk.fd, disk.part_start) != 0 || exfat_find(&fs, opt_iso, &runs, &size) != 0) {
        logmsg("could not locate %s\n", opt_iso);
        _exit(1);
    }

    char sysblocks[2][32];
    int ndev = 0;
    if (map_file(&disk, &runs, size, DM_NAME, opt_iso, 0, 0, sysblocks[ndev]) != 0) {
        logmsg("could not create a device for the ISO\n");
        _exit(1);
    }
    /* A stable name for kernel arguments such as live-media=/dev/glassboot. devtmpfs is
     * a single instance, so the link is visible in the distribution's /dev too. */
    unlink(DEV "/glassboot");
    if (symlink(sysblocks[ndev], DEV "/glassboot") == 0) logmsg("/dev/glassboot -> %s\n", sysblocks[ndev]);
    ndev++;

    /* Persistence file (glassboot.cfg "persist"): mapped read-write, through
     * device-mapper (see map_file). The distribution finds it by its file-system label
     * (casper-rw, persistence, ...) as if it were a partition. */
    if (opt_persist[0]) {
        struct runs pruns = {0};
        uint64_t psize = 0;
        if (exfat_find(&fs, opt_persist, &pruns, &psize) != 0) {
            logmsg("persistence file %s not found\n", opt_persist);
        } else if (map_file(&disk, &pruns, psize, DM_NAME "-persist", opt_persist, 1, 1, sysblocks[ndev]) == 0) {
            unlink(DEV "/glassboot-persist");
            if (symlink(sysblocks[ndev], DEV "/glassboot-persist") == 0)
                logmsg("/dev/glassboot-persist -> %s\n", sysblocks[ndev]);
            if (strncmp(sysblocks[ndev], "dm-", 3) == 0) fs_links(sysblocks[ndev]);
            ndev++;
        } else {
            logmsg("could not map persistence file %s\n", opt_persist);
        }
    }
    nudge_udev(sysblocks, ndev);
    _exit(0);
}

/* Developer test: glassboot-hook --test DISK_OR_IMAGE PARTNUM /path/in/exfat.iso
 * Prints the sector runs the worker would map; changes nothing. */
static int test_mode(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s --test DISK PARTNUM PATH\n", argv[0]); return 2; }
    int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
    uint8_t mbr[512];
    if (fd < 0 || preadn(fd, mbr, 512, 0) != 0) { perror(argv[2]); return 1; }
    unsigned pn = (unsigned)atoi(argv[3]);
    uint64_t start = 0, plen = 0;
    if (mbr[450] == 0xEE) {
        if (pn < 1 || pn > 128 || gpt_partition(fd, NULL, pn, &start, &plen) != 0) return 2;
    } else {
        if (pn < 1 || pn > 4) return 2;
        start = le32(mbr + 0x1BE + 16 * (pn - 1) + 8);
    }
    struct exfat fs;
    struct runs runs = {0};
    uint64_t size = 0;
    if (exfat_open(&fs, fd, start) != 0 || exfat_find(&fs, argv[4], &runs, &size) != 0) {
        fprintf(stderr, "not found\n");
        return 1;
    }
    printf("size %llu bytes, %zu run(s)\n", (unsigned long long)size, runs.n);
    for (size_t i = 0; i < runs.n; i++)
        printf("  disk sector %llu, %llu sectors\n", (unsigned long long)(start + runs.v[i].off / 512),
               (unsigned long long)(runs.v[i].len / 512));
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--test")) return test_mode(argc, argv);
    if (getpid() == 1) {
        pid_t pid = fork();
        if (pid == 0) worker();
        /* Parent: become the distribution's init. */
        argv[0] = "/init";
        execv("/init", argv);
        execv("/sbin/init", argv);
        logmsg("cannot start /init: %s\n", strerror(errno));
        return 1;
    }
    /* Not PID 1 (manual testing): run the worker in the foreground. */
    worker();
    return 0;
}
