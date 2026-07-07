/* game.c -- engine-level setup for libLEGO_Pixel_Mobile.so (TT Fusion).
 *
 * patch_game() installs the pthread role hints and the native-resolution render
 * hook before so_finalize maps the image executable. Controllers and cutscenes
 * need no engine-internal hooks: input uses the native controller ABI (from
 * main.c) and cutscenes flow through the JNI startMoviePlayback path.
 *
 * The two offsets below are from libLEGO_Pixel_Mobile.so's exported symbols
 * (_ZTV23UI_LoadingScreen_Module, _Z23fnaTexture_CreateRenderjjjjjPKc); the
 * vtable slot layout and CreateRender prologue match Batman 3, so cr_tramp.s and
 * the slot math are reused unchanged.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <switch.h> // armGetSystemTick

#include "../so_util.h"
#include "../hooks.h"
#include "../pthr.h"
#include "../config.h" // screen_width / screen_height

extern so_module game_mod; // defined in main.c

// --- loading-screen detection ----------------------------------------------
// The CPU boost (ApmCpuBoostMode_FastLoad downclocks the GPU) must run on loading
// screens only, never in gameplay. The loading screen is a UI_LoadingScreen_Module,
// but it is not the engine's "current module" (another module hosts it), so it
// cannot be found by walking the module-state chain. Instead we hook its vtable
// (@_ZTV23UI_LoadingScreen_Module 0x5eea20): Module_Update [+0x30] runs every frame
// it is on screen, Module_Exit [+0x40] when it leaves. The wrappers stamp/clear
// g_ls_tick and call the originals; main.c boosts while the stamp is fresh.
#define OFF_LS_VTABLE 0x5eea20

volatile uint64_t g_ls_tick; // last tick the loading screen updated; 0 = not loading

typedef void *(*ls_voidfn)(void *self);
typedef void *(*ls_updatefn)(void *self, float dt);
static ls_voidfn   s_ls_exit_orig;
static ls_updatefn s_ls_update_orig;

static void *my_ls_update(void *self, float dt) {
  g_ls_tick = armGetSystemTick();
  return s_ls_update_orig(self, dt);
}
static void *my_ls_exit(void *self) {
  g_ls_tick = 0;
  return s_ls_exit_orig(self);
}

// --- native-resolution rendering -------------------------------------------
// The engine sizes its scene render targets to window*0.75 (a hardcoded mobile
// downscale: 1280x720->960x540, 1920x1080->1440x810) via fnaTexture_CreateRender
// @0x3f3b48 (args w0=width, w1=height). The scale is computed deep in a
// vtable-dispatched render-init with no clean global, so we detour CreateRender
// and bump the scene-sized targets back to the full window resolution (native
// 720p handheld / 1080p docked). The projection is aspect-based (both 16:9), so
// geometry stays correct; egl.c remaps the matching scene viewport/scissor.
#define OFF_CREATERENDER 0x3f3b48

uint64_t g_cr_continue; // runtime addr of fnaTexture_CreateRender + 0x10

// Trampoline (cr_tramp.s): re-runs the clobbered prologue, then jumps to +0x10.
extern void *cr_tramp(unsigned w, unsigned h, unsigned c, unsigned d, unsigned t);

static void *my_CreateRender(unsigned w, unsigned h, unsigned c, unsigned d, unsigned t) {
  if (config.native_res && screen_width > 0 && (int)w == screen_width * 3 / 4 &&
      (int)h == screen_height * 3 / 4) {
    w = (unsigned)screen_width;   // window*0.75 scene target -> full window res
    h = (unsigned)screen_height;
  }
  return cr_tramp(w, h, c, d, t);
}

void patch_game(void) {
  pthr_set_role_symbols(0, 0, 0);

  // continuation = runtime (executable) address; hook write = writable mirror.
  // (Only installed when native-res is on; the detour is a no-op otherwise, but
  // skipping it entirely keeps native_res=0 a clean "stock engine" fallback.)
  if (config.native_res) {
    g_cr_continue = (uint64_t)game_mod.load_virtbase + OFF_CREATERENDER + 0x10;
    hook_arm64((uintptr_t)game_mod.load_base + OFF_CREATERENDER, (uintptr_t)&my_CreateRender);
  }

  // Loading-screen vtable hook (writes via the writable mirror; reflects at
  // load_virtbase since the whole image is aliased). The slots already hold
  // runtime pointers after so_relocate, so the wrappers can call them directly.
  uintptr_t vt = (uintptr_t)game_mod.load_base + OFF_LS_VTABLE;
  s_ls_update_orig = *(ls_updatefn *)(vt + 0x30);
  s_ls_exit_orig   = *(ls_voidfn *)(vt + 0x40);
  *(void **)(vt + 0x30) = (void *)&my_ls_update;
  *(void **)(vt + 0x40) = (void *)&my_ls_exit;
}
