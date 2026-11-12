/* glassboot.c - Glassboot helpers for GRUB.
 *
 * Built into Glassboot's GRUB (see scripts/build.sh). Two features:
 *
 *  gb_scan DEVICE
 *      Recursively finds bootable files on DEVICE (e.g. "(hd0,msdos1)"): *.iso, *.wim,
 *      *.img and *.vhd everywhere, plus *.efi on UEFI. Publishes a GRUB script with one
 *      menu entry per file as (proc)/glassboot_menu. Sets gb_iso_count.
 *
 *  gb_wimboot_efi, gb_efi_vdisk_boot (UEFI only): see below.
 *
 *  gb_hook_enable ISO_PATH PART_UUID INITRD
 *  gb_hook_disable
 *      While enabled, every `linux` command (also linuxefi/linux16) gets extra kernel
 *      arguments, and every `initrd` command gets INITRD appended:
 *
 *          rdinit=/glassboot/init glassboot.iso=<path> glassboot.part=<partuuid>
 *          live-media=/dev/glassboot
 *
 *      This lets an ISO's own unmodified boot menu run. Our initrd hook (hook/) then
 *      maps the ISO file as a block device before the distribution looks for its media.
 *
 *  Copyright (C) 2026 Glassboot contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the
 *  terms of the GNU General Public License as published by the Free Software Foundation,
 *  either version 3 of the License, or (at your option) any later version.
 */

#include <grub/command.h>
#include <grub/acpi.h>
#include <grub/crypto.h>
#include <grub/device.h>
#include <grub/disk.h>
#include <grub/partition.h>
#include <grub/dl.h>
#include <grub/env.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/procfs.h>
#include <grub/types.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* ------------------------------------------------------------------ string buffer */

struct sbuf
{
  char *data;
  grub_size_t len, cap;
};

static int
sbuf_add (struct sbuf *b, const char *s)
{
  grub_size_t n = grub_strlen (s);
  if (b->len + n + 1 > b->cap)
    {
      grub_size_t cap = b->cap ? b->cap * 2 : 4096;
      char *d;
      while (cap < b->len + n + 1)
	cap *= 2;
      d = grub_realloc (b->data, cap);
      if (!d)
	return 0;
      b->data = d;
      b->cap = cap;
    }
  grub_memcpy (b->data + b->len, s, n + 1);
  b->len += n;
  return 1;
}

/* Append S inside a GRUB double-quoted string. */
static int
sbuf_add_quoted (struct sbuf *b, const char *s)
{
  char one[3];
  for (; *s; s++)
    {
      int i = 0;
      if (*s == '"' || *s == '\\' || *s == '$')
	one[i++] = '\\';
      one[i++] = *s;
      one[i] = 0;
      if (!sbuf_add (b, one))
	return 0;
    }
  return 1;
}

/* ------------------------------------------------------------------ glassboot.cfg */
/*
 * gb_config FILE
 *
 * Reads the user's \glassboot\glassboot.cfg. One setting per line; '#' starts a comment
 * line. Global settings become exported environment variables:
 *
 *   timeout = SECONDS        -> gb_cfg_timeout
 *   default = /path/file.iso -> gb_cfg_default (resolved to a menu id by gb_scan)
 *   view = list|tree         -> gb_cfg_view
 *   theme = /path/theme.txt  -> gb_cfg_theme
 *   password = grub.pbkdf2.sha512...  -> gb_cfg_password
 *
 * Per-file settings (paths on the data partition, case-insensitive, '\' or '/'):
 *
 *   alias   /path/file.iso = Menu name
 *   hide    /path/folder-or-file
 *   args    /path/file.iso = extra kernel arguments
 *   persist /path/file.iso = /path/persistence.dat
 *   dud     /path/file.iso = /path/driver-disk.iso   (may repeat; Linux driver update disks)
 */
enum { CFG_ALIAS, CFG_HIDE, CFG_ARGS, CFG_PERSIST, CFG_DUD, CFG_CLASS,
       CFG_KICKSTART, CFG_PRESEED, CFG_AUTOINSTALL, CFG_AUTOYAST };

struct cfg_item
{
  int type;
  char *path;
  char *value;
};

#define CFG_MAX_ITEMS 512
static struct cfg_item cfg_items[CFG_MAX_ITEMS];
static int cfg_nitems;

static void
cfg_clear (void)
{
  int i;
  for (i = 0; i < cfg_nitems; i++)
    {
      grub_free (cfg_items[i].path);
      grub_free (cfg_items[i].value);
    }
  cfg_nitems = 0;
}

static char *
trim (char *s)
{
  char *e;
  while (*s == ' ' || *s == '\t')
    s++;
  e = s + grub_strlen (s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
    *--e = 0;
  return s;
}

/* "/Dir\sub/file.iso/" -> "/Dir/sub/file.iso" (newly allocated) */
static char *
norm_path (const char *p)
{
  char *r = grub_malloc (grub_strlen (p) + 2), *o = r;
  if (!r)
    return NULL;
  if (*p != '/' && *p != '\\')
    *o++ = '/';
  for (; *p; p++)
    *o++ = (*p == '\\') ? '/' : *p;
  while (o > r + 1 && o[-1] == '/')
    o--;
  *o = 0;
  return r;
}

static const char *
cfg_lookup (int type, const char *path)
{
  int i;
  for (i = 0; i < cfg_nitems; i++)
    if (cfg_items[i].type == type && grub_strcasecmp (cfg_items[i].path, path) == 0)
      return cfg_items[i].value;
  return NULL;
}

static int
cfg_hidden (const char *path)
{
  int i;
  for (i = 0; i < cfg_nitems; i++)
    if (cfg_items[i].type == CFG_HIDE)
      {
	grub_size_t n = grub_strlen (cfg_items[i].path);
	if (grub_strncasecmp (cfg_items[i].path, path, n) == 0 && (path[n] == 0 || path[n] == '/'))
	  return 1;
      }
  return 0;
}

static void
cfg_set_global (const char *key, const char *value)
{
  char name[64];
  grub_snprintf (name, sizeof (name), "gb_cfg_%s", key);
  grub_env_set (name, value);
  grub_env_export (name);
}

static grub_err_t
grub_cmd_gb_config (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  grub_file_t f;
  grub_off_t size;
  char *buf, *line, *next;
  int lineno = 0;

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("usage: gb_config FILE"));
  cfg_clear ();
  f = grub_file_open (argv[0], GRUB_FILE_TYPE_CONFIG | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!f)
    {
      grub_errno = GRUB_ERR_NONE; /* no config file: defaults */
      return GRUB_ERR_NONE;
    }
  size = grub_file_size (f);
  if (size > 256 * 1024)
    size = 256 * 1024;
  buf = grub_zalloc (size + 1);
  if (!buf || grub_file_read (f, buf, size) != (grub_ssize_t) size)
    {
      grub_file_close (f);
      grub_free (buf);
      return grub_error (GRUB_ERR_FILE_READ_ERROR, "cannot read %s", argv[0]);
    }
  grub_file_close (f);

  for (line = buf; line; line = next)
    {
      char *eq, *key, *rest;
      int type = -1;
      next = grub_strchr (line, '\n');
      if (next)
	*next++ = 0;
      lineno++;
      line = trim (line);
      /* UTF-8 byte order mark (Notepad) */
      if (lineno == 1 && (grub_uint8_t) line[0] == 0xef && (grub_uint8_t) line[1] == 0xbb && (grub_uint8_t) line[2] == 0xbf)
	line = trim (line + 3);
      if (*line == 0 || *line == '#')
	continue;

      /* First word: per-file keyword or global key. */
      key = line;
      rest = line;
      while (*rest && *rest != ' ' && *rest != '\t' && *rest != '=')
	rest++;
      if (*rest == '=')
	{
	  *rest++ = 0;
	}
      else if (*rest)
	{
	  *rest++ = 0;
	  rest = trim (rest);
	  if (*rest == '=')
	    rest++;
	}
      key = trim (key);
      rest = trim (rest);

      if (grub_strcasecmp (key, "alias") == 0)
	type = CFG_ALIAS;
      else if (grub_strcasecmp (key, "hide") == 0)
	type = CFG_HIDE;
      else if (grub_strcasecmp (key, "args") == 0)
	type = CFG_ARGS;
      else if (grub_strcasecmp (key, "persist") == 0)
	type = CFG_PERSIST;
      else if (grub_strcasecmp (key, "class") == 0)
	type = CFG_CLASS;
      else if (grub_strcasecmp (key, "dud") == 0)
	type = CFG_DUD;
      else if (grub_strcasecmp (key, "kickstart") == 0)
        type = CFG_KICKSTART;
      else if (grub_strcasecmp (key, "preseed") == 0)
        type = CFG_PRESEED;
      else if (grub_strcasecmp (key, "autoinstall") == 0)
        type = CFG_AUTOINSTALL;
      else if (grub_strcasecmp (key, "autoyast") == 0)
        type = CFG_AUTOYAST;

      if (type < 0)
	{
	  if (grub_strcasecmp (key, "timeout") == 0 || grub_strcasecmp (key, "default") == 0
	      || grub_strcasecmp (key, "view") == 0 || grub_strcasecmp (key, "theme") == 0
	      || grub_strcasecmp (key, "password") == 0 || grub_strcasecmp (key, "language") == 0)
	    {
	      char lower[16];
	      grub_size_t k;
	      for (k = 0; key[k] && k < sizeof (lower) - 1; k++)
		lower[k] = grub_tolower (key[k]);
	      lower[k] = 0;
	      if (grub_strcmp (lower, "default") == 0 || grub_strcmp (lower, "theme") == 0)
		{
		  char *p = norm_path (rest);
		  cfg_set_global (lower, p ? p : rest);
		  grub_free (p);
		}
	      else
		cfg_set_global (lower, rest);
	    }
	  else
	    grub_printf ("glassboot.cfg line %d: unknown setting \"%s\" (ignored)\n", lineno, key);
	  continue;
	}

      if (cfg_nitems >= CFG_MAX_ITEMS)
	continue;
      if (type == CFG_HIDE)
	{
	  cfg_items[cfg_nitems].type = type;
	  cfg_items[cfg_nitems].path = norm_path (rest);
	  cfg_items[cfg_nitems].value = NULL;
	  cfg_nitems++;
	  continue;
	}
      eq = grub_strchr (rest, '=');
      if (!eq)
	{
	  grub_printf ("glassboot.cfg line %d: expected \"%s PATH = VALUE\" (ignored)\n", lineno, key);
	  continue;
	}
      *eq = 0;
      cfg_items[cfg_nitems].type = type;
      cfg_items[cfg_nitems].path = norm_path (trim (rest));
      cfg_items[cfg_nitems].value = (type == CFG_PERSIST || type == CFG_DUD || type >= CFG_KICKSTART) ? norm_path (trim (eq + 1))
				    : grub_strdup (trim (eq + 1));
      cfg_nitems++;
    }
  grub_free (buf);
  grub_errno = GRUB_ERR_NONE;
  return GRUB_ERR_NONE;
}

/* ------------------------------------------------------------------ gb_scan */

#define GB_MAX_DEPTH 8
#define GB_MAX_ISOS 4096

struct iso_entry
{
  char *path;      /* "/dir/name.iso" */
  grub_off_t size;
  int kind;        /* GB_KIND_* */
};

struct scan_ctx
{
  const char *devname;
  struct iso_entry *isos;
  grub_size_t count, cap;
  char **subdirs;  /* names collected for the directory being listed */
  grub_size_t nsub, capsub;
  const char *dir; /* directory being listed */
};

static char *menu_text;

static int
has_suffix (const char *name, const char *suffix)
{
  grub_size_t n = grub_strlen (name), s = grub_strlen (suffix);
  return n > s && grub_strcasecmp (name + n - s, suffix) == 0;
}

enum
{
  GB_KIND_NONE,
  GB_KIND_ISO,  /* gb_boot_iso */
  GB_KIND_EFI,  /* gb_boot_efi: EFI application (UEFI only) */
  GB_KIND_IMG,  /* gb_boot_img: raw disk image / fixed VHD (UEFI virtual disk, BIOS memdisk) */
  GB_KIND_WIM   /* gb_boot_wim: Windows PE image */
};

static const char *const kind_class[] = { "", "iso", "efi", "disk", "windows" };

static int
bootable_kind (const char *name)
{
  const char *platform = grub_env_get ("grub_platform");
  int efi = platform && grub_strcmp (platform, "efi") == 0;
  if (has_suffix (name, ".iso"))
    return GB_KIND_ISO;
  if (has_suffix (name, ".wim"))
    return GB_KIND_WIM;
  if (efi && has_suffix (name, ".efi"))
    return GB_KIND_EFI;
  if (has_suffix (name, ".img") || has_suffix (name, ".vhd") || has_suffix (name, ".vhdx"))
    return GB_KIND_IMG;
  return GB_KIND_NONE;
}

static int
skip_dir (const char *name)
{
  return name[0] == '.' || name[0] == '$'
    || grub_strcasecmp (name, "System Volume Information") == 0
    || grub_strcasecmp (name, "glassboot") == 0;
}

static char *
join_path (const char *dir, const char *name)
{
  return grub_xasprintf ("%s%s%s", dir, (dir[0] && dir[grub_strlen (dir) - 1] == '/') ? "" : "/", name);
}

static int
scan_hook (const char *filename, const struct grub_dirhook_info *info, void *data)
{
  struct scan_ctx *ctx = data;
  int kind;

  if (grub_strcmp (filename, ".") == 0 || grub_strcmp (filename, "..") == 0)
    return 0;

  if (info->dir)
    {
      if (skip_dir (filename))
	return 0;
      {
	char *p = join_path (ctx->dir, filename);
	int hidden = p && cfg_hidden (p);
	grub_free (p);
	if (hidden)
	  return 0;
      }
      if (ctx->nsub == ctx->capsub)
	{
	  grub_size_t cap = ctx->capsub ? ctx->capsub * 2 : 16;
	  char **n = grub_realloc (ctx->subdirs, cap * sizeof (char *));
	  if (!n)
	    return 1;
	  ctx->subdirs = n;
	  ctx->capsub = cap;
	}
      ctx->subdirs[ctx->nsub++] = grub_strdup (filename);
      return 0;
    }

  kind = bootable_kind (filename);
  if (!kind || filename[0] == '.')
    return 0; /* also skips macOS "._name.iso" resource-fork files */
  if (ctx->count >= GB_MAX_ISOS)
    return 1;
  if (ctx->count == ctx->cap)
    {
      grub_size_t cap = ctx->cap ? ctx->cap * 2 : 32;
      struct iso_entry *n = grub_realloc (ctx->isos, cap * sizeof (*n));
      if (!n)
	return 1;
      ctx->isos = n;
      ctx->cap = cap;
    }
  ctx->isos[ctx->count].path = join_path (ctx->dir, filename);
  if (!ctx->isos[ctx->count].path || cfg_hidden (ctx->isos[ctx->count].path))
    {
      grub_free (ctx->isos[ctx->count].path);
      return 0;
    }
  ctx->isos[ctx->count].size = 0;
  ctx->isos[ctx->count].kind = kind;
  ctx->count++;
  return 0;
}

static void
scan_dir (struct scan_ctx *ctx, grub_device_t dev, grub_fs_t fs, const char *dir, int depth)
{
  char **subs;
  grub_size_t nsub, i;

  ctx->dir = dir;
  ctx->subdirs = NULL;
  ctx->nsub = ctx->capsub = 0;
  fs->fs_dir (dev, dir, scan_hook, ctx);
  grub_errno = GRUB_ERR_NONE; /* unreadable directories are skipped, not fatal */

  /* Recurse after listing: fs drivers aren't re-entrant inside their dir hook. */
  subs = ctx->subdirs;
  nsub = ctx->nsub;
  for (i = 0; i < nsub; i++)
    {
      if (depth < GB_MAX_DEPTH)
	{
	  char *path = join_path (dir, subs[i]);
	  if (path)
	    scan_dir (ctx, dev, fs, path, depth + 1);
	  grub_free (path);
	}
      grub_free (subs[i]);
    }
  grub_free (subs);
}

static int
iso_cmp (const struct iso_entry *a, const struct iso_entry *b)
{
  /* Files in the root first, then by path, case-insensitively. */
  int da = grub_strchr (a->path + 1, '/') != NULL;
  int db = grub_strchr (b->path + 1, '/') != NULL;
  if (da != db)
    return da - db;
  return grub_strcasecmp (a->path, b->path);
}

static void
format_size (grub_off_t size, char *out, grub_size_t outlen)
{
  const char *units[] = { "B", "KB", "MB", "GB", "TB" };
  grub_uint64_t whole = size, frac = 0, rem;
  int u = 0;
  /* grub_divmod64: 32-bit targets have no libgcc 64-bit division. */
  while (whole >= 1000 && u < 4)
    {
      whole = grub_divmod64 (whole, 1000, &rem);
      frac = (grub_uint32_t) rem / 100;
      u++;
    }
  if (u == 0)
    grub_snprintf (out, outlen, "%llu B", (unsigned long long) whole);
  else
    grub_snprintf (out, outlen, "%llu.%llu %s", (unsigned long long) whole,
		   (unsigned long long) frac, units[u]);
}

/* One menu entry. Its id is "gb:<path>" so `default` can refer to it. */
static void
emit_entry (struct sbuf *menu, const struct iso_entry *e, const char *indent, int tree)
{
  static const char *const kind_name[] = { "", "iso", "efi", "img", "wim" };
  const char *alias = cfg_lookup (CFG_ALIAS, e->path);
  const char *label = e->path + 1;
  char size[32];

  if (tree)
    {
      const char *slash = grub_strrchr (e->path, '/');
      label = slash ? slash + 1 : e->path;
    }
  format_size (e->size, size, sizeof (size));
  sbuf_add (menu, indent);
  sbuf_add (menu, "menuentry \"");
  sbuf_add_quoted (menu, alias ? alias : label);
  sbuf_add (menu, "    [");
  sbuf_add (menu, size);
  sbuf_add (menu, "]\" --class ");
  {
    const char *cls = cfg_lookup (CFG_CLASS, e->path);
    sbuf_add (menu, "\"");
    sbuf_add_quoted (menu, cls && *cls ? cls : kind_class[e->kind]);
    sbuf_add (menu, "\"");
  }
  sbuf_add (menu, " --id \"gb:");
  sbuf_add_quoted (menu, e->path);
  sbuf_add (menu, "\" \"");
  sbuf_add_quoted (menu, e->path);
  sbuf_add (menu, "\" {\n");
  sbuf_add (menu, indent);
  sbuf_add (menu, "  gb_entry ");
  sbuf_add (menu, kind_name[e->kind]);
  sbuf_add (menu, " \"$2\"\n");
  sbuf_add (menu, indent);
  sbuf_add (menu, "}\n");
}

/* Directory part of a path: "/a/b/c.iso" -> length of "/a/b" (0 for root files). */
static grub_size_t
parent_len (const char *path)
{
  const char *slash = grub_strrchr (path, '/');
  return slash ? (grub_size_t) (slash - path) : 0;
}

/* Tree view: files in DIR (length dlen) as entries, then each sub-folder as a submenu. */
static void
emit_tree (struct sbuf *menu, struct iso_entry *isos, grub_size_t count, const char *dir,
	   grub_size_t dlen, int depth)
{
  grub_size_t i, j;
  char indent[2 * GB_MAX_DEPTH + 3];
  grub_size_t k;

  for (k = 0; k < (grub_size_t) depth * 2 && k < sizeof (indent) - 1; k++)
    indent[k] = ' ';
  indent[k] = 0;

  for (i = 0; i < count; i++)
    if (parent_len (isos[i].path) == dlen && grub_strncasecmp (isos[i].path, dir, dlen) == 0)
      emit_entry (menu, &isos[i], indent, 1);

  /* Immediate sub-folders, each once, in sorted order. */
  for (i = 0; i < count; i++)
    {
      const char *p = isos[i].path;
      const char *sub, *end;
      char *name, *child;
      int seen = 0;

      if (grub_strncasecmp (p, dir, dlen) != 0 || p[dlen] != '/' || parent_len (p) == dlen)
	continue;
      sub = p + dlen + 1;
      end = grub_strchr (sub, '/');
      if (!end)
	continue;
      for (j = 0; j < i && !seen; j++)
	{
	  const char *q = isos[j].path;
	  if (grub_strncasecmp (q, p, end - p) == 0 && q[end - p] == '/')
	    seen = 1;
	}
      if (seen)
	continue;
      name = grub_strndup (sub, end - sub);
      child = grub_strndup (p, end - p);
      if (name && child)
	{
	  sbuf_add (menu, indent);
	  sbuf_add (menu, "submenu \"");
	  sbuf_add_quoted (menu, name);
	  sbuf_add (menu, "/\" --class folder --id \"gbdir:");
	  sbuf_add_quoted (menu, child);
	  sbuf_add (menu, "\" {\n");
	  emit_tree (menu, isos, count, child, end - p, depth + 1);
	  sbuf_add (menu, indent);
	  sbuf_add (menu, "}\n");
	}
      grub_free (name);
      grub_free (child);
    }
}

/* GRUB's `default` for PATH: "gb:/a.iso", or in tree view "gbdir:/x>gbdir:/x/y>gb:/x/y/a.iso". */
static char *
default_id (const char *path, int tree)
{
  struct sbuf b = { 0 };
  const char *p;
  if (tree)
    for (p = grub_strchr (path + 1, '/'); p; p = grub_strchr (p + 1, '/'))
      {
	char *dir = grub_strndup (path, p - path);
	sbuf_add (&b, "gbdir:");
	sbuf_add (&b, dir ? dir : "");
	sbuf_add (&b, ">");
	grub_free (dir);
      }
  sbuf_add (&b, "gb:");
  sbuf_add (&b, path);
  return b.data;
}

static char *
menu_get_contents (grub_size_t *sz)
{
  char *copy;
  if (!menu_text)
    {
      *sz = 0;
      return grub_strdup ("");
    }
  *sz = grub_strlen (menu_text);
  copy = grub_strdup (menu_text);
  return copy;
}

static struct grub_procfs_entry menu_entry = {
  .name = "glassboot_menu",
  .get_contents = menu_get_contents
};

static int
contains_ci (const char *text, const char *query)
{
  grub_size_t n = grub_strlen (query);
  for (; *text; text++)
    if (grub_strncasecmp (text, query, n) == 0)
      return 1;
  return n == 0;
}

static grub_err_t
grub_cmd_gb_scan (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  struct scan_ctx ctx;
  grub_device_t dev;
  grub_fs_t fs;
  char *devname;
  grub_size_t i, j, len;
  struct sbuf menu = { 0 };
  char count[32];

  if (argc < 1 || argc > 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("usage: gb_scan (DEVICE) [FILTER]"));

  len = grub_strlen (argv[0]);
  if (len >= 2 && argv[0][0] == '(' && argv[0][len - 1] == ')')
    devname = grub_strndup (argv[0] + 1, len - 2);
  else
    devname = grub_strdup (argv[0]);
  if (!devname)
    return grub_errno;

  dev = grub_device_open (devname);
  if (!dev)
    {
      grub_free (devname);
      return grub_errno;
    }
  fs = grub_fs_probe (dev);
  if (!fs)
    {
      grub_device_close (dev);
      grub_free (devname);
      return grub_errno;
    }

  grub_memset (&ctx, 0, sizeof (ctx));
  ctx.devname = devname;
  scan_dir (&ctx, dev, fs, "/", 0);
  grub_device_close (dev);

  if (argc == 2 && argv[1][0])
    {
      grub_size_t kept = 0;
      for (i = 0; i < ctx.count; i++)
        {
          const char *alias = cfg_lookup (CFG_ALIAS, ctx.isos[i].path);
          if (contains_ci (ctx.isos[i].path, argv[1]) || (alias && contains_ci (alias, argv[1])))
            ctx.isos[kept++] = ctx.isos[i];
          else
            grub_free (ctx.isos[i].path);
        }
      ctx.count = kept;
    }

  /* Sizes (opening a file is just a directory lookup). */
  for (i = 0; i < ctx.count; i++)
    {
      char *full = grub_xasprintf ("(%s)%s", devname, ctx.isos[i].path);
      grub_file_t f = full ? grub_file_open (full, GRUB_FILE_TYPE_GET_SIZE | GRUB_FILE_TYPE_NO_DECOMPRESS) : NULL;
      if (f)
	{
	  ctx.isos[i].size = grub_file_size (f);
	  grub_file_close (f);
	}
      grub_errno = GRUB_ERR_NONE;
      grub_free (full);
    }

  /* Insertion sort: lists are small. */
  for (i = 1; i < ctx.count; i++)
    for (j = i; j > 0 && iso_cmp (&ctx.isos[j - 1], &ctx.isos[j]) > 0; j--)
      {
	struct iso_entry t = ctx.isos[j];
	ctx.isos[j] = ctx.isos[j - 1];
	ctx.isos[j - 1] = t;
      }

  {
    const char *view = grub_env_get ("gb_cfg_view");
    const char *want = grub_env_get ("gb_cfg_default");
    int tree = view && grub_strcasecmp (view, "tree") == 0;

    if (tree)
      emit_tree (&menu, ctx.isos, ctx.count, "", 0, 0);
    else
      for (i = 0; i < ctx.count; i++)
	emit_entry (&menu, &ctx.isos[i], "", 0);

    /* Resolve the configured default to a menu id (only if that file exists). */
    grub_env_unset ("gb_default_id");
    for (i = 0; want && *want && i < ctx.count; i++)
      if (grub_strcasecmp (ctx.isos[i].path, want) == 0)
	{
	  char *id = default_id (ctx.isos[i].path, tree);
	  if (id)
	    {
	      grub_env_set ("gb_default_id", id);
	      grub_env_export ("gb_default_id");
	    }
	  grub_free (id);
	  break;
	}
  }
  for (i = 0; i < ctx.count; i++)
    grub_free (ctx.isos[i].path);
  grub_free (ctx.isos);
  grub_free (devname);

  grub_free (menu_text);
  menu_text = menu.data ? menu.data : grub_strdup ("");
  grub_snprintf (count, sizeof (count), "%llu", (unsigned long long) ctx.count);
  grub_env_set ("gb_iso_count", count);
  grub_env_export ("gb_iso_count");
  return GRUB_ERR_NONE;
}

/* ------------------------------------------------------------------ injection folders */

/* Files (not directories, not dot-files) directly inside DIR, which has the form
   "(device)/path". Sorted by name. Returns the count; *names must be freed with
   free_names. A missing directory is not an error (0 files). */
struct name_list
{
  char **v;
  int n, cap;
};

static int
list_hook (const char *filename, const struct grub_dirhook_info *info, void *data)
{
  struct name_list *l = data;
  if (info->dir || filename[0] == '.')
    return 0;
  if (l->n == l->cap)
    {
      int cap = l->cap ? l->cap * 2 : 8;
      char **v = grub_realloc (l->v, cap * sizeof (char *));
      if (!v)
	return 1;
      l->v = v;
      l->cap = cap;
    }
  l->v[l->n++] = grub_strdup (filename);
  return 0;
}

static void
free_names (struct name_list *l)
{
  int i;
  for (i = 0; i < l->n; i++)
    grub_free (l->v[i]);
  grub_free (l->v);
  l->v = NULL;
  l->n = l->cap = 0;
}

static int
list_dir_files (const char *dir, struct name_list *out)
{
  const char *close;
  char *devname;
  grub_device_t dev;
  grub_fs_t fs;
  int i, j;

  grub_memset (out, 0, sizeof (*out));
  if (dir[0] != '(' || !(close = grub_strchr (dir, ')')))
    return 0;
  devname = grub_strndup (dir + 1, close - dir - 1);
  if (!devname)
    return 0;
  dev = grub_device_open (devname);
  grub_free (devname);
  if (!dev)
    {
      grub_errno = GRUB_ERR_NONE;
      return 0;
    }
  fs = grub_fs_probe (dev);
  if (fs)
    fs->fs_dir (dev, close + 1, list_hook, out);
  grub_device_close (dev);
  grub_errno = GRUB_ERR_NONE;
  for (i = 1; i < out->n; i++)
    for (j = i; j > 0 && grub_strcasecmp (out->v[j - 1], out->v[j]) > 0; j--)
      {
	char *t = out->v[j];
	out->v[j] = out->v[j - 1];
	out->v[j - 1] = t;
      }
  return out->n;
}

static int
file_exists (const char *path)
{
  grub_file_t f = grub_file_open (path, GRUB_FILE_TYPE_GET_SIZE | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!f)
    {
      grub_errno = GRUB_ERR_NONE;
      return 0;
    }
  grub_file_close (f);
  return 1;
}

/* ------------------------------------------------------------------ linux/initrd hook */

static int hook_enabled;
/* Extra kernel arguments, one per element: the linux loader quotes any single argument
   that contains spaces, so they must not be joined. */
#define HOOK_MAX_ARGS 40
static char *hook_args[HOOK_MAX_ARGS];
static int hook_nargs;
/* Extra initrd files: the hook cpio, then user injection archives. */
#define HOOK_MAX_INITRDS 20
static char *hook_initrds[HOOK_MAX_INITRDS];
static int hook_ninitrds;

/* Physical maps from GRUB's filesystem drivers avoid requiring NTFS/XFS/UDF
   modules in every distribution's initrd. The file must be fully allocated,
   uncompressed and sector-aligned; gaps in callback coverage are rejected. */
static char *hook_maps[3];
struct hook_extent { grub_uint64_t off, len; };
struct hook_map_ctx {
  struct hook_extent *v;
  grub_size_t n, cap;
  grub_uint64_t base, total;
};
static grub_err_t
hook_map_read (grub_disk_addr_t sector, unsigned offset, unsigned length,
               char *buf __attribute__ ((unused)), void *data)
{
  struct hook_map_ctx *c = data;
  grub_uint64_t at;
  if (sector < c->base || offset || !length)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "unaligned file extents");
  at = (sector - c->base) << 9;
  if (c->n && c->v[c->n-1].off + c->v[c->n-1].len == at)
    c->v[c->n-1].len += length;
  else
    {
      if (c->n >= 65536) return grub_error (GRUB_ERR_BAD_FILE_TYPE, "too many file extents");
      if (c->n == c->cap)
        {
          void *p;
          c->cap = c->cap ? c->cap * 2 : 16;
          p = grub_realloc (c->v, c->cap * sizeof (*c->v));
          if (!p) return grub_errno;
          c->v = p;
        }
      c->v[c->n].off = at; c->v[c->n++].len = length;
    }
  c->total += length;
  return GRUB_ERR_NONE;
}
static grub_err_t
hook_make_map (const char *device, const char *path, int index)
{
  char *full = grub_xasprintf ("%s%s", device, path), *buf = NULL;
  grub_file_t file;
  struct hook_map_ctx c = {0};
  struct sbuf text = {0};
  grub_size_t i;
  char line[96];
  grub_err_t err = GRUB_ERR_NONE;
  if (!full) return grub_errno;
  file = grub_file_open (full, GRUB_FILE_TYPE_PRINT_BLOCKLIST | GRUB_FILE_TYPE_NO_DECOMPRESS);
  grub_free (full);
  if (!file) return grub_errno;
  if (!file->device->disk) { err = grub_error (GRUB_ERR_BAD_DEVICE, "disk file required"); goto done; }
  if (!grub_strcmp (file->fs->name, "exfat")) goto done;
  /* Never bypass a COW filesystem for writable persistence. The hook uses a
     filesystem-backed loop in that case, so reflinks and sparse files stay safe. */
  if (index == 1) goto done;
  grub_printf ("Mapping %s for the Linux hook...\n", path);
  buf = grub_malloc (1024 * 1024);
  if (!buf) { err = grub_errno; goto done; }
  c.base = grub_partition_get_start (file->device->disk->partition);
  file->read_hook = hook_map_read; file->read_hook_data = &c;
  while (grub_file_read (file, buf, 1024 * 1024) > 0) ;
  if (grub_errno) { err = grub_errno; goto done; }
  if (!c.n || c.total != grub_file_size (file))
    { err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "sparse/compressed files cannot be mapped; copy %s without compression", path); goto done; }
  grub_snprintf (line, sizeof (line), "GBMAP1 %llu %llu\n", (unsigned long long)c.total, (unsigned long long)c.n);
  if (!sbuf_add (&text, line)) { err = grub_errno; goto done; }
  for (i = 0; i < c.n; i++)
    {
      if ((c.v[i].len & 511) && i + 1 != c.n)
        { err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "non-sector-aligned file extent"); goto done; }
      grub_snprintf (line, sizeof (line), "%llu %llu\n", (unsigned long long)c.v[i].off, (unsigned long long)c.v[i].len);
      if (!sbuf_add (&text, line)) { err = grub_errno; goto done; }
    }
  hook_maps[index] = text.data; text.data = NULL;
done:
  grub_free (text.data); grub_free (c.v); grub_free (buf); grub_file_close (file);
  return err;
}
static char *hook_map_get (int i, grub_size_t *size) {
  const char *s = hook_maps[i] ? hook_maps[i] : "";
  *size = grub_strlen (s); return grub_strdup (s);
}
static char *hook_map_iso (grub_size_t *n) { return hook_map_get (0,n); }
static char *hook_map_persist (grub_size_t *n) { return hook_map_get (1,n); }
static char *hook_map_seed (grub_size_t *n) { return hook_map_get (2,n); }
static struct grub_procfs_entry hook_map_entries[3] = {
  { .name="glassboot_iso_map", .get_contents=hook_map_iso },
  { .name="glassboot_persist_map", .get_contents=hook_map_persist },
  { .name="glassboot_seed_map", .get_contents=hook_map_seed }
};

static void
free_hook_args (void)
{
  int i;
  for (i = 0; i < hook_nargs; i++)
    grub_free (hook_args[i]);
  hook_nargs = 0;
  for (i = 0; i < hook_ninitrds; i++)
    grub_free (hook_initrds[i]);
  hook_ninitrds = 0;
  for (i = 0; i < 3; i++) { grub_free (hook_maps[i]); hook_maps[i] = NULL; }
}

struct wrapped
{
  const char *name;
  grub_command_t cmd;
  grub_command_func_t orig;
  int is_initrd;
};

static struct wrapped wrapped[] = {
  { "linux", NULL, NULL, 0 },
  { "initrd", NULL, NULL, 1 },
  { "linux16", NULL, NULL, 0 },
  { "initrd16", NULL, NULL, 1 },
};
#define N_WRAPPED (sizeof (wrapped) / sizeof (wrapped[0]))

/* linuxefi/initrdefi are distro-patched GRUB names; alias them to linux/initrd. */
static grub_command_t alias_linuxefi, alias_initrdefi;

static struct wrapped *
find_wrapped (grub_command_t cmd)
{
  grub_size_t i;
  for (i = 0; i < N_WRAPPED; i++)
    if (wrapped[i].cmd == cmd)
      return &wrapped[i];
  return NULL;
}

static grub_err_t
call_with_extra (struct wrapped *w, grub_command_t cmd, int argc, char **argv)
{
  char **extra = w->is_initrd ? hook_initrds : hook_args;
  int nextra = w->is_initrd ? hook_ninitrds : hook_nargs;
  char **nargv;
  grub_err_t err;
  int i;

  if (!hook_enabled || nextra == 0 || argc < 1)
    return w->orig (cmd, argc, argv);

  nargv = grub_calloc (argc + nextra + 1, sizeof (char *));
  if (!nargv)
    return grub_errno;
  for (i = 0; i < argc; i++)
    nargv[i] = argv[i];
  for (i = 0; i < nextra; i++)
    nargv[argc + i] = extra[i];
  err = w->orig (cmd, argc + nextra, nargv);
  grub_free (nargv);
  return err;
}

static grub_err_t
hooked (grub_command_t cmd, int argc, char **argv)
{
  struct wrapped *w = find_wrapped (cmd);
  if (!w)
    return grub_error (GRUB_ERR_BUG, "glassboot: unknown wrapped command");
  return call_with_extra (w, cmd, argc, argv);
}

static grub_err_t
alias_cmd (grub_command_t cmd, int argc, char **argv)
{
  const char *target = (cmd == alias_initrdefi) ? "initrd" : "linux";
  grub_command_t real = grub_command_find (target);
  if (!real)
    return grub_error (GRUB_ERR_UNKNOWN_COMMAND, "%s not available", target);
  return real->func (real, argc, argv);
}

static void
install_wrappers (void)
{
  grub_size_t i;

  /* Make sure the loaders are present so there is something to wrap. */
  grub_dl_load ("linux");
#if defined (GRUB_MACHINE_PCBIOS)
  grub_dl_load ("linux16");
#endif
  grub_errno = GRUB_ERR_NONE;

  for (i = 0; i < N_WRAPPED; i++)
    {
      grub_command_t c = grub_command_find (wrapped[i].name);
      if (!c || c->func == hooked)
	continue;
      wrapped[i].cmd = c;
      wrapped[i].orig = c->func;
      c->func = hooked;
    }
  if (!alias_linuxefi && !grub_command_find ("linuxefi"))
    alias_linuxefi = grub_register_command ("linuxefi", alias_cmd, N_("FILE [ARGS...]"),
					    N_("Alias of linux (Glassboot)."));
  if (!alias_initrdefi && !grub_command_find ("initrdefi"))
    alias_initrdefi = grub_register_command ("initrdefi", alias_cmd, N_("FILE..."),
					     N_("Alias of initrd (Glassboot)."));
}

static void
uninstall_wrappers (void)
{
  grub_size_t i;
  for (i = 0; i < N_WRAPPED; i++)
    if (wrapped[i].cmd && wrapped[i].cmd->func == hooked)
      wrapped[i].cmd->func = wrapped[i].orig;
  if (alias_linuxefi)
    grub_unregister_command (alias_linuxefi);
  if (alias_initrdefi)
    grub_unregister_command (alias_initrdefi);
  alias_linuxefi = alias_initrdefi = NULL;
}

/* Kernel parameters can't contain spaces; percent-encode anything unusual. */
static int
add_encoded (struct sbuf *b, const char *s)
{
  static const char hex[] = "0123456789ABCDEF";
  char one[4];
  for (; *s; s++)
    {
      unsigned char c = (unsigned char) *s;
      if (c <= ' ' || c >= 0x7f || c == '%' || c == '"' || c == '\'' || c == '\\')
	{
	  one[0] = '%';
	  one[1] = hex[c >> 4];
	  one[2] = hex[c & 15];
	  one[3] = 0;
	}
      else
	{
	  one[0] = (char) c;
	  one[1] = 0;
	}
      if (!sbuf_add (b, one))
	return 0;
    }
  return 1;
}

static grub_err_t
grub_cmd_gb_hook_enable (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  struct sbuf iso = { 0 }, part = { 0 };
  struct name_list inj;
  int i;

  hook_enabled = 0;

  if (argc != 3 && argc != 4)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       N_("usage: gb_hook_enable ISO_PATH PART_UUID HOOK_INITRD [DATA_DEVICE]"));

  if (!sbuf_add (&iso, "glassboot.iso=") || !add_encoded (&iso, argv[0])
      || !sbuf_add (&part, "glassboot.part=") || !add_encoded (&part, argv[1]))
    {
      grub_free (iso.data);
      grub_free (part.data);
      return grub_errno;
    }

  free_hook_args ();
  hook_args[hook_nargs++] = grub_strdup ("rdinit=/glassboot/init");
  hook_args[hook_nargs++] = iso.data;
  hook_args[hook_nargs++] = part.data;
  /* Debian live-boot and Ubuntu casper skip loop and device-mapper devices when they
     scan for their media, but accept an explicit one. The hook creates /dev/glassboot.
     Other distributions ignore this argument. */
  hook_args[hook_nargs++] = grub_strdup ("live-media=/dev/glassboot");

  /* NoCloud uses a CIDATA seed disk, available after the live system switches
     root. The other installers read their answer file directly from the initrd. */
  {
    const char *seed = cfg_lookup (CFG_AUTOINSTALL, argv[0]);
    if (seed)
      {
        struct sbuf p = { 0 };
        if (sbuf_add (&p, "glassboot.seed=") && add_encoded (&p, seed))
          {
            hook_args[hook_nargs++] = p.data;
            hook_args[hook_nargs++] = grub_strdup ("autoinstall");
            hook_args[hook_nargs++] = grub_strdup ("ds=nocloud");
          }
        else
          grub_free (p.data);
      }
    if (argc >= 4)
      {
        static const struct { int type; const char *name; const char *arg; } answers[] = {
          { CFG_KICKSTART, "glassboot/ks.cfg", "inst.ks=file:/glassboot/ks.cfg" },
          { CFG_PRESEED, "preseed.cfg", "auto=true priority=critical preseed/file=/preseed.cfg" },
          { CFG_AUTOYAST, "autoinst.xml", "autoyast=file:///autoinst.xml" }
        };
        grub_size_t a;
        for (a = 0; a < sizeof (answers) / sizeof (answers[0]); a++)
          {
            const char *answer = cfg_lookup (answers[a].type, argv[0]);
            char *path;
            if (!answer) continue;
            path = grub_xasprintf ("%s%s", argv[3], answer);
            if (!path || !file_exists (path))
              {
                grub_free (path);
                free_hook_args ();
                return grub_error (GRUB_ERR_FILE_NOT_FOUND, "unattended answer file missing: %s", answer);
              }
            hook_initrds[hook_ninitrds++] = grub_xasprintf ("newc:%s:%s", answers[a].name, path);
            grub_free (path);
            if (answers[a].type == CFG_PRESEED)
              {
                hook_args[hook_nargs++] = grub_strdup ("auto=true");
                hook_args[hook_nargs++] = grub_strdup ("priority=critical");
                hook_args[hook_nargs++] = grub_strdup ("preseed/file=/preseed.cfg");
              }
            else
              hook_args[hook_nargs++] = grub_strdup (answers[a].arg);
          }
      }
  }

  /* glassboot.cfg: persistence file for this ISO (mapped read-write by the hook). */
  {
    const char *persist = cfg_lookup (CFG_PERSIST, argv[0]);
    if (persist)
      {
	struct sbuf p = { 0 };
	if (sbuf_add (&p, "glassboot.persist=") && add_encoded (&p, persist))
	  hook_args[hook_nargs++] = p.data;
	else
	  grub_free (p.data);
      }
  }
  /* glassboot.cfg: extra kernel arguments, one argv element per word (the linux loader
     quotes any argument that contains a space). */
  {
    const char *extra = cfg_lookup (CFG_ARGS, argv[0]);
    while (extra && *extra && hook_nargs < HOOK_MAX_ARGS)
      {
	const char *end;
	while (*extra == ' ' || *extra == '\t')
	  extra++;
	if (!*extra)
	  break;
	end = extra;
	while (*end && *end != ' ' && *end != '\t')
	  end++;
	hook_args[hook_nargs++] = grub_strndup (extra, end - extra);
	extra = end;
      }
  }
  /* The hook first, then injection archives (later archives overwrite earlier files):
       DATA_DEVICE/glassboot/inject/linux/<any file>   added to every Linux boot
       DATA_DEVICE<iso path>.cpio                       added to this ISO only */
  hook_initrds[hook_ninitrds++] = grub_strdup (argv[2]);
  if (argc >= 4)
    {
      const char *paths[3] = { argv[0], cfg_lookup (CFG_PERSIST, argv[0]), cfg_lookup (CFG_AUTOINSTALL, argv[0]) };
      static const char *names[3] = { "iso", "persist", "seed" };
      for (i = 0; i < 3; i++)
        if (paths[i])
          {
            grub_err_t err = hook_make_map (argv[3], paths[i], i);
            if (err) { free_hook_args (); return err; }
            if (hook_maps[i])
              hook_initrds[hook_ninitrds++] = grub_xasprintf ("newc:glassboot/%s.map:(proc)/glassboot_%s_map", names[i], names[i]);
          }
    }
  if (argc == 4)
    {
      char *dir = grub_xasprintf ("%s/glassboot/inject/linux", argv[3]);
      char *own = grub_xasprintf ("%s%s.cpio", argv[3], argv[0]);
      if (dir && list_dir_files (dir, &inj))
	{
	  for (i = 0; i < inj.n && hook_ninitrds < HOOK_MAX_INITRDS - 1; i++)
	    hook_initrds[hook_ninitrds++] = grub_xasprintf ("%s/%s", dir, inj.v[i]);
	  free_names (&inj);
	}
      if (own && file_exists (own) && hook_ninitrds < HOOK_MAX_INITRDS)
	{
	  hook_initrds[hook_ninitrds++] = own;
	  own = NULL;
	}
      grub_free (dir);
      grub_free (own);

      /* glassboot.cfg "dud": Linux driver update disks. GRUB's "newc:NAME:FILE" initrd
	 syntax puts each file into the initramfs as /glassboot-dud/N.<ext>, and
	 inst.dd=/glassboot-dud/N.<ext> makes Anaconda (Fedora, RHEL, Rocky, Alma, ...)
	 apply it at once: a path that exists in the initramfs needs no device. */
      {
	int n = 0, c;
	for (c = 0; c < cfg_nitems; c++)
	  {
	    const char *file = cfg_items[c].value, *dot;
	    char ext[8] = "iso", *path, *initrd;
	    grub_size_t k;
	    if (cfg_items[c].type != CFG_DUD || grub_strcasecmp (cfg_items[c].path, argv[0]) != 0)
	      continue;
	    if (hook_ninitrds >= HOOK_MAX_INITRDS || hook_nargs >= HOOK_MAX_ARGS)
	      break;
	    path = grub_xasprintf ("%s%s", argv[3], file);
	    if (!path || !file_exists (path))
	      {
		grub_printf ("glassboot.cfg: driver update disk %s not found\n", file);
		grub_free (path);
		continue;
	      }
	    dot = grub_strrchr (file, '.');
	    if (dot && grub_strlen (dot + 1) < sizeof (ext) && !grub_strchr (dot, '/'))
	      for (k = 0; k <= grub_strlen (dot + 1); k++)
		ext[k] = grub_tolower (dot[1 + k]);
	    initrd = grub_xasprintf ("newc:glassboot-dud/%d.%s:%s", n, ext, path);
	    grub_free (path);
	    if (!initrd)
	      break;
	    hook_initrds[hook_ninitrds++] = initrd;
	    hook_args[hook_nargs++] = grub_xasprintf ("inst.dd=/glassboot-dud/%d.%s", n, ext);
	    n++;
	  }
      }
    }
  install_wrappers ();
  hook_enabled = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_gb_hook_disable (grub_command_t cmd __attribute__ ((unused)),
			  int argc __attribute__ ((unused)), char **argv __attribute__ ((unused)))
{
  hook_enabled = 0;
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_gb_hook_add_initrd (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  if (argc != 1 || !hook_enabled)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "gb_hook_add_initrd requires an active hook and one initrd path");
  if (hook_ninitrds >= HOOK_MAX_INITRDS)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "too many injected initrd files");
  hook_initrds[hook_ninitrds] = grub_strdup (argv[0]);
  if (!hook_initrds[hook_ninitrds]) return grub_errno;
  hook_ninitrds++;
  return GRUB_ERR_NONE;
}

/* ------------------------------------------------------------------ disk image files */
/*
 * Reads the virtual disk inside an image file: raw (.img, .iso), VHD (fixed or dynamic)
 * and VHDX. Formats: Microsoft "Virtual Hard Disk Image Format Specification" (VHD) and
 * "[MS-VHDX]". Read-only. Differencing disks (which need their parent) and VHDX files
 * with an unreplayed log are refused with a message.
 */
enum { VIMG_RAW, VIMG_VHD_FIXED, VIMG_VHD_DYNAMIC, VIMG_VHDX };
static const char *const vimg_names[] = { "raw", "vhd-fixed", "vhd-dynamic", "vhdx" };

struct vimg
{
  grub_file_t file;
  int fmt;
  grub_uint64_t size;        /* virtual disk size in bytes */
  grub_uint32_t block_size;  /* dynamic formats: bytes per block */
  grub_uint32_t nbat;
  grub_uint64_t *bat;        /* file offset of each block's data, 0 = not allocated */
  int dirty;                 /* VHDX with an unreplayed log */
};

static grub_uint32_t
vimg_crc32c (const grub_uint8_t *p, grub_size_t n)
{
  grub_uint32_t crc = 0xffffffff;
  int k;
  while (n--)
    {
      crc ^= *p++;
      for (k = 0; k < 8; k++)
	crc = (crc >> 1) ^ (0x82f63b78 & -(crc & 1));
    }
  return ~crc;
}

static int
vimg_vhd_checksum (const grub_uint8_t *p, grub_size_t n, grub_size_t field)
{
  grub_uint32_t sum = 0;
  grub_size_t i;
  for (i = 0; i < n; i++)
    if (i < field || i >= field + 4)
      sum += p[i];
  return ~sum == grub_be_to_cpu32 (grub_get_unaligned32 (p + field));
}

static grub_err_t
vimg_pread (struct vimg *v, grub_uint64_t off, void *buf, grub_size_t len)
{
  grub_uint64_t size = grub_file_size (v->file);
  if (off > size || len > size - off)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "disk image contains an out-of-range file offset");
  grub_file_seek (v->file, off);
  if (grub_file_read (v->file, buf, len) != (grub_ssize_t) len)
    return grub_errno ? grub_errno : grub_error (GRUB_ERR_FILE_READ_ERROR, "short read in disk image");
  return GRUB_ERR_NONE;
}

static void
vimg_close (struct vimg *v)
{
  if (v->file)
    grub_file_close (v->file);
  grub_free (v->bat);
  grub_memset (v, 0, sizeof (*v));
}

static grub_err_t
vimg_open_vhd_dynamic (struct vimg *v, const grub_uint8_t *footer)
{
  grub_uint8_t hdr[1024];
  grub_uint64_t table;
  grub_uint32_t i, bitmap, *raw;

  if (vimg_pread (v, grub_be_to_cpu64 (grub_get_unaligned64 (footer + 16)), hdr, sizeof (hdr)))
    return grub_errno;
  if (grub_memcmp (hdr, "cxsparse", 8) != 0 || !vimg_vhd_checksum (hdr, sizeof (hdr), 36))
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHD: no dynamic disk header");
  table = grub_be_to_cpu64 (grub_get_unaligned64 (hdr + 16));
  v->nbat = grub_be_to_cpu32 (grub_get_unaligned32 (hdr + 28));
  v->block_size = grub_be_to_cpu32 (grub_get_unaligned32 (hdr + 32));
  if (v->block_size < 512 || (v->block_size & (v->block_size - 1)) || v->nbat == 0 || v->nbat > (1U << 24))
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHD: unsupported block size or table");
  /* Each block starts with a sector bitmap (one bit per sector, padded to 512 bytes). */
  bitmap = ALIGN_UP ((v->block_size / 512 + 7) / 8, 512);
  if (!v->size || grub_divmod64 (v->size - 1, v->block_size, 0) >= v->nbat)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHD: block table does not cover virtual disk");
  raw = grub_calloc (v->nbat, 4);
  v->bat = grub_calloc (v->nbat, 8);
  if (!raw || !v->bat || vimg_pread (v, table, raw, (grub_size_t) v->nbat * 4))
    {
      grub_free (raw);
      return grub_errno;
    }
  for (i = 0; i < v->nbat; i++)
    {
      grub_uint32_t e = grub_be_to_cpu32 (raw[i]);
      v->bat[i] = e == 0xffffffff ? 0 : (grub_uint64_t) e * 512 + bitmap;
      if (v->bat[i] && (v->bat[i] > grub_file_size (v->file)
          || v->block_size > grub_file_size (v->file) - v->bat[i]))
        {
          grub_free (raw);
          return grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHD: allocated block outside file");
        }
    }
  grub_free (raw);
  return GRUB_ERR_NONE;
}

static grub_err_t
vimg_open_vhdx (struct vimg *v)
{
  static const grub_uint8_t bat_guid[16] =
    { 0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42, 0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08 };
  static const grub_uint8_t meta_guid[16] =
    { 0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b, 0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e };
  static const grub_uint8_t params_guid[16] =
    { 0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d, 0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b };
  static const grub_uint8_t size_guid[16] =
    { 0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48, 0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8 };
  static const grub_uint8_t lss_guid[16] =
    { 0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47, 0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f };
  grub_uint8_t *buf, *hdr = NULL, zero[16] = { 0 };
  grub_uint64_t best_seq = 0, bat_off = 0, meta_off = 0, chunk, bat_entries, i;
  grub_uint32_t meta_len = 0, bat_len = 0, lss = 0, n, k;
  grub_err_t err = GRUB_ERR_NONE;

  buf = grub_malloc (65536);
  if (!buf)
    return grub_errno;

  /* Two headers (at 64 KiB and 128 KiB): use the valid one with the higher sequence. */
  for (k = 1; k <= 2; k++)
    {
      grub_uint8_t h[4096];
      grub_uint32_t crc;
      if (vimg_pread (v, (grub_uint64_t) k * 65536, h, sizeof (h)))
	goto fail;
      if (grub_memcmp (h, "head", 4) != 0)
	continue;
      crc = grub_get_unaligned32 (h + 4);
      grub_set_unaligned32 (h + 4, 0);
      if (vimg_crc32c (h, sizeof (h)) != grub_le_to_cpu32 (crc))
	continue;
      if (grub_le_to_cpu16 (grub_get_unaligned16 (h + 66)) != 1)
        continue;
      if (!hdr || grub_le_to_cpu64 (grub_get_unaligned64 (h + 8)) > best_seq)
	{
	  best_seq = grub_le_to_cpu64 (grub_get_unaligned64 (h + 8));
	  grub_memcpy (buf, h, 128);
	  hdr = buf;
	}
    }
  if (!hdr)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: no valid header");
      goto fail;
    }
  /* Reading stale allocation metadata can return incorrect boot files. Only Windows
     can replay the log; never expose such a disk through either GRUB or EFI. */
  v->dirty = grub_memcmp (hdr + 48, zero, 16) != 0;
  if (v->dirty)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: unreplayed log; attach and cleanly detach the disk in Windows first");
      goto fail;
    }

  /* Independently checksummed redundant region tables. */
  for (k = 3; k <= 4; k++)
    {
      grub_uint32_t crc;
      if (vimg_pread (v, (grub_uint64_t) k * 65536, buf, 65536))
        goto fail;
      crc = grub_le_to_cpu32 (grub_get_unaligned32 (buf + 4));
      grub_set_unaligned32 (buf + 4, 0);
      if (!grub_memcmp (buf, "regi", 4) && vimg_crc32c (buf, 65536) == crc)
        break;
    }
  if (k == 5)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: no region table");
      goto fail;
    }
  n = grub_le_to_cpu32 (grub_get_unaligned32 (buf + 8));
  if (n > 2047)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: invalid region entry count");
      goto fail;
    }
  for (k = 0; k < n && k < 2047; k++)
    {
      grub_uint8_t *e = buf + 16 + 32 * k;
      if (grub_memcmp (e, bat_guid, 16) == 0)
        {
	  bat_off = grub_le_to_cpu64 (grub_get_unaligned64 (e + 16));
          bat_len = grub_le_to_cpu32 (grub_get_unaligned32 (e + 24));
        }
      else if (grub_memcmp (e, meta_guid, 16) == 0)
	{
	  meta_off = grub_le_to_cpu64 (grub_get_unaligned64 (e + 16));
	  meta_len = grub_le_to_cpu32 (grub_get_unaligned32 (e + 24));
	}
      else if (grub_le_to_cpu32 (grub_get_unaligned32 (e + 28)) & 1)
        {
          err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: unknown required region");
          goto fail;
        }
    }
  if (!bat_off || !meta_off || meta_len < 65536 || !bat_len
      || bat_off > grub_file_size (v->file) || bat_len > grub_file_size (v->file) - bat_off
      || meta_off > grub_file_size (v->file) || meta_len > grub_file_size (v->file) - meta_off)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: block table or metadata missing");
      goto fail;
    }

  /* Metadata: block size, parent flag, virtual size, logical sector size. */
  if (vimg_pread (v, meta_off, buf, 65536))
    goto fail;
  if (grub_memcmp (buf, "metadata", 8) != 0)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: bad metadata table");
      goto fail;
    }
  n = grub_le_to_cpu16 (grub_get_unaligned16 (buf + 10));
  if (n > 2047)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: invalid metadata entry count");
      goto fail;
    }
  for (k = 0; k < n; k++)
    {
      grub_uint8_t *e = buf + 32 + 32 * k, item[8];
      grub_uint32_t off = grub_le_to_cpu32 (grub_get_unaligned32 (e + 16));
      grub_uint32_t length = grub_le_to_cpu32 (grub_get_unaligned32 (e + 20));
      if (grub_memcmp (e, params_guid, 16) && grub_memcmp (e, size_guid, 16) && grub_memcmp (e, lss_guid, 16))
	continue;
      if (length < (grub_memcmp (e, lss_guid, 16) ? 8U : 4U)
          || off < 65536 || off > meta_len || length > meta_len - off)
        {
          err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: invalid metadata item bounds");
          goto fail;
        }
      grub_memset (item, 0, sizeof (item));
      if (vimg_pread (v, meta_off + off, item, grub_memcmp (e, lss_guid, 16) ? 8 : 4))
	goto fail;
      if (grub_memcmp (e, params_guid, 16) == 0)
	{
	  v->block_size = grub_le_to_cpu32 (grub_get_unaligned32 (item));
	  if (grub_le_to_cpu32 (grub_get_unaligned32 (item + 4)) & 2)
	    {
	      err = grub_error (GRUB_ERR_BAD_FILE_TYPE,
				"VHDX: differencing disks are not supported (merge it into its parent)");
	      goto fail;
	    }
	}
      else if (grub_memcmp (e, size_guid, 16) == 0)
	v->size = grub_le_to_cpu64 (grub_get_unaligned64 (item));
      else
	lss = grub_le_to_cpu32 (grub_get_unaligned32 (item));
    }
  if (lss != 512)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: only 512-byte logical sectors are supported");
      goto fail;
    }
  if (v->block_size < (1U << 20) || v->block_size > (256U << 20) || (v->block_size & (v->block_size - 1))
      || v->size == 0)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: unsupported block size");
      goto fail;
    }

  /* Block table: after every CHUNK payload entries comes one sector-bitmap entry. */
  chunk = grub_divmod64 ((grub_uint64_t) 1 << 23, v->block_size / lss, 0);
  {
    grub_uint64_t blocks = grub_divmod64 (v->size - 1, v->block_size, 0) + 1;
    if (blocks > (1U << 24))
      {
	err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: disk too large");
	goto fail;
      }
    v->nbat = (grub_uint32_t) blocks;
  }
  bat_entries = v->nbat + grub_divmod64 (v->nbat, chunk, 0) + 1;
  if (bat_entries * 8 > bat_len)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: block table does not cover virtual disk");
      goto fail;
    }
  v->bat = grub_calloc (v->nbat, 8);
  {
    grub_uint64_t *raw = grub_calloc (bat_entries, 8);
    if (!raw || !v->bat || vimg_pread (v, bat_off, raw, bat_entries * 8))
      {
	grub_free (raw);
	goto fail;
      }
    for (i = 0; i < v->nbat; i++)
      {
	grub_uint64_t e = grub_le_to_cpu64 (raw[i + grub_divmod64 (i, chunk, 0)]);
	grub_uint32_t state = e & 7;
	/* Partially present blocks require parent/bitmap semantics. Reserved states
           must not silently turn into zeros. */
        if (state == 4 || state == 5 || state == 7)
          {
            grub_free (raw);
            err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: unsupported payload block state");
            goto fail;
          }
	v->bat[i] = state == 6 ? (e >> 20) << 20 : 0;
        if (state == 6 && (!v->bat[i] || v->bat[i] > grub_file_size (v->file)
            || v->block_size > grub_file_size (v->file) - v->bat[i]))
          {
            grub_free (raw);
            err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: allocated block outside file");
            goto fail;
          }
      }
    grub_free (raw);
  }
  grub_free (buf);
  return GRUB_ERR_NONE;

 fail:
  grub_free (buf);
  return err ? err : (grub_errno ? grub_errno : grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHDX: cannot read"));
}

/* Open PATH and work out its format from its contents (not its name). */
static grub_err_t
vimg_open (struct vimg *v, const char *path)
{
  grub_uint8_t footer[512], sig[8];
  grub_uint64_t fsize;

  grub_memset (v, 0, sizeof (*v));
  v->file = grub_file_open (path, GRUB_FILE_TYPE_LOOPBACK | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!v->file)
    return grub_errno;
  fsize = grub_file_size (v->file);
  v->size = fsize;
  v->fmt = VIMG_RAW;

  if (fsize >= 1024 * 1024 && vimg_pread (v, 0, sig, 8) == GRUB_ERR_NONE
      && grub_memcmp (sig, "vhdxfile", 8) == 0)
    {
      v->fmt = VIMG_VHDX;
      v->size = 0;
      if (vimg_open_vhdx (v))
	goto fail;
      return GRUB_ERR_NONE;
    }
  grub_errno = GRUB_ERR_NONE;

  if (fsize >= 1024 && vimg_pread (v, fsize - 512, footer, 512) == GRUB_ERR_NONE
      && grub_memcmp (footer, "conectix", 8) == 0)
    {
      grub_uint32_t type = grub_be_to_cpu32 (grub_get_unaligned32 (footer + 60));
      if (!vimg_vhd_checksum (footer, sizeof (footer), 64))
        {
          grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHD: invalid footer checksum");
          goto fail;
        }
      v->size = grub_be_to_cpu64 (grub_get_unaligned64 (footer + 48));
      if (type == 2)
	{
	  v->fmt = VIMG_VHD_FIXED;
	  if (!v->size || v->size > fsize - 512)
            {
              grub_error (GRUB_ERR_BAD_FILE_TYPE, "VHD: truncated fixed disk");
              goto fail;
            }
	}
      else if (type == 3)
	{
	  v->fmt = VIMG_VHD_DYNAMIC;
	  if (vimg_open_vhd_dynamic (v, footer))
	    goto fail;
	}
      else
	{
	  grub_error (GRUB_ERR_BAD_FILE_TYPE,
		      "VHD: differencing disks are not supported (merge it into its parent)");
	  goto fail;
	}
    }
  if (v->fmt == VIMG_RAW && (has_suffix (path, ".vhd") || has_suffix (path, ".vhdx")))
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid or truncated VHD/VHDX container");
      goto fail;
    }
  grub_errno = GRUB_ERR_NONE;
  return GRUB_ERR_NONE;

 fail:
  {
    grub_err_t e = grub_errno ? grub_errno : GRUB_ERR_BAD_FILE_TYPE;
    char msg[GRUB_MAX_ERRMSG];
    grub_strncpy (msg, grub_errmsg, sizeof (msg) - 1);
    msg[sizeof (msg) - 1] = 0;
    grub_errno = GRUB_ERR_NONE;
    vimg_close (v);
    return grub_error (e, "%s: %s", path, msg);
  }
}

/* Read LEN bytes of the virtual disk at OFF. Unallocated blocks read as zeros. */
static grub_err_t
vimg_read (struct vimg *v, grub_uint64_t off, grub_size_t len, void *buf)
{
  grub_uint8_t *out = buf;
  if (off > v->size || len > v->size - off)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "read beyond the end of the disk image");
  if (v->fmt == VIMG_RAW || v->fmt == VIMG_VHD_FIXED)
    return vimg_pread (v, off, buf, len);
  while (len)
    {
      grub_uint64_t in;
      grub_uint64_t blk = grub_divmod64 (off, v->block_size, &in);
      grub_size_t n = v->block_size - in;
      if (n > len)
	n = len;
      if (v->fmt == VIMG_VHD_DYNAMIC && blk < v->nbat && v->bat[blk])
        {
          grub_uint8_t bits;
          grub_uint32_t bitmap = ALIGN_UP ((v->block_size / 512 + 7) / 8, 512);
          grub_uint64_t sector = in / 512;
          if (n > 512 - (in & 511))
            n = 512 - (in & 511);
          if (vimg_pread (v, v->bat[blk] - bitmap + sector / 8, &bits, 1))
            return grub_errno;
          if (!(bits & (0x80 >> (sector & 7))))
            {
              grub_memset (out, 0, n);
              out += n;
              off += n;
              len -= n;
              continue;
            }
        }
      if (blk >= v->nbat || !v->bat[blk])
	grub_memset (out, 0, n);
      else if (vimg_pread (v, v->bat[blk] + in, out, n))
	return grub_errno;
      out += n;
      off += n;
      len -= n;
    }
  return GRUB_ERR_NONE;
}

/*
 * gb_vdisk FILE  /  gb_vdisk
 *
 * Make the disk inside FILE (raw, VHD or VHDX) available as the GRUB device (gbvd), so
 * GRUB's partition and file system drivers can read it, e.g. "ls (gbvd,gpt3)/". Sets
 * gb_vdisk_format. Without arguments, removes it.
 */
static struct vimg gbvd_img;
static unsigned long gbvd_generation;

static int
gbvd_iterate (grub_disk_dev_iterate_hook_t hook, void *hook_data, grub_disk_pull_t pull)
{
  if (pull != GRUB_DISK_PULL_NONE || !gbvd_img.file)
    return 0;
  return hook ("gbvd", hook_data);
}

static grub_err_t
gbvd_open (const char *name, grub_disk_t disk)
{
  if (grub_strcmp (name, "gbvd") != 0 || !gbvd_img.file)
    return grub_error (GRUB_ERR_UNKNOWN_DEVICE, "not a Glassboot virtual disk");
  disk->total_sectors = gbvd_img.size >> GRUB_DISK_SECTOR_BITS;
  disk->max_agglomerate = 1048576 >> (GRUB_DISK_SECTOR_BITS + GRUB_DISK_CACHE_BITS);
  disk->id = gbvd_generation; /* a new id per image, so the disk cache never mixes them */
  disk->data = &gbvd_img;
  return GRUB_ERR_NONE;
}

static void
gbvd_close (grub_disk_t disk __attribute__ ((unused)))
{
}

static grub_err_t
gbvd_read (grub_disk_t disk, grub_disk_addr_t sector, grub_size_t size, char *buf)
{
  return vimg_read (disk->data, sector << GRUB_DISK_SECTOR_BITS, size << GRUB_DISK_SECTOR_BITS, buf);
}

static grub_err_t
gbvd_write (grub_disk_t disk __attribute__ ((unused)), grub_disk_addr_t sector __attribute__ ((unused)),
	    grub_size_t size __attribute__ ((unused)), const char *buf __attribute__ ((unused)))
{
  return grub_error (GRUB_ERR_WRITE_ERROR, "the Glassboot virtual disk is read-only");
}

static struct grub_disk_dev gbvd_dev = {
  .name = "gbvd",
  /* Only GRUB's host utilities use the hostdisk id; firmware builds never register it. */
  .id = GRUB_DISK_DEVICE_HOSTDISK_ID,
  .disk_iterate = gbvd_iterate,
  .disk_open = gbvd_open,
  .disk_close = gbvd_close,
  .disk_read = gbvd_read,
  .disk_write = gbvd_write,
  .next = 0
};

/* Sets gb_vdisk_windows to the first partition holding Windows' boot loader. */
static int
gbvd_find_windows (grub_disk_t disk __attribute__ ((unused)), const grub_partition_t p,
		   void *data __attribute__ ((unused)))
{
  char *part = grub_partition_get_name (p), *dev, *f1, *f2;
  int found = 0;
  if (!part)
    return 0;
  dev = grub_xasprintf ("(gbvd,%s)", part);
  f1 = grub_xasprintf ("%s/Windows/System32/winload.exe", dev);
  f2 = grub_xasprintf ("%s/Windows/System32/winload.efi", dev);
  if (dev && f1 && f2 && (file_exists (f1) || file_exists (f2)))
    {
      /* Windows' own record of its setup state. "GENERALIZE_RESEAL": never started
	 (a freshly applied image); "UNDEPLOYABLE": its first start failed. */
      char *ini = grub_xasprintf ("%s/Windows/Setup/State/State.ini", dev);
      const char *state = "ready";
      grub_file_t sf = ini ? grub_file_open (ini, GRUB_FILE_TYPE_CONFIG | GRUB_FILE_TYPE_NO_DECOMPRESS) : NULL;
      if (sf)
	{
	  char buf[513];
	  grub_ssize_t n = grub_file_read (sf, buf, sizeof (buf) - 1);
	  grub_file_close (sf);
	  if (n > 0)
	    {
	      grub_ssize_t j;
	      buf[n] = 0;
	      for (j = 0; j < n; j++)       /* tolerate UTF-16: drop NULs */
		if (!buf[j])
		  buf[j] = ' ';
	      if (grub_strstr (buf, "GENERALIZE_RESEAL"))
		state = "generalized";
	      else if (grub_strstr (buf, "UNDEPLOYABLE"))
		state = "failed";
	    }
	}
      grub_errno = GRUB_ERR_NONE;
      grub_free (ini);
      grub_env_set ("gb_vdisk_windows", dev);
      grub_env_export ("gb_vdisk_windows");
      grub_env_set ("gb_vdisk_windows_state", state);
      grub_env_export ("gb_vdisk_windows_state");
      found = 1;
    }
  grub_errno = GRUB_ERR_NONE;
  grub_free (part);
  grub_free (dev);
  grub_free (f1);
  grub_free (f2);
  return found;
}

static grub_err_t
grub_cmd_gb_vdisk (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  vimg_close (&gbvd_img);
  gbvd_generation++;
  grub_env_unset ("gb_vdisk_format");
  grub_env_unset ("gb_vdisk_windows");
  grub_env_unset ("gb_vdisk_windows_state");
  if (argc == 0)
    return GRUB_ERR_NONE;
  if (vimg_open (&gbvd_img, argv[0]))
    return grub_errno;
  if (gbvd_img.size < 512)
    {
      vimg_close (&gbvd_img);
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, "%s is too small", argv[0]);
    }
  grub_env_set ("gb_vdisk_format", vimg_names[gbvd_img.fmt]);
  grub_env_export ("gb_vdisk_format");

  /* An installed Windows inside? (native VHD boot, see gb_vhd_bcd) */
  {
    grub_disk_t d = grub_disk_open ("gbvd");
    if (d)
      {
	grub_partition_iterate (d, gbvd_find_windows, NULL);
	grub_disk_close (d);
      }
    grub_errno = GRUB_ERR_NONE;
  }
  return GRUB_ERR_NONE;
}

/*
 * gb_vhd_bcd TEMPLATE VHD_PATH DATA_DEVICE
 *
 * Native VHD boot of Windows: reads the BCD template (made by scripts/make-bcd.py) and
 * fills in, in place, the VHD's path on the data partition and that partition's identity
 * (MBR: disk signature + byte offset; GPT: disk GUID + partition GUID), the same fields
 * bcdboot writes for "vhd=[X:]\path". The result is (proc)/glassboot_bcd, which wimboot
 * hands to Windows Boot Manager. Boot manager then opens the VHD from the data partition
 * (Windows 10 1809 and later read exFAT) and loads Windows from inside it.
 */
#define BCD_PATH_FIELD 520
static char *bcd_buf;
static grub_size_t bcd_len;

static char *
bcd_get_contents (grub_size_t *sz)
{
  char *c;
  *sz = bcd_len;
  if (!bcd_buf)
    return grub_strdup ("");
  c = grub_malloc (bcd_len);
  if (c)
    grub_memcpy (c, bcd_buf, bcd_len);
  return c;
}

static struct grub_procfs_entry bcd_entry = {
  .name = "glassboot_bcd",
  .get_contents = bcd_get_contents
};

static grub_err_t
grub_cmd_gb_vhd_bcd (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  static const char marker[] = "\\GLASSBOOT-VHD-PATH";
  grub_uint8_t mk[sizeof (marker) * 2], ident[40];
  grub_file_t f;
  grub_disk_t disk = NULL;
  grub_size_t i, k, plen;
  int patched = 0, gpt = 0;
  const char *path;
  char *devname;

  if (argc != 3)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("usage: gb_vhd_bcd TEMPLATE VHD_PATH DATA_DEVICE"));
  path = argv[1];
  plen = grub_strlen (path);
  if (plen == 0 || plen * 2 + 2 > BCD_PATH_FIELD)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "VHD path too long (at most 259 characters)");
  for (i = 0; i < plen; i++)
    if ((grub_uint8_t) path[i] >= 0x80)
      return grub_error (GRUB_ERR_BAD_ARGUMENT,
			 "the VHD path must use ASCII characters only (rename the file or its folders)");

  /* The data partition's identity: offset (bytes) + MBR signature, or GPT GUIDs. */
  grub_memset (ident, 0, sizeof (ident));
  devname = grub_file_get_device_name (argv[2]);
  disk = grub_disk_open (devname ? devname : argv[2]);
  grub_free (devname);
  if (!disk || !disk->partition)
    {
      if (disk)
	grub_disk_close (disk);
      return grub_error (GRUB_ERR_BAD_DEVICE, "%s is not a partition", argv[2]);
    }
  {
    struct grub_partition *part = disk->partition;
    grub_uint64_t start = grub_partition_get_start (part);
    gpt = grub_strcmp (part->partmap->name, "gpt") == 0;
    disk->partition = part->parent;       /* read the whole disk */
    if (!gpt)
      {
	grub_uint64_t bytes = start << GRUB_DISK_SECTOR_BITS;
	grub_set_unaligned64 (ident, grub_cpu_to_le64 (bytes));
	grub_set_unaligned32 (ident + 20, grub_cpu_to_le32 (1));
	if (grub_disk_read (disk, 0, 440, 4, ident + 24))
	  goto fail;
      }
    else
      {
	grub_uint8_t hdr[92], ent[32];
	grub_uint64_t entries;
	grub_uint32_t esize;
	if (grub_disk_read (disk, 1, 0, sizeof (hdr), hdr))
	  goto fail;
	entries = grub_le_to_cpu64 (grub_get_unaligned64 (hdr + 72));
	esize = grub_le_to_cpu32 (grub_get_unaligned32 (hdr + 84));
	if (grub_disk_read (disk, entries, (grub_off_t) part->index * esize, sizeof (ent), ent))
	  goto fail;
	grub_memcpy (ident, ent + 16, 16);        /* unique partition GUID */
	grub_set_unaligned32 (ident + 20, 0);
	grub_memcpy (ident + 24, hdr + 56, 16);   /* disk GUID */
      }
    disk->partition = part;
  }
  grub_disk_close (disk);
  disk = NULL;

  f = grub_file_open (argv[0], GRUB_FILE_TYPE_CONFIG | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!f)
    return grub_errno;
  grub_free (bcd_buf);
  bcd_len = grub_file_size (f);
  bcd_buf = bcd_len < (1 << 20) ? grub_malloc (bcd_len) : NULL;
  if (!bcd_buf || grub_file_read (f, bcd_buf, bcd_len) != (grub_ssize_t) bcd_len)
    {
      grub_file_close (f);
      grub_free (bcd_buf);
      bcd_buf = NULL;
      return grub_errno ? grub_errno : grub_error (GRUB_ERR_FILE_READ_ERROR, "cannot read %s", argv[0]);
    }
  grub_file_close (f);

  for (i = 0; i < sizeof (marker) - 1; i++)
    {
      mk[2 * i] = marker[i];
      mk[2 * i + 1] = 0;
    }
  /* Each device element: [identity 40 bytes][reserved 16][path field at +0x38]. */
  for (i = 0x38; i + BCD_PATH_FIELD <= bcd_len; i += 2)
    {
      grub_uint8_t *p = (grub_uint8_t *) bcd_buf + i;
      if (grub_memcmp (p, mk, 2 * (sizeof (marker) - 1)) != 0)
	continue;
      grub_memcpy (p - 0x38, ident, sizeof (ident));
      grub_memset (p, 0, BCD_PATH_FIELD);
      for (k = 0; k < plen; k++)
	p[2 * k] = path[k] == '/' ? '\\' : path[k];
      patched++;
      i += BCD_PATH_FIELD - 2;
    }
  if (!patched)
    {
      grub_free (bcd_buf);
      bcd_buf = NULL;
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, "%s is not a Glassboot VHD BCD template", argv[0]);
    }
  return GRUB_ERR_NONE;

 fail:
  if (disk)
    grub_disk_close (disk);
  return grub_errno;
}

/* ------------------------------------------------------------------ gb_wimboot_efi */
#ifdef GRUB_MACHINE_EFI
/*
 * gb_wimboot_efi WIMBOOT [NAME PATH]...
 *
 * Start wimboot on UEFI the way iPXE does: wimboot, running as a UEFI application, reads
 * its files (BCD, boot.sdi, boot.wim, injected files) from a UEFI file system on the
 * device it was loaded from. Upstream GRUB's `linux` loader can't pass them otherwise
 * (it no longer has the EFI handover entry wimboot's initrd mode would need).
 *
 * We install a read-only EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on a new handle. Its root
 * directory lists NAME entries backed by GRUB files at PATH (read on demand, so a large
 * boot.wim isn't copied into memory). GRUB stays resident while wimboot and Windows Boot
 * Manager read through it, up to ExitBootServices.
 */
#include <grub/efi/api.h>
#include <grub/efi/efi.h>

#define GB_MAX_VFILES 48

struct gb_vfile
{
  char *name;
  grub_file_t file;
};

static struct gb_vfile vfiles[GB_MAX_VFILES];
static int nvfiles;

struct gb_file_proto;
typedef grub_efi_status_t (__grub_efi_api *gb_open_t) (struct gb_file_proto *, struct gb_file_proto **,
						       grub_efi_char16_t *, grub_efi_uint64_t, grub_efi_uint64_t);
typedef grub_efi_status_t (__grub_efi_api *gb_self_t) (struct gb_file_proto *);
typedef grub_efi_status_t (__grub_efi_api *gb_rw_t) (struct gb_file_proto *, grub_efi_uintn_t *, void *);
typedef grub_efi_status_t (__grub_efi_api *gb_getpos_t) (struct gb_file_proto *, grub_efi_uint64_t *);
typedef grub_efi_status_t (__grub_efi_api *gb_setpos_t) (struct gb_file_proto *, grub_efi_uint64_t);
typedef grub_efi_status_t (__grub_efi_api *gb_info_t) (struct gb_file_proto *, grub_guid_t *,
						       grub_efi_uintn_t *, void *);

/* EFI_FILE_PROTOCOL (revision 1 layout). */
struct gb_file_proto
{
  grub_efi_uint64_t revision;
  gb_open_t open;
  gb_self_t close;
  gb_self_t delete;
  gb_rw_t read;
  gb_rw_t write;
  gb_getpos_t get_position;
  gb_setpos_t set_position;
  gb_info_t get_info;
  gb_info_t set_info;
  gb_self_t flush;
};

/* One open handle: the root directory (index -1) or a file. */
struct gb_handle
{
  struct gb_file_proto proto; /* must be first */
  int index;
  grub_uint64_t pos;
};

struct gb_sfs
{
  grub_efi_uint64_t revision;
  grub_efi_status_t (__grub_efi_api *open_volume) (struct gb_sfs *, struct gb_file_proto **);
};

static grub_guid_t gb_sfs_guid = GRUB_EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static grub_guid_t gb_dp_guid = GRUB_EFI_DEVICE_PATH_GUID;
static grub_guid_t gb_file_info_guid =
  { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

#define EFI_FILE_INFO_HEADER 80 /* bytes before the CHAR16 file name */

static struct gb_handle *gb_new_handle (int index);

static int
name_matches (const grub_efi_char16_t *w, const char *name)
{
  for (; *w && *name; w++, name++)
    if (grub_tolower ((int) (*w & 0x7f)) != grub_tolower ((int) *name) || *w > 0x7f)
      return 0;
  return *w == 0 && *name == 0;
}

/* Fill an EFI_FILE_INFO for vfile `index` (-1 = root directory). */
static grub_efi_status_t
fill_info (int index, grub_efi_uintn_t *size, void *buf)
{
  const char *name = index < 0 ? "" : vfiles[index].name;
  grub_size_t nlen = grub_strlen (name);
  grub_efi_uintn_t need = EFI_FILE_INFO_HEADER + (nlen + 1) * 2;
  grub_uint8_t *b = buf;
  grub_uint64_t fsize = index < 0 ? 0 : grub_file_size (vfiles[index].file);
  grub_size_t i;

  if (*size < need)
    {
      *size = need;
      return GRUB_EFI_BUFFER_TOO_SMALL;
    }
  grub_memset (b, 0, need);
  grub_set_unaligned64 (b + 0, need);
  grub_set_unaligned64 (b + 8, fsize);
  grub_set_unaligned64 (b + 16, fsize);
  grub_set_unaligned64 (b + 72, index < 0 ? 0x11 : 0x01); /* DIRECTORY|READ_ONLY : READ_ONLY */
  for (i = 0; i < nlen; i++)
    grub_set_unaligned16 (b + EFI_FILE_INFO_HEADER + 2 * i, (grub_uint8_t) name[i]);
  *size = need;
  return GRUB_EFI_SUCCESS;
}

static grub_efi_status_t __grub_efi_api
gb_f_open (struct gb_file_proto *self, struct gb_file_proto **out, grub_efi_char16_t *name,
	   grub_efi_uint64_t mode, grub_efi_uint64_t attrs __attribute__ ((unused)))
{
  struct gb_handle *h = (struct gb_handle *) self, *n;
  int i;

  if (h->index >= 0)
    return GRUB_EFI_NOT_FOUND;
  if (mode != 1) /* EFI_FILE_MODE_READ */
    return GRUB_EFI_WRITE_PROTECTED;
  while (*name == '\\')
    name++;
  if (*name == 0 || (name[0] == '.' && name[1] == 0))
    i = -1;
  else
    {
      for (i = 0; i < nvfiles; i++)
	if (name_matches (name, vfiles[i].name))
	  break;
      if (i == nvfiles)
	return GRUB_EFI_NOT_FOUND;
    }
  n = gb_new_handle (i);
  if (!n)
    return GRUB_EFI_OUT_OF_RESOURCES;
  *out = &n->proto;
  return GRUB_EFI_SUCCESS;
}

static grub_efi_status_t __grub_efi_api
gb_f_close (struct gb_file_proto *self)
{
  grub_free (self);
  return GRUB_EFI_SUCCESS;
}

static grub_efi_status_t __grub_efi_api
gb_f_delete (struct gb_file_proto *self)
{
  grub_free (self);
  return GRUB_EFI_WARN_DELETE_FAILURE;
}

static grub_efi_status_t __grub_efi_api
gb_f_read (struct gb_file_proto *self, grub_efi_uintn_t *size, void *buf)
{
  struct gb_handle *h = (struct gb_handle *) self;
  grub_file_t f;
  grub_uint64_t fsize, n;
  grub_ssize_t got;

  if (h->index < 0)
    {
      /* Directory listing: one EFI_FILE_INFO per call, size 0 at the end. */
      grub_efi_status_t st;
      if (h->pos >= (grub_uint64_t) nvfiles)
	{
	  *size = 0;
	  return GRUB_EFI_SUCCESS;
	}
      st = fill_info ((int) h->pos, size, buf);
      if (st == GRUB_EFI_SUCCESS)
	h->pos++;
      return st;
    }

  f = vfiles[h->index].file;
  fsize = grub_file_size (f);
  n = h->pos >= fsize ? 0 : fsize - h->pos;
  if (n > *size)
    n = *size;
  if (n)
    {
      grub_file_seek (f, h->pos);
      got = grub_file_read (f, buf, n);
      if (got < 0 || (grub_uint64_t) got != n)
	{
	  grub_errno = GRUB_ERR_NONE;
	  return GRUB_EFI_DEVICE_ERROR;
	}
    }
  h->pos += n;
  *size = n;
  return GRUB_EFI_SUCCESS;
}

static grub_efi_status_t __grub_efi_api
gb_f_write (struct gb_file_proto *self __attribute__ ((unused)), grub_efi_uintn_t *size __attribute__ ((unused)),
	    void *buf __attribute__ ((unused)))
{
  return GRUB_EFI_WRITE_PROTECTED;
}

static grub_efi_status_t __grub_efi_api
gb_f_getpos (struct gb_file_proto *self, grub_efi_uint64_t *pos)
{
  *pos = ((struct gb_handle *) self)->pos;
  return GRUB_EFI_SUCCESS;
}

static grub_efi_status_t __grub_efi_api
gb_f_setpos (struct gb_file_proto *self, grub_efi_uint64_t pos)
{
  struct gb_handle *h = (struct gb_handle *) self;
  if (h->index < 0)
    {
      if (pos != 0)
	return GRUB_EFI_UNSUPPORTED;
      h->pos = 0;
      return GRUB_EFI_SUCCESS;
    }
  h->pos = (pos == ~(grub_efi_uint64_t) 0) ? grub_file_size (vfiles[h->index].file) : pos;
  return GRUB_EFI_SUCCESS;
}

static grub_efi_status_t __grub_efi_api
gb_f_getinfo (struct gb_file_proto *self, grub_guid_t *type, grub_efi_uintn_t *size, void *buf)
{
  if (grub_memcmp (type, &gb_file_info_guid, sizeof (*type)) != 0)
    return GRUB_EFI_UNSUPPORTED;
  return fill_info (((struct gb_handle *) self)->index, size, buf);
}

static grub_efi_status_t __grub_efi_api
gb_f_setinfo (struct gb_file_proto *self __attribute__ ((unused)), grub_guid_t *type __attribute__ ((unused)),
	      grub_efi_uintn_t *size __attribute__ ((unused)), void *buf __attribute__ ((unused)))
{
  return GRUB_EFI_WRITE_PROTECTED;
}

static grub_efi_status_t __grub_efi_api
gb_f_flush (struct gb_file_proto *self __attribute__ ((unused)))
{
  return GRUB_EFI_SUCCESS;
}

static struct gb_handle *
gb_new_handle (int index)
{
  struct gb_handle *h = grub_zalloc (sizeof (*h));
  if (!h)
    return NULL;
  h->proto.revision = 0x00010000;
  h->proto.open = gb_f_open;
  h->proto.close = gb_f_close;
  h->proto.delete = gb_f_delete;
  h->proto.read = gb_f_read;
  h->proto.write = gb_f_write;
  h->proto.get_position = gb_f_getpos;
  h->proto.set_position = gb_f_setpos;
  h->proto.get_info = gb_f_getinfo;
  h->proto.set_info = gb_f_setinfo;
  h->proto.flush = gb_f_flush;
  h->index = index;
  return h;
}

static grub_efi_status_t __grub_efi_api
gb_open_volume (struct gb_sfs *self __attribute__ ((unused)), struct gb_file_proto **root)
{
  struct gb_handle *h = gb_new_handle (-1);
  if (!h)
    return GRUB_EFI_OUT_OF_RESOURCES;
  *root = &h->proto;
  return GRUB_EFI_SUCCESS;
}

static struct gb_sfs gb_sfs = { 0x00010000, gb_open_volume };

/* Vendor hardware device path node (Glassboot GUID) + end node. */
static struct
{
  grub_uint8_t type, subtype;
  grub_uint16_t length;
  grub_uint8_t guid[16]; /* 8a2f0d6c-4b1e-4c47-9a5e-474c41535342, little-endian fields */
  grub_uint8_t end_type, end_subtype;
  grub_uint16_t end_length;
} GRUB_PACKED gb_devpath = {
  0x01, 0x04, 20,
  { 0x6c, 0x0d, 0x2f, 0x8a, 0x1e, 0x4b, 0x47, 0x4c, 0x9a, 0x5e, 0x47, 0x4c, 0x41, 0x53, 0x53, 0x42 },
  0x7f, 0xff, 4
};

static void
free_vfiles (void)
{
  int i;
  for (i = 0; i < nvfiles; i++)
    {
      if (vfiles[i].file)
	grub_file_close (vfiles[i].file);
      grub_free (vfiles[i].name);
    }
  nvfiles = 0;
}

static grub_err_t
grub_cmd_gb_wimboot_efi (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_handle_t handle = NULL, image = NULL;
  grub_efi_loaded_image_t *loaded;
  grub_efi_status_t st;
  grub_file_t wf;
  grub_off_t wsize;
  void *wbuf = NULL;
  const char *inject = NULL;
  struct name_list inj;
  int i;

  /* Optional: --inject DIR adds every file in DIR (injected into WinPE's System32). */
  if (argc >= 2 && grub_strcmp (argv[0], "--inject") == 0)
    {
      inject = argv[1];
      argc -= 2;
      argv += 2;
    }
  if (argc < 1 || (argc - 1) % 2 != 0 || (argc - 1) / 2 > GB_MAX_VFILES)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       N_("usage: gb_wimboot_efi [--inject DIR] WIMBOOT [NAME PATH]..."));

  free_vfiles ();
  if (inject && list_dir_files (inject, &inj))
    {
      for (i = 0; i < inj.n && nvfiles < GB_MAX_VFILES - (argc - 1) / 2; i++)
	{
	  char *path = grub_xasprintf ("%s/%s", inject, inj.v[i]);
	  grub_file_t f = path ? grub_file_open (path, GRUB_FILE_TYPE_LINUX_INITRD | GRUB_FILE_TYPE_NO_DECOMPRESS) : NULL;
	  grub_free (path);
	  if (!f)
	    {
	      grub_errno = GRUB_ERR_NONE;
	      continue;
	    }
	  vfiles[nvfiles].name = grub_strdup (inj.v[i]);
	  vfiles[nvfiles].file = f;
	  nvfiles++;
	}
      free_names (&inj);
    }
  for (i = 1; i + 1 < argc; i += 2)
    {
      grub_file_t f = grub_file_open (argv[i + 1], GRUB_FILE_TYPE_LINUX_INITRD | GRUB_FILE_TYPE_NO_DECOMPRESS);
      if (!f)
	{
	  free_vfiles ();
	  return grub_errno;
	}
      vfiles[nvfiles].name = grub_strdup (argv[i]);
      vfiles[nvfiles].file = f;
      nvfiles++;
    }

  wf = grub_file_open (argv[0], GRUB_FILE_TYPE_EFI_CHAINLOADED_IMAGE);
  if (!wf)
    goto fail;
  wsize = grub_file_size (wf);
  wbuf = grub_malloc (wsize);
  if (!wbuf || grub_file_read (wf, wbuf, wsize) != (grub_ssize_t) wsize)
    {
      grub_file_close (wf);
      if (!grub_errno)
	grub_error (GRUB_ERR_FILE_READ_ERROR, "short read of %s", argv[0]);
      goto fail;
    }
  grub_file_close (wf);

  st = bs->install_protocol_interface (&handle, &gb_sfs_guid, GRUB_EFI_NATIVE_INTERFACE, &gb_sfs);
  if (st == GRUB_EFI_SUCCESS)
    st = bs->install_protocol_interface (&handle, &gb_dp_guid, GRUB_EFI_NATIVE_INTERFACE, &gb_devpath);
  if (st != GRUB_EFI_SUCCESS)
    {
      grub_error (GRUB_ERR_BAD_OS, "cannot install the virtual file system (%lx)", (unsigned long) st);
      goto fail;
    }

  st = bs->load_image (0, grub_efi_image_handle, (grub_efi_device_path_t *) &gb_devpath, wbuf, wsize, &image);
  if (st != GRUB_EFI_SUCCESS)
    {
      grub_error (GRUB_ERR_BAD_OS, "cannot load wimboot (%lx)", (unsigned long) st);
      goto fail;
    }
  loaded = grub_efi_get_loaded_image (image);
  if (loaded)
    {
      loaded->device_handle = handle; /* wimboot reads its files from here */
      loaded->load_options = NULL;
      loaded->load_options_size = 0;
    }

  st = bs->start_image (image, NULL, NULL);
  /* Only reached if wimboot (or Windows Boot Manager) returned. */
  grub_error (GRUB_ERR_BAD_OS, "wimboot returned (%lx)", (unsigned long) st);
  bs->unload_image (image);

 fail:
  if (handle)
    {
      bs->uninstall_protocol_interface (handle, &gb_dp_guid, &gb_devpath);
      bs->uninstall_protocol_interface (handle, &gb_sfs_guid, &gb_sfs);
    }
  grub_free (wbuf);
  free_vfiles ();
  return grub_errno;
}
/* ------------------------------------------------------------------ gb_efi_vdisk_boot */
/*
 * gb_efi_vdisk_boot cd|hd|vhd FILE
 *
 * Present FILE (an ISO, a raw disk image, or a fixed-size VHD) to the firmware as a
 * read-only EFI_BLOCK_IO device: a 2048-byte-sector "CD" for ISOs, a 512-byte-sector
 * disk otherwise. The firmware's own partition driver then finds the El Torito boot
 * image / MBR / GPT partitions on it, its FAT driver mounts the EFI system partition,
 * and we start \EFI\BOOT\BOOT<arch>.EFI from there, exactly as if the ISO had been
 * burned to a disc. The boot loader can keep reading the image through BlockIo (GRUB
 * serves the reads) until ExitBootServices.
 */
struct gb_vdisk
{
  grub_efi_block_io_t bio; /* must be first */
  grub_efi_block_io_media_t media;
  struct vimg img;
  grub_uint64_t size;      /* bytes exposed */
};

static struct gb_vdisk gb_vd;
static grub_guid_t gb_bio_guid = GRUB_EFI_BLOCK_IO_GUID;

/* Vendor hardware node (a different Glassboot GUID) + end node. */
static struct
{
  grub_uint8_t type, subtype;
  grub_uint16_t length;
  grub_uint8_t guid[16]; /* 8a2f0d6c-4b1e-4c47-9a5e-474c4153534b */
  grub_uint8_t end_type, end_subtype;
  grub_uint16_t end_length;
} GRUB_PACKED gb_vd_devpath = {
  0x01, 0x04, 20,
  { 0x6c, 0x0d, 0x2f, 0x8a, 0x1e, 0x4b, 0x47, 0x4c, 0x9a, 0x5e, 0x47, 0x4c, 0x41, 0x53, 0x53, 0x4b },
  0x7f, 0xff, 4
};

static grub_efi_status_t __grub_efi_api
gb_vd_reset (grub_efi_block_io_t *this __attribute__ ((unused)),
	     grub_efi_boolean_t ext __attribute__ ((unused)))
{
  return GRUB_EFI_SUCCESS;
}

static grub_efi_status_t __grub_efi_api
gb_vd_read (grub_efi_block_io_t *this, grub_efi_uint32_t media_id, grub_efi_lba_t lba,
	    grub_efi_uintn_t size, void *buf)
{
  struct gb_vdisk *vd = (struct gb_vdisk *) this;
  grub_uint64_t off;

  if (media_id != vd->media.media_id)
    return GRUB_EFI_MEDIA_CHANGED;
  if (!buf)
    return GRUB_EFI_INVALID_PARAMETER;
  if (size == 0)
    return GRUB_EFI_SUCCESS;
  if (size % vd->media.block_size)
    return GRUB_EFI_BAD_BUFFER_SIZE;
  off = lba * vd->media.block_size;
  if (lba > vd->media.last_block || off + size > vd->size)
    return GRUB_EFI_INVALID_PARAMETER;
  if (vimg_read (&vd->img, off, size, buf))
    {
      grub_errno = GRUB_ERR_NONE;
      return GRUB_EFI_DEVICE_ERROR;
    }
  return GRUB_EFI_SUCCESS;
}

static grub_efi_status_t __grub_efi_api
gb_vd_write (grub_efi_block_io_t *this __attribute__ ((unused)), grub_efi_uint32_t media_id __attribute__ ((unused)),
	     grub_efi_lba_t lba __attribute__ ((unused)), grub_efi_uintn_t size __attribute__ ((unused)),
	     void *buf __attribute__ ((unused)))
{
  return GRUB_EFI_WRITE_PROTECTED;
}

static grub_efi_status_t __grub_efi_api
gb_vd_flush (grub_efi_block_io_t *this __attribute__ ((unused)))
{
  return GRUB_EFI_SUCCESS;
}

/* Size of a device path including its end node. */
static grub_size_t
dp_size (const grub_efi_device_path_t *dp)
{
  const grub_uint8_t *p = (const grub_uint8_t *) dp;
  grub_size_t n = 0;
  while (1)
    {
      grub_uint16_t len = grub_get_unaligned16 (p + n + 2);
      if (len < 4)
	return 0;
      if (p[n] == 0x7f && p[n + 1] == 0xff)
	return n + len;
      n += len;
    }
}

/* DEVICE + MEDIA_FILEPATH(path) + END */
static grub_efi_device_path_t *
dp_append_file (const grub_efi_device_path_t *dev, const char *path)
{
  grub_size_t dsize = dp_size (dev), plen = grub_strlen (path), fnode = 4 + (plen + 1) * 2, i;
  grub_uint8_t *out, *f;
  if (dsize < 4)
    return NULL;
  out = grub_zalloc (dsize - 4 + fnode + 4);
  if (!out)
    return NULL;
  grub_memcpy (out, dev, dsize - 4);
  f = out + dsize - 4;
  f[0] = 0x04; /* MEDIA_DEVICE_PATH */
  f[1] = 0x04; /* MEDIA_FILEPATH_DP */
  grub_set_unaligned16 (f + 2, (grub_uint16_t) fnode);
  for (i = 0; i < plen; i++)
    grub_set_unaligned16 (f + 4 + 2 * i, (grub_uint8_t) path[i]);
  f += fnode;
  f[0] = 0x7f;
  f[1] = 0xff;
  grub_set_unaligned16 (f + 2, 4);
  return (grub_efi_device_path_t *) out;
}

static grub_err_t
grub_cmd_gb_efi_vdisk_boot (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_handle_t handle = NULL, *fs_handles = NULL;
  grub_efi_uintn_t nfs = 0, i;
  grub_efi_status_t st;
  grub_uint32_t block;
  int tried = 0;
#if defined (__aarch64__)
  const char *loader = "\\EFI\\BOOT\\BOOTAA64.EFI";
#elif defined (__x86_64__)
  const char *loader = "\\EFI\\BOOT\\BOOTX64.EFI";
#else
  const char *loader = "\\EFI\\BOOT\\BOOTIA32.EFI";
#endif

  if (argc != 2 || (grub_strcmp (argv[0], "cd") && grub_strcmp (argv[0], "hd") && grub_strcmp (argv[0], "vhd")))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("usage: gb_efi_vdisk_boot cd|hd|vhd FILE"));

  grub_memset (&gb_vd, 0, sizeof (gb_vd));
  /* The format (raw, VHD fixed/dynamic, VHDX) comes from the file's contents. */
  if (vimg_open (&gb_vd.img, argv[1]))
    return grub_errno;
  block = argv[0][0] == 'c' ? 2048 : 512;
  if (block == 2048 && gb_vd.img.fmt != VIMG_RAW)
    {
      vimg_close (&gb_vd.img);
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, "%s is a disk image, not a CD image", argv[1]);
    }
  gb_vd.size = gb_vd.img.size;

  {
    /* grub_divmod64: i386-efi has no libgcc 64-bit division. */
    grub_uint64_t rem, blocks = grub_divmod64 (gb_vd.size, block, &rem);
    gb_vd.size -= rem;
    gb_vd.media.last_block = blocks ? blocks - 1 : 0;
  }
  if (gb_vd.size < block)
    {
      vimg_close (&gb_vd.img);
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, "%s is too small", argv[1]);
    }
  gb_vd.media.media_id = 0x47420001;
  gb_vd.media.removable_media = 1;
  gb_vd.media.media_present = 1;
  gb_vd.media.read_only = 1;
  gb_vd.media.block_size = block;
  gb_vd.media.io_align = 0;
  gb_vd.bio.revision = 0x00010000;
  gb_vd.bio.media = &gb_vd.media;
  gb_vd.bio.reset = gb_vd_reset;
  gb_vd.bio.read_blocks = gb_vd_read;
  gb_vd.bio.write_blocks = gb_vd_write;
  gb_vd.bio.flush_blocks = gb_vd_flush;

  st = bs->install_protocol_interface (&handle, &gb_dp_guid, GRUB_EFI_NATIVE_INTERFACE, &gb_vd_devpath);
  if (st == GRUB_EFI_SUCCESS)
    st = bs->install_protocol_interface (&handle, &gb_bio_guid, GRUB_EFI_NATIVE_INTERFACE, &gb_vd.bio);
  if (st != GRUB_EFI_SUCCESS)
    {
      grub_error (GRUB_ERR_BAD_OS, "cannot install the virtual disk (%lx)", (unsigned long) st);
      goto out;
    }

  /* Let the firmware's partition (MBR/GPT/El Torito) and FAT drivers bind to it. */
  bs->connect_controller (handle, NULL, NULL, 1);

  /* Try every file system that sits on our device, in firmware order. */
  fs_handles = grub_efi_locate_handle (GRUB_EFI_BY_PROTOCOL, &gb_sfs_guid, NULL, &nfs);
  for (i = 0; fs_handles && i < nfs; i++)
    {
      grub_efi_device_path_t *dp = grub_efi_get_device_path (fs_handles[i]), *fp;
      grub_efi_handle_t image = NULL;
      if (!dp || grub_memcmp (dp, &gb_vd_devpath, 20) != 0)
	continue;
      fp = dp_append_file (dp, loader);
      if (!fp)
	continue;
      tried++;
      st = bs->load_image (0, grub_efi_image_handle, fp, NULL, 0, &image);
      grub_free (fp);
      if (st != GRUB_EFI_SUCCESS)
	continue;
      grub_printf ("Starting %s from %s\n", loader, argv[1]);
      st = bs->start_image (image, NULL, NULL);
      grub_error (GRUB_ERR_BAD_OS, "the image's boot loader returned (%lx)", (unsigned long) st);
      goto out;
    }
  if (tried)
    grub_error (GRUB_ERR_FILE_NOT_FOUND, "no %s on %s", loader, argv[1]);
  else
    grub_error (GRUB_ERR_BAD_OS, "the firmware found no EFI-bootable file system on %s", argv[1]);

 out:
  grub_free (fs_handles);
  if (handle)
    {
      bs->disconnect_controller (handle, NULL, NULL);
      bs->uninstall_protocol_interface (handle, &gb_bio_guid, &gb_vd.bio);
      bs->uninstall_protocol_interface (handle, &gb_dp_guid, &gb_vd_devpath);
    }
  vimg_close (&gb_vd.img);
  return grub_errno;
}
#endif /* GRUB_MACHINE_EFI */

/* ------------------------------------------------------------------ gb_scan_os */
/*
 * gb_scan_os OWN_DISK
 *
 * Finds installed operating systems on every disk except OWN_DISK (e.g. "hd0") and
 * publishes a menu as (proc)/glassboot_os. Sets gb_os_count.
 *   UEFI: EFI boot loaders in each FAT partition's \EFI\<vendor>\ folder.
 *   BIOS: every other disk with an MBR boot signature (chainload its boot sector).
 */
static char *os_text;

struct os_ctx
{
  const char *own;
  struct sbuf menu;
  int count;
  int efi;
};

#ifdef GRUB_MACHINE_EFI
/* Loader file names per vendor folder, in order of preference. */
static const char *const efi_loaders[] = {
#if defined (__aarch64__)
  "shimaa64.efi", "grubaa64.efi", "systemd-bootaa64.efi",
#elif defined (__x86_64__)
  "shimx64.efi", "grubx64.efi", "systemd-bootx64.efi",
#else
  "shimia32.efi", "grubia32.efi", "systemd-bootia32.efi",
#endif
  NULL
};
#endif

static void
os_add (struct os_ctx *c, const char *title, const char *body)
{
  sbuf_add (&c->menu, "menuentry \"");
  sbuf_add_quoted (&c->menu, title);
  sbuf_add (&c->menu, "\" --class os {\n");
  sbuf_add (&c->menu, body);
  sbuf_add (&c->menu, "}\n");
  c->count++;
}

#ifdef GRUB_MACHINE_EFI
/*
 * UEFI: scan through the firmware rather than GRUB's disk list. Firmware in "fast boot"
 * mode only connects the boot device, and GRUB's EFI disk list is built once at start-up,
 * so other disks may be invisible to GRUB. We connect every controller, then look at each
 * EFI_SIMPLE_FILE_SYSTEM the firmware's own FAT driver provides (skipping the Glassboot
 * drive) and boot a choice with LoadImage(device path + file), like the firmware's menu.
 */
#include <grub/efi/disk.h>

#define GB_MAX_OS 32
static grub_efi_device_path_t *os_paths[GB_MAX_OS];
static int os_npaths;

static void
os_free_paths (void)
{
  int i;
  for (i = 0; i < os_npaths; i++)
    grub_free (os_paths[i]);
  os_npaths = 0;
}

static grub_efi_char16_t *
to_u16 (const char *s)
{
  grub_size_t n = grub_strlen (s), i;
  grub_efi_char16_t *w = grub_calloc (n + 1, sizeof (*w));
  if (w)
    for (i = 0; i < n; i++)
      w[i] = (grub_uint8_t) s[i];
  return w;
}

static int
efi_file_exists (struct gb_file_proto *root, const char *path)
{
  struct gb_file_proto *f = NULL;
  grub_efi_char16_t *w = to_u16 (path);
  grub_efi_status_t st;
  if (!w)
    return 0;
  st = root->open (root, &f, w, 1, 0);
  grub_free (w);
  if (st != GRUB_EFI_SUCCESS || !f)
    return 0;
  f->close (f);
  return 1;
}

static void
efi_os_add (struct os_ctx *c, grub_efi_device_path_t *dp, const char *path, const char *title, int disk)
{
  char *t, *body;
  if (os_npaths >= GB_MAX_OS)
    return;
  os_paths[os_npaths] = dp_append_file (dp, path);
  if (!os_paths[os_npaths])
    return;
  t = grub_xasprintf ("%s    (disk %d, %s)", title, disk, path);
  body = grub_xasprintf ("  gb_efi_boot_os %d\n", os_npaths);
  os_npaths++;
  if (t && body)
    os_add (c, t, body);
  grub_free (t);
  grub_free (body);
}

static void
efi_scan_os (struct os_ctx *c)
{
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_handle_t *handles;
  grub_efi_uintn_t n = 0, i;
  grub_efi_device_path_t *own_dp = NULL;
  grub_size_t own_len = 0;
  int disk = 0;

  os_free_paths ();

  /* Connect every controller so all disks and their file systems appear. */
  handles = grub_efi_locate_handle (GRUB_EFI_ALL_HANDLES, NULL, NULL, &n);
  for (i = 0; handles && i < n; i++)
    bs->connect_controller (handles[i], NULL, NULL, 1);
  grub_free (handles);

  /* The Glassboot drive's device path: file systems below it are skipped. */
  if (c->own[0])
    {
      grub_disk_t d = grub_disk_open (c->own);
      if (d)
	{
	  grub_efi_handle_t h = grub_efidisk_get_device_handle (d);
	  if (h)
	    own_dp = grub_efi_get_device_path (h);
	  grub_disk_close (d);
	}
      grub_errno = GRUB_ERR_NONE;
      if (own_dp)
	own_len = dp_size (own_dp) - 4; /* without the end node */
    }

  handles = grub_efi_locate_handle (GRUB_EFI_BY_PROTOCOL, &gb_sfs_guid, NULL, &n);
  if (grub_env_get ("gb_debug"))
    {
      grub_printf ("own=%s own_len=%d sfs=%d own_dp: ", c->own, (int) own_len, (int) n);
      if (own_dp)
	grub_efi_print_device_path (own_dp);
      grub_printf ("\n");
    }
  for (i = 0; handles && i < n; i++)
    {
      grub_efi_device_path_t *dp = grub_efi_get_device_path (handles[i]);
      struct gb_sfs *fs;
      struct gb_file_proto *root = NULL, *efi = NULL;
      grub_efi_char16_t *w;
      int found = 0, j;

      if (grub_env_get ("gb_debug") && dp)
	{
	  grub_printf ("sfs %d: ", (int) i);
	  grub_efi_print_device_path (dp);
	  grub_printf ("\n");
	}

      if (!dp)
	continue;
      if (own_len && dp_size (dp) >= own_len && grub_memcmp (dp, own_dp, own_len) == 0)
	continue; /* a partition of the Glassboot drive */
      if (grub_memcmp (dp, &gb_vd_devpath, 20) == 0 || grub_memcmp (dp, &gb_devpath, 20) == 0)
	continue; /* our own virtual devices */
      fs = grub_efi_open_protocol (handles[i], &gb_sfs_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
      if (!fs || fs->open_volume (fs, &root) != GRUB_EFI_SUCCESS || !root)
	continue;
      disk++;

      if (efi_file_exists (root, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi"))
	{
	  efi_os_add (c, dp, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi", "Windows Boot Manager", disk);
	  found++;
	}

      /* Other vendors: \EFI\<vendor>\<known loader>. */
      w = to_u16 ("\\EFI");
      if (w && root->open (root, &efi, w, 1, 0) == GRUB_EFI_SUCCESS && efi)
	{
	  grub_uint8_t buf[1024];
	  while (1)
	    {
	      grub_efi_uintn_t size = sizeof (buf);
	      char name[256];
	      grub_size_t k, nlen;
	      if (efi->read (efi, &size, buf) != GRUB_EFI_SUCCESS || size == 0)
		break;
	      if (!(grub_get_unaligned64 (buf + 72) & 0x10))
		continue; /* not a directory */
	      for (k = 0; k < sizeof (name) - 1; k++)
		{
		  grub_uint16_t ch = grub_get_unaligned16 (buf + 80 + 2 * k);
		  if (!ch)
		    break;
		  name[k] = ch < 0x80 ? (char) ch : '_';
		}
	      name[k] = 0;
	      nlen = k;
	      if (!nlen || name[0] == '.' || grub_strcasecmp (name, "Microsoft") == 0
		  || grub_strcasecmp (name, "BOOT") == 0)
		continue;
	      for (j = 0; efi_loaders[j]; j++)
		{
		  char *path = grub_xasprintf ("\\EFI\\%s\\%s", name, efi_loaders[j]);
		  if (path && efi_file_exists (root, path))
		    {
		      efi_os_add (c, dp, path, name, disk);
		      found++;
		      grub_free (path);
		      break;
		    }
		  grub_free (path);
		}
	    }
	  efi->close (efi);
	}
      grub_free (w);

      /* Nothing recognised: the firmware's fallback loader, if any. */
      if (!found)
	{
#if defined (__aarch64__)
	  const char *fb = "\\EFI\\BOOT\\BOOTAA64.EFI";
#elif defined (__x86_64__)
	  const char *fb = "\\EFI\\BOOT\\BOOTX64.EFI";
#else
	  const char *fb = "\\EFI\\BOOT\\BOOTIA32.EFI";
#endif
	  if (efi_file_exists (root, fb))
	    efi_os_add (c, dp, fb, "EFI boot loader", disk);
	}
      root->close (root);
    }
  grub_free (handles);
}

static grub_err_t
grub_cmd_gb_efi_boot_os (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_handle_t image = NULL;
  grub_efi_status_t st;
  int idx;

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("usage: gb_efi_boot_os N"));
  idx = grub_strtol (argv[0], NULL, 10);
  if (idx < 0 || idx >= os_npaths)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "no such entry");
  st = bs->load_image (0, grub_efi_image_handle, os_paths[idx], NULL, 0, &image);
  if (st != GRUB_EFI_SUCCESS)
    return grub_error (GRUB_ERR_BAD_OS, "cannot load the boot loader (%lx)", (unsigned long) st);
  st = bs->start_image (image, NULL, NULL);
  return grub_error (GRUB_ERR_BAD_OS, "the boot loader returned (%lx)", (unsigned long) st);
}
#endif /* GRUB_MACHINE_EFI */

static int
os_device_hook (const char *name, void *data)
{
  struct os_ctx *c = data;
  grub_size_t olen = grub_strlen (c->own);
  const char *comma = grub_strchr (name, ',');
  grub_size_t dlen = comma ? (grub_size_t) (comma - name) : grub_strlen (name);

  /* Skip the Glassboot drive itself, loop devices, and non-disk devices. */
  if ((dlen == olen && grub_strncmp (name, c->own, olen) == 0) || grub_strncmp (name, "hd", 2) != 0)
    return 0;

  if (c->efi)
    return 0;
  else if (!comma)
    {
      grub_disk_t disk = grub_disk_open (name);
      grub_uint8_t sig[2];
      if (disk)
	{
	  if (grub_disk_read (disk, 0, 510, 2, sig) == GRUB_ERR_NONE && sig[0] == 0x55 && sig[1] == 0xAA)
	    {
	      char *title = grub_xasprintf ("Boot disk %s (its own boot loader)", name);
	      char *body = grub_xasprintf ("  set root=(%s)\n  drivemap -s (hd0) (%s)\n  chainloader +1\n  boot\n",
					   name, name);
	      if (title && body)
		os_add (c, title, body);
	      grub_free (title);
	      grub_free (body);
	    }
	  grub_disk_close (disk);
	}
      grub_errno = GRUB_ERR_NONE;
    }
  return 0;
}

static char *
os_get_contents (grub_size_t *sz)
{
  *sz = os_text ? grub_strlen (os_text) : 0;
  return grub_strdup (os_text ? os_text : "");
}

static struct grub_procfs_entry os_entry = {
  .name = "glassboot_os",
  .get_contents = os_get_contents
};

static grub_err_t
grub_cmd_gb_scan_os (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  struct os_ctx c;
  const char *platform = grub_env_get ("grub_platform");
  char count[16];

  grub_memset (&c, 0, sizeof (c));
  c.own = argc > 0 ? argv[0] : "";
  c.efi = platform && grub_strcmp (platform, "efi") == 0;
#ifdef GRUB_MACHINE_EFI
  if (c.efi)
    efi_scan_os (&c);
  else
#endif
    grub_device_iterate (os_device_hook, &c);
  grub_free (os_text);
  os_text = c.menu.data ? c.menu.data : grub_strdup ("");
  grub_snprintf (count, sizeof (count), "%d", c.count);
  grub_env_set ("gb_os_count", count);
  grub_env_export ("gb_os_count");
  return GRUB_ERR_NONE;
}

/* ------------------------------------------------------------------ module */

/* Corruption detection, not a substitute for a signed/Secure Boot trust chain:
   this command and its manifest are on the same writable ESP. */
static grub_err_t
grub_cmd_gb_verify_esp (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  grub_file_t manifest;
  char *text, *line, *next;
  grub_size_t size;
  unsigned count = 0;
  const gcry_md_spec_t *md;
  static const char hex[] = "0123456789abcdef";
  if (argc != 2) return grub_error (GRUB_ERR_BAD_ARGUMENT, "usage: gb_verify_esp MANIFEST ESP_DEVICE");
  grub_dl_load ("gcry_sha256");
  grub_errno = GRUB_ERR_NONE;
  md = grub_crypto_lookup_md_by_name ("sha256");
  if (!md) return grub_error (GRUB_ERR_BAD_ARGUMENT, "SHA-256 unavailable");
  manifest = grub_file_open (argv[0], GRUB_FILE_TYPE_CONFIG | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!manifest) return grub_errno;
  size = grub_file_size (manifest);
  if (!size || size > 512 * 1024) { grub_file_close (manifest); return grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid boot manifest size"); }
  text = grub_zalloc (size + 1);
  if (!text || grub_file_read (manifest, text, size) != (grub_ssize_t) size)
    { grub_file_close (manifest); grub_free (text); return grub_error (GRUB_ERR_FILE_READ_ERROR, "cannot read boot manifest"); }
  grub_file_close (manifest);
  for (line = text; line; line = next)
    {
      char *path, *full, actual[65];
      grub_file_t file;
      grub_uint8_t *bytes, hash[32];
      grub_size_t len, i;
      next = grub_strchr (line, '\n'); if (next) *next++ = 0;
      if (!*line || *line == '#') continue;
      if (grub_strlen (line) < 67 || line[64] != ' ' || line[65] != ' ')
        { grub_free (text); return grub_error (GRUB_ERR_BAD_FILE_TYPE, "malformed boot manifest"); }
      path = line + 66;
      if (*path == '/' || grub_strstr (path, "..") || grub_strchr (path, '\\') || grub_strchr (path, '\r'))
        { grub_free (text); return grub_error (GRUB_ERR_BAD_FILE_TYPE, "unsafe boot manifest path"); }
      full = grub_xasprintf ("%s/%s", argv[1], path);
      if (!full) { grub_free (text); return grub_errno; }
      file = grub_file_open (full, GRUB_FILE_TYPE_CONFIG | GRUB_FILE_TYPE_NO_DECOMPRESS);
      grub_free (full);
      if (!file) { grub_free (text); return grub_errno; }
      len = grub_file_size (file);
      if (len > 32 * 1024 * 1024)
        { grub_file_close (file); grub_free (text); return grub_error (GRUB_ERR_BAD_FILE_TYPE, "boot file too large"); }
      bytes = grub_malloc (len ? len : 1);
      if (!bytes || grub_file_read (file, bytes, len) != (grub_ssize_t) len)
        { grub_file_close (file); grub_free (bytes); grub_free (text); return grub_error (GRUB_ERR_FILE_READ_ERROR, "cannot read boot file"); }
      grub_file_close (file);
      grub_crypto_hash (md, hash, bytes, len); grub_free (bytes);
      for (i = 0; i < 32; i++) { actual[2*i] = hex[hash[i] >> 4]; actual[2*i+1] = hex[hash[i] & 15]; }
      actual[64] = 0;
      if (grub_strncasecmp (actual, line, 64))
        {
          grub_error (GRUB_ERR_BAD_FILE_TYPE, "boot file checksum mismatch: %s", path);
          grub_free (text); return grub_errno;
        }
      count++;
    }
  grub_free (text);
  if (!count) return grub_error (GRUB_ERR_BAD_FILE_TYPE, "empty boot manifest");
  grub_printf ("Glassboot: verified %u boot files.\n", count);
  return GRUB_ERR_NONE;
}

/* GRUB's BSD loader predates FreeBSD's move from hint.acpi.0.rsdp to
   acpi.rsdp. EFI has no legacy BIOS scan area: pass the firmware pointer. */
static grub_err_t
grub_cmd_gb_freebsd_env (grub_command_t cmd __attribute__ ((unused)),
                         int argc __attribute__ ((unused)), char **argv __attribute__ ((unused)))
{
  void *rsdp = grub_machine_acpi_get_rsdpv2 ();
  char addr[32];
  grub_env_unset ("kFreeBSD.acpi.rsdp");
  grub_env_unset ("kFreeBSD.hint.acpi.0.rsdp");
  if (!rsdp)
    rsdp = grub_machine_acpi_get_rsdpv1 ();
  if (rsdp)
    {
      grub_snprintf (addr, sizeof (addr), "0x%llx", (unsigned long long) (grub_addr_t) rsdp);
      grub_env_set ("kFreeBSD.acpi.rsdp", addr);
      grub_env_set ("kFreeBSD.hint.acpi.0.rsdp", addr);
    }
  return GRUB_ERR_NONE;
}

static grub_command_t cmd_scan, cmd_enable, cmd_disable, cmd_config, cmd_scan_os, cmd_vdisk, cmd_vhd_bcd, cmd_freebsd_env, cmd_hook_add_initrd, cmd_verify_esp;
#ifdef GRUB_MACHINE_EFI
static grub_command_t cmd_wimboot_efi, cmd_vdisk_boot, cmd_efi_boot_os;
#endif

GRUB_MOD_INIT (glassboot)
{
  int map;
  cmd_verify_esp = grub_register_command ("gb_verify_esp", grub_cmd_gb_verify_esp, N_("MANIFEST ESP_DEVICE"),
                                        N_("Verify the boot-file SHA-256 manifest."));
  for (map = 0; map < 3; map++) grub_procfs_register (hook_map_entries[map].name, &hook_map_entries[map]);
  cmd_freebsd_env = grub_register_command ("gb_freebsd_env", grub_cmd_gb_freebsd_env, 0,
                                          N_("Pass firmware ACPI information to FreeBSD."));
  cmd_scan = grub_register_command ("gb_scan", grub_cmd_gb_scan, N_("(DEVICE)"),
				    N_("Find ISO files and build the Glassboot menu."));
  cmd_enable = grub_register_command ("gb_hook_enable", grub_cmd_gb_hook_enable,
				      N_("ISO_PATH PART_UUID INITRD"),
				      N_("Add the Glassboot hook to Linux boots."));
  cmd_disable = grub_register_command ("gb_hook_disable", grub_cmd_gb_hook_disable, 0,
				       N_("Stop adding the Glassboot hook."));
  cmd_hook_add_initrd = grub_register_command ("gb_hook_add_initrd", grub_cmd_gb_hook_add_initrd, N_("INITRD"),
                                              N_("Append an initrd file to the active Linux hook."));
  grub_procfs_register ("glassboot_menu", &menu_entry);
#ifdef GRUB_MACHINE_EFI
  cmd_wimboot_efi = grub_register_command ("gb_wimboot_efi", grub_cmd_gb_wimboot_efi,
					   N_("WIMBOOT [NAME PATH]..."),
					   N_("Start wimboot with files served from a virtual UEFI file system."));
  cmd_vdisk_boot = grub_register_command ("gb_efi_vdisk_boot", grub_cmd_gb_efi_vdisk_boot,
					  N_("cd|hd|vhd FILE"),
					  N_("Expose FILE as a virtual CD/disk and start its EFI boot loader."));
#endif
#ifdef GRUB_MACHINE_EFI
  cmd_efi_boot_os = grub_register_command ("gb_efi_boot_os", grub_cmd_gb_efi_boot_os, N_("N"),
					   N_("Boot entry N found by gb_scan_os."));
#endif
  cmd_config = grub_register_command ("gb_config", grub_cmd_gb_config, N_("FILE"),
				      N_("Read the Glassboot configuration (glassboot.cfg)."));
  cmd_scan_os = grub_register_command ("gb_scan_os", grub_cmd_gb_scan_os, N_("OWN_DISK"),
				       N_("Find installed operating systems on other disks."));
  grub_procfs_register ("glassboot_os", &os_entry);
  cmd_vdisk = grub_register_command ("gb_vdisk", grub_cmd_gb_vdisk, N_("[FILE]"),
				     N_("Make the disk inside FILE (raw, VHD, VHDX) available as (gbvd)."));
  grub_disk_dev_register (&gbvd_dev);
  cmd_vhd_bcd = grub_register_command ("gb_vhd_bcd", grub_cmd_gb_vhd_bcd,
				       N_("TEMPLATE VHD_PATH DATA_DEVICE"),
				       N_("Make (proc)/glassboot_bcd for booting Windows from a VHD."));
  grub_procfs_register ("glassboot_bcd", &bcd_entry);
}

GRUB_MOD_FINI (glassboot)
{
  int map;
  grub_unregister_command (cmd_verify_esp);
  for (map = 0; map < 3; map++) grub_procfs_unregister (&hook_map_entries[map]);
  grub_unregister_command (cmd_freebsd_env);
  uninstall_wrappers ();
  grub_procfs_unregister (&menu_entry);
  grub_procfs_unregister (&os_entry);
  grub_unregister_command (cmd_config);
  grub_unregister_command (cmd_scan_os);
  grub_unregister_command (cmd_vdisk);
  grub_unregister_command (cmd_vhd_bcd);
  grub_procfs_unregister (&bcd_entry);
  grub_free (bcd_buf);
  grub_disk_dev_unregister (&gbvd_dev);
  vimg_close (&gbvd_img);
  cfg_clear ();
  grub_free (os_text);
  grub_unregister_command (cmd_scan);
  grub_unregister_command (cmd_enable);
  grub_unregister_command (cmd_disable);
  grub_unregister_command (cmd_hook_add_initrd);
#ifdef GRUB_MACHINE_EFI
  grub_unregister_command (cmd_wimboot_efi);
  grub_unregister_command (cmd_vdisk_boot);
  grub_unregister_command (cmd_efi_boot_os);
  os_free_paths ();
#endif
  grub_free (menu_text);
  free_hook_args ();
}
