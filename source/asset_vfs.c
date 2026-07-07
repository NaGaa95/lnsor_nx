/* asset_vfs.c -- read-only VFS over the extracted assets.
 *
 * The assets ship as four DEFLATE zips (assetpack1..4); a deflate stream is not
 * seekable and the biggest asset is a ~400 MB .fib the engine streams randomly,
 * so on the first run we inflate every asset once into <ASSETS_DIR>/<rel> and
 * serve those real files thereafter. Each is its own uncompressed file, so every
 * payload window is (base 0, len = file size). The two engine entry points --
 * AAssetManager_open (-> asset_vfs_lookup, see libc_shim.c) and
 * fopen("<VFS_LOCATION>/<rel>") (-> asset_vfs_fopen) -- both route here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>
#include <switch.h>

#include "config.h"
#include "asset_vfs.h"

// ---------------------------------------------------------------------------
// index: one record per real asset file under ASSETS_DIR.
// `key` is the asset path under assets/, e.g. "lego_pixel_mobile_1.fib" or
// "cutscenes/lvl_01_01_chens_island_intro.mp4". `path` is the real SD path we
// open; it doubles as the "container" handed back by asset_vfs_lookup.
// ---------------------------------------------------------------------------

typedef struct {
  char    *key;
  char    *path;
  int64_t  size;
} VfsEntry;

static VfsEntry *s_entries = NULL;
static int       s_entry_count = 0;
static int       s_entry_cap = 0;

static char s_assets_dir[512];

// "<assets_dir>/.extracted": the extraction-complete sentinel (the install
// folder is resolved at runtime, so this is composed rather than a fixed macro).
static void sentinel_path(char *out, size_t n) {
  snprintf(out, n, "%s/%s", s_assets_dir, EXTRACT_SENTINEL);
}

// ---------------------------------------------------------------------------
// open handle (cast to FILE* and handed to the engine). Backed by a real fd
// into the extracted file; base is always 0 (each asset is its own file), kept
// only so the read/seek code reads identically to the Batman port.
// ---------------------------------------------------------------------------

#define VFS_FILE_MAGIC 0x56465332u // 'VFS2'

typedef struct VfsFile {
  uint32_t magic;
  int      fd;
  int64_t  base;   // always 0 here
  int64_t  len;    // file length
  int64_t  pos;    // read cursor
  int      eof;
} VfsFile;

#define MAX_VFS_OPEN 64
static VfsFile *s_open[MAX_VFS_OPEN];
static Mutex    s_lock;

static int reg_add(VfsFile *h) {
  int ok = 0;
  mutexLock(&s_lock);
  for (int i = 0; i < MAX_VFS_OPEN; i++)
    if (!s_open[i]) { s_open[i] = h; ok = 1; break; }
  mutexUnlock(&s_lock);
  return ok;
}

static void reg_del(VfsFile *h) {
  mutexLock(&s_lock);
  for (int i = 0; i < MAX_VFS_OPEN; i++)
    if (s_open[i] == h) { s_open[i] = NULL; break; }
  mutexUnlock(&s_lock);
}

int asset_vfs_is_handle(const FILE *f) {
  if (!f) return 0;
  const VfsFile *h = (const VfsFile *)f;
  // fast lock-free reject: a real newlib FILE* / fake_sF buffer will not carry
  // our magic in its first word. Only pay the locked scan for real candidates.
  if (h->magic != VFS_FILE_MAGIC)
    return 0;
  int found = 0;
  mutexLock(&s_lock);
  for (int i = 0; i < MAX_VFS_OPEN; i++)
    if (s_open[i] == h) { found = 1; break; }
  mutexUnlock(&s_lock);
  return found;
}

// ---------------------------------------------------------------------------
// little-endian readers over a byte buffer
// ---------------------------------------------------------------------------

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------
// index build / lookup
// ---------------------------------------------------------------------------

static void entry_add(const char *key, const char *path, int64_t size) {
  if (s_entry_count >= s_entry_cap) {
    int ncap = s_entry_cap ? s_entry_cap * 2 : 128;
    VfsEntry *n = realloc(s_entries, (size_t)ncap * sizeof(VfsEntry));
    if (!n) return;
    s_entries = n; s_entry_cap = ncap;
  }
  s_entries[s_entry_count].key  = strdup(key);
  s_entries[s_entry_count].path = strdup(path);
  s_entries[s_entry_count].size = size;
  s_entry_count++;
}

// Normalise a requested name to the key form (asset-relative path). Tolerates a
// leading VFS_PREFIX, a leading "assets/", and leading slashes. Writes into out.
static void normalize_name(const char *name, char *out, size_t cap) {
  const char *p = name ? name : "";
  if (strncmp(p, VFS_PREFIX, strlen(VFS_PREFIX)) == 0)
    p += strlen(VFS_PREFIX);
  else if (strcmp(p, VFS_LOCATION) == 0)
    p += strlen(VFS_LOCATION);
  while (*p == '/') p++;
  if (strncmp(p, "assets/", 7) == 0)
    p += 7;
  snprintf(out, cap, "%s", p);
}

static const VfsEntry *entry_find(const char *name) {
  char key[1024];
  normalize_name(name, key, sizeof(key));
  for (int i = 0; i < s_entry_count; i++)
    if (!strcmp(s_entries[i].key, key))
      return &s_entries[i];
  return NULL;
}

// Recursively index a directory tree. The key is the substring after
// ASSET_ZIP_MARKER if the path carries it (a hand-unzipped tree), else the path
// relative to ASSETS_DIR (our flat extraction). Hidden files are skipped.
static void index_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  char child[1024];
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue; // "." ".." and hidden sentinels
    snprintf(child, sizeof(child), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(child, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      index_dir(child);
    } else if (S_ISREG(st.st_mode)) {
      const char *marker = strstr(child, ASSET_ZIP_MARKER);
      const char *key;
      if (marker) {
        key = marker + strlen(ASSET_ZIP_MARKER);
      } else {
        key = child + strlen(s_assets_dir);
        while (*key == '/') key++;
      }
      if (*key && !entry_find(key)) // first hit wins on a clash
        entry_add(key, child, (int64_t)st.st_size);
    }
  }
  closedir(d);
}

// ---------------------------------------------------------------------------
// extraction of the assetpack zips (first run only)
// ---------------------------------------------------------------------------

#define ZIP_EOCD_SIG 0x06054b50u
#define ZIP_CDH_SIG  0x02014b50u
#define ZIP_LFH_SIG  0x04034b50u

// One real asset to inflate out of a pack (every asset is DEFLATE).
typedef struct {
  int      pack;     // index into the caller's packs[]
  int64_t  payload;  // absolute byte offset of the compressed payload
  int64_t  csize;    // compressed length
  int64_t  usize;    // uncompressed length
  char    *rel;      // asset-relative path (dir separators as in the zip)
} ExtractItem;

static ExtractItem *s_items = NULL;
static int s_item_count = 0, s_item_cap = 0;

static void item_add(int pack, int64_t payload, int64_t csize, int64_t usize,
                     const char *rel) {
  if (s_item_count >= s_item_cap) {
    int ncap = s_item_cap ? s_item_cap * 2 : 256;
    ExtractItem *n = realloc(s_items, (size_t)ncap * sizeof(ExtractItem));
    if (!n) return;
    s_items = n; s_item_cap = ncap;
  }
  s_items[s_item_count].pack    = pack;
  s_items[s_item_count].payload = payload;
  s_items[s_item_count].csize   = csize;
  s_items[s_item_count].usize   = usize;
  s_items[s_item_count].rel     = strdup(rel);
  s_item_count++;
}

// Parse one pack's central directory and append every real asset (path carrying
// ASSET_ZIP_MARKER, not a directory entry) to the extract list. Returns 0 on a
// parseable zip, -1 if the file could not be read as one.
static int collect_pack(int pack, const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;

  int64_t fsize = lseek(fd, 0, SEEK_END);
  if (fsize <= 0) { close(fd); return -1; }

  const int64_t scan = fsize < 65557 ? fsize : 65557;
  uint8_t *tail = malloc((size_t)scan);
  if (!tail) { close(fd); return -1; }
  lseek(fd, fsize - scan, SEEK_SET);
  if (read(fd, tail, (size_t)scan) != (ssize_t)scan) { free(tail); close(fd); return -1; }

  int64_t eocd = -1;
  for (int64_t i = scan - 22; i >= 0; i--)
    if (rd32(tail + i) == ZIP_EOCD_SIG) { eocd = i; break; }
  if (eocd < 0) { free(tail); close(fd); return -1; }

  const uint32_t cd_size = rd32(tail + eocd + 12);
  const uint32_t cd_off  = rd32(tail + eocd + 16);
  free(tail);

  uint8_t *cd = malloc(cd_size);
  if (!cd) { close(fd); return -1; }
  lseek(fd, cd_off, SEEK_SET);
  if (read(fd, cd, cd_size) != (ssize_t)cd_size) { free(cd); close(fd); return -1; }

  uint32_t p = 0;
  while (p + 46 <= cd_size && rd32(cd + p) == ZIP_CDH_SIG) {
    const uint32_t csize    = rd32(cd + p + 20);
    const uint32_t usize    = rd32(cd + p + 24);
    const uint16_t name_len = rd16(cd + p + 28);
    const uint16_t extra_len= rd16(cd + p + 30);
    const uint16_t cmt_len  = rd16(cd + p + 32);
    const uint32_t lho      = rd32(cd + p + 42);
    const char    *name     = (const char *)(cd + p + 46);

    if (name_len > 0 && (uint32_t)(p + 46 + name_len) <= cd_size) {
      char nbuf[1024];
      int nl = name_len < (int)sizeof(nbuf) - 1 ? name_len : (int)sizeof(nbuf) - 1;
      memcpy(nbuf, name, nl); nbuf[nl] = 0;

      const char *marker = strstr(nbuf, ASSET_ZIP_MARKER);
      if (marker) {
        const char *rel = marker + strlen(ASSET_ZIP_MARKER);
        const size_t rlen = strlen(rel);
        if (*rel && rel[rlen - 1] != '/') { // skip directory entries
          uint8_t lfh[30];
          if (lseek(fd, lho, SEEK_SET) == (off_t)lho &&
              read(fd, lfh, 30) == 30 && rd32(lfh) == ZIP_LFH_SIG) {
            const uint16_t l_name  = rd16(lfh + 26);
            const uint16_t l_extra = rd16(lfh + 28);
            const int64_t payload = (int64_t)lho + 30 + l_name + l_extra;
            item_add(pack, payload, (int64_t)csize, (int64_t)usize, rel);
          }
        }
      }
    }
    p += 46u + name_len + extra_len + cmt_len;
  }

  free(cd);
  close(fd);
  return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
  const uint8_t *p = buf;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) return -1;
    p += w; n -= (size_t)w;
  }
  return 0;
}

// mkdir every component of a directory path (ignoring EEXIST).
static void mkdirs(const char *dirpath) {
  char tmp[1024];
  size_t n = strnlen(dirpath, sizeof(tmp) - 1);
  memcpy(tmp, dirpath, n); tmp[n] = 0;
  for (char *s = tmp + 1; *s; s++) {
    if (*s == '/') { *s = 0; mkdir(tmp, 0777); *s = '/'; }
  }
  mkdir(tmp, 0777);
}

// Raw-inflate one payload from an open pack fd into outpath. Streams in fixed
// buffers so a 400 MB asset never lands in RAM. Returns 0 on success.
#define IO_CHUNK (256 * 1024)

static int extract_one(int packfd, const ExtractItem *it, const char *outpath) {
  if (lseek(packfd, it->payload, SEEK_SET) != it->payload) return -1;
  int ofd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (ofd < 0) return -1;

  unsigned char *inb = malloc(IO_CHUNK);
  unsigned char *outb = malloc(IO_CHUNK);
  int rc = -1;
  int64_t written = 0;
  if (!inb || !outb) goto done;

  z_stream zs;
  memset(&zs, 0, sizeof zs);
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) goto done; // raw deflate (no header)
  int64_t rem = it->csize;
  for (;;) {
    if (zs.avail_in == 0) {
      if (rem <= 0) { inflateEnd(&zs); goto done; } // truncated stream
      size_t want = rem < IO_CHUNK ? (size_t)rem : IO_CHUNK;
      ssize_t got = read(packfd, inb, want);
      if (got <= 0) { inflateEnd(&zs); goto done; }
      zs.next_in = inb; zs.avail_in = (uInt)got; rem -= got;
    }
    zs.next_out = outb; zs.avail_out = IO_CHUNK;
    int zret = inflate(&zs, Z_NO_FLUSH);
    // Z_BUF_ERROR just means "needs a refill"; the loop top supplies more input.
    if (zret != Z_OK && zret != Z_STREAM_END && zret != Z_BUF_ERROR) {
      inflateEnd(&zs); goto done;
    }
    size_t produced = IO_CHUNK - zs.avail_out;
    if (produced && write_all(ofd, outb, produced) < 0) { inflateEnd(&zs); goto done; }
    written += (int64_t)produced;
    if (zret == Z_STREAM_END) break;
  }
  inflateEnd(&zs);
  rc = (written == it->usize) ? 0 : -1;

done:
  free(inb); free(outb);
  close(ofd);
  if (rc != 0) unlink(outpath); // leave no partial file behind
  return rc;
}

// Extract all packs into ASSETS_DIR (skipping already-complete files, so an
// interrupted run resumes), with a text progress screen. Returns 0 on success,
// -1 if extraction was attempted and failed (fatal), -2 if the packs are absent
// (nothing extracted -- the caller may still index a hand-extracted tree).
static int extract_all(const char *const *packs, int npacks) {
  struct stat pst;
  for (int i = 0; i < npacks; i++)
    if (stat(packs[i], &pst) != 0)
      return -2;

  s_item_count = 0;
  int64_t total = 0;
  for (int i = 0; i < npacks; i++) {
    if (collect_pack(i, packs[i]) != 0)
      return -1; // a pack is present but unreadable/corrupt
  }
  for (int i = 0; i < s_item_count; i++)
    total += s_items[i].usize;
  if (s_item_count == 0)
    return -1;

  consoleInit(NULL);
  printf("LEGO Ninjago: Shadow of Ronin\n");
  printf("First-run asset extraction (one-time).\n");
  printf("Inflating %d files, ~%lld MB. Please wait...\n\n",
         s_item_count, (long long)(total / (1024 * 1024)));
  consoleUpdate(NULL);

  int cur_pack = -1, packfd = -1;
  int64_t done_bytes = 0;
  int rc = 0;
  char outpath[1024], dirpath[1024];

  for (int i = 0; i < s_item_count; i++) {
    ExtractItem *it = &s_items[i];
    snprintf(outpath, sizeof(outpath), "%s/%s", s_assets_dir, it->rel);

    // resume: skip files already fully extracted
    struct stat st;
    if (stat(outpath, &st) == 0 && st.st_size == it->usize) {
      done_bytes += it->usize;
      continue;
    }

    // ensure parent dir exists
    snprintf(dirpath, sizeof(dirpath), "%s", outpath);
    char *slash = strrchr(dirpath, '/');
    if (slash) { *slash = 0; mkdirs(dirpath); }

    if (it->pack != cur_pack) {
      if (packfd >= 0) close(packfd);
      packfd = open(packs[it->pack], O_RDONLY);
      cur_pack = it->pack;
      if (packfd < 0) { rc = -1; break; }
    }

    printf("\r%3lld%%  %-40.40s", (long long)(total ? done_bytes * 100 / total : 100), it->rel);
    consoleUpdate(NULL);

    if (extract_one(packfd, it, outpath) != 0) { rc = -1; break; }
    done_bytes += it->usize;
  }
  if (packfd >= 0) close(packfd);

  if (rc == 0) {
    printf("\r100%%  done.                                        \n");
    // write the sentinel so we never re-scan the packs
    char sent[600];
    sentinel_path(sent, sizeof(sent));
    int sfd = open(sent, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (sfd >= 0) { (void)!write(sfd, "ok\n", 3); close(sfd); }
    consoleUpdate(NULL);
    svcSleepThread(400000000ull); // ~0.4 s so the "done" line is visible
  } else {
    printf("\n\nExtraction FAILED (SD full, or a bad assetpack).\n");
    consoleUpdate(NULL);
    svcSleepThread(1500000000ull);
  }

  consoleExit(NULL);

  for (int i = 0; i < s_item_count; i++) free(s_items[i].rel);
  free(s_items); s_items = NULL; s_item_count = s_item_cap = 0;
  return rc;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

int asset_vfs_init(const char *assets_dir, const char *const *packs, int npacks) {
  mutexInit(&s_lock);
  snprintf(s_assets_dir, sizeof(s_assets_dir), "%s", assets_dir);

  // extract on first run (sentinel absent).
  char sent[600];
  sentinel_path(sent, sizeof(sent));
  struct stat st;
  if (stat(sent, &st) != 0) {
    mkdir(assets_dir, 0777);
    int r = extract_all(packs, npacks);
    if (r == -1)
      return -1; // extraction was attempted but failed (e.g. SD full) -> fatal;
                 // do NOT index a partial tree and pretend it worked.
    // r == -2 (packs absent) falls through: the user may have hand-extracted the
    // assets already, which the index below will pick up.
  }

  index_dir(assets_dir);
  return s_entry_count > 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// name resolution (AAssetManager_open / AAsset_openFileDescriptor)
// ---------------------------------------------------------------------------

int asset_vfs_lookup(const char *name, const char **container_path,
                     int64_t *off, int64_t *size) {
  const VfsEntry *e = entry_find(name);
  if (!e) return -1;
  if (container_path) *container_path = e->path; // real file; open at offset 0
  if (off)  *off  = 0;
  if (size) *size = e->size;
  return 0;
}

// ---------------------------------------------------------------------------
// path classification / lookup (fopen seam)
// ---------------------------------------------------------------------------

int asset_vfs_is_vfs(const char *path) {
  if (!path) return 0;
  return strncmp(path, VFS_PREFIX, strlen(VFS_PREFIX)) == 0 ||
         strcmp(path, VFS_LOCATION) == 0;
}

int64_t asset_vfs_size(const char *path) {
  const VfsEntry *e = entry_find(path);
  return e ? e->size : -1;
}

// ---------------------------------------------------------------------------
// open / read  (identical semantics to the Batman port; base is 0)
// ---------------------------------------------------------------------------

static int mode_is_read_only(const char *mode) {
  if (!mode) return 0;
  if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))
    return 0;
  return strchr(mode, 'r') != NULL;
}

static VfsFile *make_handle(const VfsEntry *e) {
  int fd = open(e->path, O_RDONLY);
  if (fd < 0) return NULL;

  VfsFile *h = calloc(1, sizeof(*h));
  if (!h) { close(fd); return NULL; }
  h->magic = VFS_FILE_MAGIC;
  h->fd = fd;
  h->base = 0;
  h->len = e->size;
  h->pos = 0;
  h->eof = 0;
  if (!reg_add(h)) { close(fd); free(h); return NULL; }
  return h;
}

FILE *asset_vfs_fopen(const char *path, const char *mode) {
  if (!mode_is_read_only(mode))
    return NULL;
  const VfsEntry *e = entry_find(path);
  if (!e)
    return NULL;
  return (FILE *)make_handle(e);
}

size_t asset_vfs_fread(void *ptr, size_t size, size_t n, FILE *f) {
  VfsFile *h = (VfsFile *)f;
  if (size == 0 || n == 0) return 0;
  int64_t want = (int64_t)size * (int64_t)n;
  int64_t avail = h->len - h->pos;
  if (avail <= 0) { h->eof = 1; return 0; }
  if (want > avail) want = avail;
  int64_t whole = (want / (int64_t)size) * (int64_t)size;
  ssize_t got = read(h->fd, ptr, (size_t)whole);
  if (got < 0) return 0;
  h->pos += got;
  if (h->pos >= h->len) h->eof = 1;
  return (size_t)(got / (ssize_t)size);
}

int asset_vfs_fseek(FILE *f, long off, int whence) {
  VfsFile *h = (VfsFile *)f;
  int64_t np;
  switch (whence) {
    case SEEK_SET: np = off; break;
    case SEEK_CUR: np = h->pos + off; break;
    case SEEK_END: np = h->len + off; break;
    default: return -1;
  }
  if (np < 0) np = 0;
  if (np > h->len) np = h->len;
  if (lseek(h->fd, h->base + np, SEEK_SET) != h->base + np)
    return -1;
  h->pos = np;
  h->eof = 0;
  return 0;
}

long asset_vfs_ftell(FILE *f) {
  return (long)((VfsFile *)f)->pos;
}

int asset_vfs_fgetc(FILE *f) {
  unsigned char c;
  if (asset_vfs_fread(&c, 1, 1, f) != 1)
    return -1; // EOF
  return (int)c;
}

int asset_vfs_ungetc(int c, FILE *f) {
  VfsFile *h = (VfsFile *)f;
  if (c == -1 || h->pos <= 0) return -1;
  if (asset_vfs_fseek(f, -1, SEEK_CUR) != 0) return -1;
  return c;
}

int asset_vfs_feof(FILE *f) {
  return ((VfsFile *)f)->eof;
}

int asset_vfs_ferror(FILE *f) {
  (void)f;
  return 0;
}

int asset_vfs_fileno(FILE *f) {
  return ((VfsFile *)f)->fd;
}

void asset_vfs_rewind(FILE *f) {
  asset_vfs_fseek(f, 0, SEEK_SET);
  ((VfsFile *)f)->eof = 0;
}

int asset_vfs_fclose(FILE *f) {
  VfsFile *h = (VfsFile *)f;
  reg_del(h);
  if (h->fd >= 0) close(h->fd);
  h->magic = 0;
  free(h);
  return 0;
}

int asset_vfs_resolve_fd(int fd, int *real_fd, int64_t *base_off, int64_t *len) {
  int ret = -1;
  mutexLock(&s_lock);
  for (int i = 0; i < MAX_VFS_OPEN; i++) {
    VfsFile *h = s_open[i];
    if (h && h->magic == VFS_FILE_MAGIC && h->fd == fd) {
      if (real_fd)  *real_fd = h->fd;
      if (base_off) *base_off = h->base; // 0
      if (len)      *len = h->len;
      ret = 0;
      break;
    }
  }
  mutexUnlock(&s_lock);
  return ret;
}
