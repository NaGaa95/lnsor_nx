/* config.h -- global configuration and config file handling
 *
 * LEGO Ninjago: Shadow of Ronin (Android, pkg com.wb.goog.lnjgo, v2.2.1.02,
 * asset version 22102). TT-Fusion engine (libLEGO_Pixel_Mobile.so), ported from
 * the LEGO Batman 3 (lbbg_nx) wrapper of the same engine family.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Memory split (see __libnx_initheap): the loaded .so image gets this fixed
// reserve, the newlib heap (game malloc + mesa host allocations) gets ALL the
// rest of the Horizon heap. libLEGO_Pixel_Mobile.so is ~13 MB (image + .bss);
// 256 MB leaves ample headroom for the image + the nouveau GLSL compiler's host
// allocations.
#define SO_REGION_MB 256

#define SO_NAME     "libLEGO_Pixel_Mobile.so"
#define CONFIG_NAME "config.txt"

// --- Android identity handed to nativeSetDeviceStrings ----------------------
// The engine fingerprints the device tier from these (Kindle/low-end checks);
// non-matching values select the default/normal tier, which is what we want.
#define BUILD_MANUFACTURER "Nintendo"
#define BUILD_MODEL        "Switch"
#define BUILD_DEVICE       "NX"
#define BUILD_PRODUCT      "nx"

// android.os.Build.VERSION.SDK_INT reported through the fake JNI (Lollipop+).
#define ANDROID_SDK_INT 21

// nativeSetCommandLine content; the engine strncpy's it into a 4096B buffer.
// Empty is the safe default (no special command-line switches).
#define ANDROID_COMMAND_LINE ""

// --- on-SD layout -----------------------------------------------------------
// The install directory is resolved at runtime from the .nro's launch path
// (resolve_base_dir() in main.c), not hardcoded; these are the leaf names joined
// under it: <base>/assetpack1..4, <base>/assets/ (extracted tree),
// <base>/assets/.extracted (sentinel), <base>/{save,write,cache}.
#define ASSETPACK_BASENAME "assetpack" // + "1".."4"
#define ASSETS_SUBDIR      "assets"
#define EXTRACT_SENTINEL   ".extracted" // under ASSETS_SUBDIR
#define SAVE_SUBDIR        "save"   // -> nativeSetSavePath
#define WRITE_SUBDIR       "write"  // -> nativeSetWritePath
#define CACHE_SUBDIR       "cache"  // -> nativeSetCachePath

// VFS sentinel handed to nativeAddAssetsDirs: getFileFromAsset() builds
// "<loc>/<rel>" and fopen()s it, and fopen_fake/stat_fake route VFS_PREFIX
// paths through asset_vfs (the extracted assets tree). This is the fopen
// fallback; the primary path is AAssetManager_open (see libc_shim.c). Must not
// collide with a real sdmc path.
#define VFS_LOCATION "vfs:/data"
#define VFS_PREFIX   "vfs:/data/"   // VFS_LOCATION "/"

// Inside the assetpack zips the real assets live at
// "files/assetpacks/<pack>/22102/22102/assets/<rel>"; asset_vfs keys each entry
// by <rel>. The extractor and the tree indexer both split on this marker.
#define ASSET_ZIP_MARKER "/22102/22102/assets/"

// Physical panel size in mm; the engine may use it for DPI-based UI scaling.
// Switch handheld panel is a 6.2" 1280x720 (~237 dpi).
#define SCREEN_PHYS_W_MM 136.7f
#define SCREEN_PHYS_H_MM 76.9f

// actual render/surface size (picked at runtime from docked state)
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int language;        // -1 = follow system; else index into the lang table
  int show_fps;        // 1 = small FPS counter in the top-left corner
  int native_res;      // 1 = render the scene at full window res (default);
                       // 0 = keep the engine's mobile downscale (fallback)
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

// ISO-639 3-letter language ("eng","fre",...) for FusionCallback
// GetCurrentLanguageAsISO3Letter, and the 2-letter region ("US") for
// GetDeviceRegion / GetCurrentLanguageCountryCode.
const char *config_lang_iso3(void);
const char *config_region(void);

#endif
