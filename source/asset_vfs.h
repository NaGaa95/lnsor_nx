/* asset_vfs.h -- read-only virtual filesystem over the extracted assets.
 *
 * The assets ship as four DEFLATE zips (assetpack1..4); asset_vfs.c inflates
 * them once into a flat tree under ASSETS_DIR and serves those real files. The
 * engine reaches an asset two ways, both routed here: AAssetManager_open("<rel>")
 * (the primary path -> AAsset_openFileDescriptor, see libc_shim.c) and
 * fopen("<VFS_LOCATION>/<rel>") (the getFileFromAsset fallback). Each entry names
 * a real uncompressed file (payload base 0, length = file size).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ASSET_VFS_H__
#define __ASSET_VFS_H__

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

// On the first run (the "<assets_dir>/.extracted" sentinel absent), inflate every
// real asset out of packs[0..npacks) into assets_dir/<rel> (with a text progress
// screen), then
// index that tree keyed by each asset's path under assets/. On later runs the
// extraction is skipped and only the index is (re)built. Returns 0 if at least
// one asset was indexed, -1 on total failure.
int asset_vfs_init(const char *assets_dir, const char *const *packs, int npacks);

// Resolve an asset-relative name (e.g. "lego_pixel_mobile_1.fib" or
// "cutscenes/foo.mp4") to its backing file + payload window. Used by
// AAssetManager_open_fake / AAsset_openFileDescriptor_fake. A leading "assets/"
// or VFS_PREFIX on `name` is tolerated. Returns 0 on hit (and fills the
// out-params; container_path points at static storage, off is always 0), -1 on
// miss.
int asset_vfs_lookup(const char *name, const char **container_path,
                     int64_t *off, int64_t *size);

// True if a path is a VFS path (begins VFS_PREFIX or equals VFS_LOCATION).
int asset_vfs_is_vfs(const char *path);

// Size of a VFS asset addressed by a VFS path, or -1 if it is not present.
int64_t asset_vfs_size(const char *path);

// fopen("rb") over the VFS, addressed by a VFS path. Returns an opaque handle
// (cast to FILE*) that the libc_shim stdio shims recognise via
// asset_vfs_is_handle(). NULL if the asset is absent or the mode is not
// read-only.
FILE *asset_vfs_fopen(const char *path, const char *mode);

// True if a FILE* is one of our VFS handles.
int asset_vfs_is_handle(const FILE *f);

// stdio operations on a VFS handle (the libc_shim shims dispatch to these).
size_t asset_vfs_fread(void *ptr, size_t size, size_t n, FILE *f);
int    asset_vfs_fseek(FILE *f, long off, int whence);
long   asset_vfs_ftell(FILE *f);
int    asset_vfs_fgetc(FILE *f);
int    asset_vfs_ungetc(int c, FILE *f);
int    asset_vfs_feof(FILE *f);
int    asset_vfs_ferror(FILE *f);
int    asset_vfs_fileno(FILE *f);
void   asset_vfs_rewind(FILE *f);
int    asset_vfs_fclose(FILE *f);

// For any OpenSL ES fd-streaming path that derives its fd from fileno() on a
// VFS handle (rather than from AAsset_openFileDescriptor): given that fd,
// return the backing real fd plus the payload base offset and length. Returns
// 0 on success, -1 if the fd is not a live VFS handle.
int asset_vfs_resolve_fd(int fd, int *real_fd, int64_t *base_off, int64_t *len);

#endif
