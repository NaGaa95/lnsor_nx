/* game.c -- engine-level setup for libLEGO_Pixel_Mobile.so (TT Fusion).
 *
 * patch_game() installs the pthread role hints and the native-resolution render
 * hook before so_finalize maps the image executable. Controllers and cutscenes
 * need no engine-internal hooks: input uses the native controller ABI (from
 * main.c) and cutscenes flow through the JNI startMoviePlayback path.
 *
 * The loading-screen and rendering offsets are from exported symbols in
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
#include "../audio.h"

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

// --- HintStone UI -----------------------------------------------------------
// Module_Init runs on a Fusion worker. Its final HUDHintStone::Show call starts
// an animation and immediately polls controls, both owned by the render path.
// Running that call on the worker can deadlock after touch input, so the hook
// performs the first two original calls there and defers only Show to the GL
// thread. The original body is:
//   Hud_ShowHudItems(false); HUDHintStone::Init(); HUDHintStone::Show();
#define OFF_HINTSTONE_VTABLE 0x5eb5c8

typedef void (*hud_show_items_fn)(int show);
typedef void (*hud_hint_fn)(void);

static hud_show_items_fn s_hud_show_items;
static hud_hint_fn s_hud_hint_init;
static hud_hint_fn s_hud_hint_show;
static volatile int s_hint_show_pending;

static void my_hint_init(void *self) {
  (void)self;
  s_hud_show_items(0);
  s_hud_hint_init();
  __atomic_store_n(&s_hint_show_pending, 1, __ATOMIC_RELEASE);
}

void hintstone_service_render_thread(void) {
  if (__atomic_exchange_n(&s_hint_show_pending, 0, __ATOMIC_ACQ_REL))
    s_hud_hint_show();
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
  if (screen_width > 0 && (int)w == screen_width * 3 / 4 &&
      (int)h == screen_height * 3 / 4) {
    w = (unsigned)screen_width;   // window*0.75 scene target -> full window res
    h = (unsigned)screen_height;
  }
  return cr_tramp(w, h, c, d, t);
}

void patch_game(void) {
  pthr_set_role_symbols(0, 0, 0);

  // continuation = runtime (executable) address; hook write = writable mirror.
  g_cr_continue = (uint64_t)game_mod.load_virtbase + OFF_CREATERENDER + 0x10;
  hook_arm64((uintptr_t)game_mod.load_base + OFF_CREATERENDER, (uintptr_t)&my_CreateRender);

  // Loading-screen vtable hook (writes via the writable mirror; reflects at
  // load_virtbase since the whole image is aliased). The slots already hold
  // runtime pointers after so_relocate, so the wrappers can call them directly.
  uintptr_t vt = (uintptr_t)game_mod.load_base + OFF_LS_VTABLE;
  s_ls_update_orig = *(ls_updatefn *)(vt + 0x30);
  s_ls_exit_orig   = *(ls_voidfn *)(vt + 0x40);
  *(void **)(vt + 0x30) = (void *)&my_ls_update;
  *(void **)(vt + 0x40) = (void *)&my_ls_exit;

  s_hud_show_items = (hud_show_items_fn)so_try_find_addr_rx(
      &game_mod, "_Z16Hud_ShowHudItemsb");
  s_hud_hint_init = (hud_hint_fn)so_try_find_addr_rx(
      &game_mod, "_ZN12HUDHintStone4InitEv");
  s_hud_hint_show = (hud_hint_fn)so_try_find_addr_rx(
      &game_mod, "_ZN12HUDHintStone4ShowEv");

  // Only replace Module_Init when every function needed to reproduce its body
  // was resolved. Otherwise the original game implementation remains intact.
  if (s_hud_show_items && s_hud_hint_init && s_hud_hint_show) {
    uintptr_t hvt = (uintptr_t)game_mod.load_base + OFF_HINTSTONE_VTABLE;
    *(void **)(hvt + 0x30) = (void *)&my_hint_init;
  }

  // Install geMusic/geSound_SetGlobalVolume hooks to capture the user's true
  // volume intent. These are called from patch_game (before so_finalize) so the
  // writable mirror is still active.
  patch_audio();
}
