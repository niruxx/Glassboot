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
#include <grub/device.h>
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
 */
enum { CFG_ALIAS, CFG_HIDE, CFG_ARGS, CFG_PERSIST };

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

      if (type < 0)
	{
	  if (grub_strcasecmp (key, "timeout") == 0 || grub_strcasecmp (key, "default") == 0
	      || grub_strcasecmp (key, "view") == 0 || grub_strcasecmp (key, "theme") == 0
	      || grub_strcasecmp (key, "password") == 0)
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
      cfg_items[cfg_nitems].value = type == CFG_PERSIST ? norm_path (trim (eq + 1)) : grub_strdup (trim (eq + 1));
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
  if (has_suffix (name, ".img") || has_suffix (name, ".vhd"))
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
  sbuf_add (menu, kind_class[e->kind]);
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

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("usage: gb_scan (DEVICE)"));

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
#define HOOK_MAX_INITRDS 12
static char *hook_initrds[HOOK_MAX_INITRDS];
static int hook_ninitrds;

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
  grub_file_t file;
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
  grub_file_seek (vd->file, off);
  if (grub_file_read (vd->file, buf, size) != (grub_ssize_t) size)
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
  grub_uint64_t fsize;
  int tried = 0;
#if defined (__x86_64__)
  const char *loader = "\\EFI\\BOOT\\BOOTX64.EFI";
#else
  const char *loader = "\\EFI\\BOOT\\BOOTIA32.EFI";
#endif

  if (argc != 2 || (grub_strcmp (argv[0], "cd") && grub_strcmp (argv[0], "hd") && grub_strcmp (argv[0], "vhd")))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("usage: gb_efi_vdisk_boot cd|hd|vhd FILE"));

  grub_memset (&gb_vd, 0, sizeof (gb_vd));
  gb_vd.file = grub_file_open (argv[1], GRUB_FILE_TYPE_LOOPBACK | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!gb_vd.file)
    return grub_errno;
  fsize = grub_file_size (gb_vd.file);
  block = argv[0][0] == 'c' ? 2048 : 512;
  gb_vd.size = fsize;

  if (argv[0][0] == 'v')
    {
      /* VHD footer: "conectix" + big-endian disk type at offset 60 (2 = fixed). */
      grub_uint8_t footer[512];
      grub_uint32_t type;
      grub_file_seek (gb_vd.file, fsize - 512);
      if (fsize < 1024 || grub_file_read (gb_vd.file, footer, 512) != 512
	  || grub_memcmp (footer, "conectix", 8) != 0)
	{
	  grub_file_close (gb_vd.file);
	  return grub_error (GRUB_ERR_BAD_FILE_TYPE, "%s is not a VHD file", argv[1]);
	}
      type = grub_be_to_cpu32 (grub_get_unaligned32 (footer + 60));
      if (type != 2)
	{
	  grub_file_close (gb_vd.file);
	  return grub_error (GRUB_ERR_BAD_FILE_TYPE,
			     "only fixed-size VHDs are supported (convert dynamic VHDs to fixed)");
	}
      gb_vd.size = fsize - 512;
    }

  {
    /* grub_divmod64: i386-efi has no libgcc 64-bit division. */
    grub_uint64_t rem, blocks = grub_divmod64 (gb_vd.size, block, &rem);
    gb_vd.size -= rem;
    gb_vd.media.last_block = blocks ? blocks - 1 : 0;
  }
  if (gb_vd.size < block)
    {
      grub_file_close (gb_vd.file);
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
  grub_file_close (gb_vd.file);
  gb_vd.file = NULL;
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
#if defined (__x86_64__)
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
#if defined (__x86_64__)
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

static grub_command_t cmd_scan, cmd_enable, cmd_disable, cmd_config, cmd_scan_os;
#ifdef GRUB_MACHINE_EFI
static grub_command_t cmd_wimboot_efi, cmd_vdisk_boot, cmd_efi_boot_os;
#endif

GRUB_MOD_INIT (glassboot)
{
  cmd_scan = grub_register_command ("gb_scan", grub_cmd_gb_scan, N_("(DEVICE)"),
				    N_("Find ISO files and build the Glassboot menu."));
  cmd_enable = grub_register_command ("gb_hook_enable", grub_cmd_gb_hook_enable,
				      N_("ISO_PATH PART_UUID INITRD"),
				      N_("Add the Glassboot hook to Linux boots."));
  cmd_disable = grub_register_command ("gb_hook_disable", grub_cmd_gb_hook_disable, 0,
				       N_("Stop adding the Glassboot hook."));
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
}

GRUB_MOD_FINI (glassboot)
{
  uninstall_wrappers ();
  grub_procfs_unregister (&menu_entry);
  grub_procfs_unregister (&os_entry);
  grub_unregister_command (cmd_config);
  grub_unregister_command (cmd_scan_os);
  cfg_clear ();
  grub_free (os_text);
  grub_unregister_command (cmd_scan);
  grub_unregister_command (cmd_enable);
  grub_unregister_command (cmd_disable);
#ifdef GRUB_MACHINE_EFI
  grub_unregister_command (cmd_wimboot_efi);
  grub_unregister_command (cmd_vdisk_boot);
  grub_unregister_command (cmd_efi_boot_os);
  os_free_paths ();
#endif
  grub_free (menu_text);
  free_hook_args ();
}
