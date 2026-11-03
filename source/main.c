/* main.c
 *
 * LEGO Ninjago: Shadow of Ronin (Android, arm64) on the Switch.
 *
 * libLEGO_Pixel_Mobile.so is a TT-Fusion engine build (same family as LEGO
 * Batman 3, which this wrapper was ported from). Fusion's render path is
 * GLSurfaceView-driven: the native lib only renders a frame on demand
 * (nativeRender). So this wrapper plays the GLSurfaceView GL thread: it brings
 * up EGL, runs nativeInit/nativeColdBoot/nativeResize, then loops nativeRender +
 * eglSwapBuffers on a dedicated thread while the main thread nurses the applet.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "saltynx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "jni_fake.h"
#include "pthr.h"
#include "asset_vfs.h"
#include "fmv.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module game_mod; // libLEGO_Pixel_Mobile.so

// fake JNI handles (must stay live for the whole session: nativeInit stores the
// activity globally, nativeRender re-caches env+class every frame)
static void *fz_class = NULL;      // jclass passed to every native
static void *fake_activity = NULL; // Activity jobject for nativeInit
static void *fake_eglcfg = NULL;   // EGLConfig jobject for nativeInit
#define FAKE_AM ((void *)0x41535345) // non-null AssetManager token (dead for I/O)

extern int g_fake_eglconfig_id; // jni_fake.c; the EGL_CONFIG_ID nativeInit reads

// Set (to a small frame countdown) by jni_fake when the engine asks the Java
// side to start a cutscene .mp4. We have no video player, so the render thread
// answers with nativeUpdateMovieInfo(playing=0) so the engine treats the movie
// as ended and continues to gameplay instead of waiting on a black surface.
volatile int g_movie_skip_request = 0;

// Set (to a small frame countdown) by jni_fake when the engine shows a modal
// ShowAlertDialog. On Android the choice returns async via
// nativeSetAlertDialogResponse; with no dialog UI the render thread auto-answers
// after a frame. DIALOG_AUTO_RESPONSE is the button index: 1 is the affirmative
// "Continue" (0 was observed to CANCEL the New Game prompt).
volatile int g_dialog_response_request = 0;
#define DIALOG_AUTO_RESPONSE 1

// fmv_audio_override (the OpenSL cutscene-audio hook sl_switch.c calls) and the
// g_fmv_* request handshake are now provided by fmv.c -- the wrapper plays the
// engine's .mp4 cutscenes with ffmpeg. See gl_thread_main's frame loop.

// separate the newlib heap from the .so load region (verbatim from the Batman 2 port)
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t so_reserve = (size_t)SO_REGION_MB * 1024 * 1024;
  fake_heap_size = (size > so_reserve + so_reserve / 2) ? (size - so_reserve)
                                                        : (size / 2);

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

// ---------------------------------------------------------------------------
// runtime install-directory resolution (the folder is not hardcoded): take the
// directory of argv[0] (the launched .nro), else the cwd, stripping a leading
// "sdmc:"-style device prefix. All paths are built under it.
// ---------------------------------------------------------------------------

static char g_base[256];        // install directory, resolved at startup
static char g_so_path[320];     // <base>/libLEGO_Pixel_Mobile.so
static char g_config_path[320]; // <base>/config.txt
static char g_save_path[320];   // <base>/save
static char g_write_path[320];  // <base>/write
static char g_cache_path[320];  // <base>/cache
static char g_assets_dir[320];  // <base>/assets
static char g_pack_path[4][320];// <base>/assetpack1..4

static void resolve_base_dir(int argc, char **argv) {
  char tmp[256] = {0};
  int have = 0;

  if (argc > 0 && argv && argv[0] && strchr(argv[0], '/')) {
    snprintf(tmp, sizeof(tmp), "%s", argv[0]);
    char *slash = strrchr(tmp, '/');
    if (slash) { *slash = '\0'; have = (tmp[0] != '\0'); }
  }
  if (!have && getcwd(tmp, sizeof(tmp)))
    have = (tmp[0] != '\0');

  char *p = tmp;
  char *colon = strchr(tmp, ':');
  if (colon && (colon - tmp) <= 6) // strip "sdmc:" etc. -> "/switch/..."
    p = colon + 1;

  snprintf(g_base, sizeof(g_base), "%s", (have && *p) ? p : ".");

  snprintf(g_so_path,     sizeof(g_so_path),     "%s/%s", g_base, SO_NAME);
  snprintf(g_config_path, sizeof(g_config_path), "%s/%s", g_base, CONFIG_NAME);
  snprintf(g_save_path,   sizeof(g_save_path),   "%s/%s", g_base, SAVE_SUBDIR);
  snprintf(g_write_path,  sizeof(g_write_path),  "%s/%s", g_base, WRITE_SUBDIR);
  snprintf(g_cache_path,  sizeof(g_cache_path),  "%s/%s", g_base, CACHE_SUBDIR);
  snprintf(g_assets_dir,  sizeof(g_assets_dir),  "%s/%s", g_base, ASSETS_SUBDIR);
  for (int i = 0; i < 4; i++)
    snprintf(g_pack_path[i], sizeof(g_pack_path[i]), "%s/%s%d", g_base, ASSETPACK_BASENAME, i + 1);
}

static void check_data(void) {
  struct stat st;
  if (stat(g_so_path, &st) < 0)
    fatal_error("Could not find\n%s.\nCheck your files in\n%s/.", SO_NAME, g_base);
  // The assets are validated by asset_vfs_init (which extracts the assetpacks on
  // first run, or indexes an already-extracted tree, and fails with a clear
  // message if neither is present) -- so nothing to check for them here.
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// Fusion JNI entry points (resolved from libLEGO_Pixel_Mobile.so by name)
// ---------------------------------------------------------------------------

static struct {
  void (*setCommandLine)(void *, void *, void *jstr);
  void (*setDeviceStrings)(void *, void *, void *, void *, void *, void *);
  void (*setWritePath)(void *, void *, void *jstr);
  void (*setSavePath)(void *, void *, void *jstr);
  void (*setCachePath)(void *, void *, void *jstr);
  void (*setAudioOutputBufferSize)(void *, void *, int frames);
  void (*addAssetsDirs)(void *, void *, void *jstrArray);
  void (*initAssetManager)(void *, void *, void *jobj);
  void (*init)(void *, void *, void *eglConfig, void *activity);
  void (*coldBoot)(void *, void *);
  void (*resize)(void *, void *, int w, int h);
  void (*render)(void *, void *);
  void (*windowFocusChanged)(void *, void *, int focused);
  void (*resume)(void *, void *);
  void (*pause)(void *, void *);
  void (*updateMovieInfo)(void *, void *, int playing, int pos);
  // delivers the user's alert-dialog choice back into the engine (button index;
  // 0 = first/positive button). The engine blocks waiting for this after a
  // ShowAlertDialog (e.g. the "no cloud save -> New Game?" prompt).
  void (*setAlertDialogResponse)(void *, void *, int response);
  unsigned char (*backButton)(void *, void *);
  // Android touch JNI ABI. Integer arguments use the general-purpose register
  // stream while x/y/pressure use the floating-point stream on AArch64, which
  // is exactly what these C declarations generate.
  void (*touchGestureStart)(void *, void *, int pointer, float x, float y,
                            float pressure);
  void (*touchGestureEnd)(void *, void *, int pointer, float x, float y,
                          float pressure);
  void (*touchDown)(void *, void *, int pointer, float x, float y,
                    float pressure);
  void (*touchUp)(void *, void *, int pointer, float x, float y,
                  float pressure);
  void (*touchMove)(void *, void *, int pointer, float x, float y,
                    float pressure);
  void (*touchCancelAll)(void *, void *);
  // engine native controller ABI (not a JNI entry): set per-frame pad state.
  // signature: (int connected, int buttonMask, float stickX, float stickY)
  void (*controllerSetData)(int, int, float, float);
} fz;

static int (*JNI_OnLoad)(void *vm, void *reserved);

#define RES(field, sym) \
  fz.field = (void *)so_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_" sym)

static void resolve_fusion_exports(void) {
  RES(setCommandLine,       "Fusion_nativeSetCommandLine");
  RES(setDeviceStrings,     "Fusion_nativeSetDeviceStrings");
  RES(setWritePath,         "Fusion_nativeSetWritePath");
  RES(setSavePath,          "Fusion_nativeSetSavePath");
  RES(setCachePath,         "Fusion_nativeSetCachePath");
  RES(setAudioOutputBufferSize, "Fusion_nativeSetAudioOutputBufferSize");
  RES(addAssetsDirs,        "Fusion_addAssetsDirs");
  RES(initAssetManager,     "Fusion_nativeInitializeAssetManager");
  RES(init,                 "GameGLSurfaceView_nativeInit");
  RES(coldBoot,             "GameGLSurfaceView_nativeColdBoot");
  RES(resize,               "GameGLSurfaceView_nativeResize");
  RES(render,               "GameGLSurfaceView_nativeRender");
  RES(windowFocusChanged,   "GameGLSurfaceView_nativeWindowFocusChanged");
  RES(resume,               "GameGLSurfaceView_nativeResume");
  RES(pause,                "GameGLSurfaceView_nativePause");
  RES(updateMovieInfo,      "GameGLSurfaceView_nativeUpdateMovieInfo");
  RES(setAlertDialogResponse, "Fusion_nativeSetAlertDialogResponse");
  RES(backButton,           "Fusion_nativeBackButtonPressed");
  RES(touchGestureStart,    "Fusion_nativeTouchEventGestureStart");
  RES(touchGestureEnd,      "Fusion_nativeTouchEventGestureEnd");
  RES(touchDown,            "Fusion_nativeTouchEventDown");
  RES(touchUp,              "Fusion_nativeTouchEventUp");
  RES(touchMove,            "Fusion_nativeTouchEventMove");
  RES(touchCancelAll,       "Fusion_nativeTouchEventCancelAll");

  // The native controller entry the Java GameController would tail-call; we call
  // it directly to push the Switch pad each frame (mangled C++ symbol:
  // fnaController_AndroidNativeControllerSetData(int,int,float,float)).
  fz.controllerSetData = (void *)so_try_find_addr_rx(
      &game_mod, "_Z44fnaController_AndroidNativeControllerSetDataiiff");

  // libLEGO_Pixel_Mobile.so registers its JNI methods by name and has NO
  // JNI_OnLoad (that lives in the DexProtector libstub.so we don't load). Use
  // the non-aborting lookup; the call is guarded in run_setup_phase.
  JNI_OnLoad = (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
}

// ---------------------------------------------------------------------------
// setup phase (main thread, no GL): the calls GameActivity.onCreate would make
// ---------------------------------------------------------------------------

static void run_setup_phase(void) {
  if (JNI_OnLoad)
    JNI_OnLoad(fake_vm, NULL);

  fz.setCommandLine(fake_env, fz_class, jni_make_string(ANDROID_COMMAND_LINE));
  fz.setDeviceStrings(fake_env, fz_class,
      jni_make_string(BUILD_MANUFACTURER), jni_make_string(BUILD_MODEL),
      jni_make_string(BUILD_DEVICE),       jni_make_string(BUILD_PRODUCT));
  fz.setWritePath(fake_env, fz_class, jni_make_string(g_write_path));
  fz.setSavePath (fake_env, fz_class, jni_make_string(g_save_path));
  fz.setCachePath(fake_env, fz_class, jni_make_string(g_cache_path));

  // NOTE: calling nativeSetAudioOutputBufferSize here regressed in-game audio on
  // the Batman 2 port (the engine left its output mix NULL after level load).
  // Leaving it OUT; in-game music works without it.

  // Register one asset directory: the VFS sentinel. addAssetsDirs iterates the
  // String[] calling fnOBBPackages_AddAssetDir on each; getFileFromAsset then
  // fopen()s "<dir>/<rel>" as the fallback. The primary path is
  // AAssetManager_open(<rel>) -> AAsset_openFileDescriptor (see libc_shim.c).
  const char *dirs[] = { VFS_LOCATION };
  fz.addAssetsDirs(fake_env, fz_class, jni_make_string_array(1, dirs));
  fz.initAssetManager(fake_env, fz_class, FAKE_AM);
}

// ---------------------------------------------------------------------------
// GL render thread: owns the EGL context for the whole session
// ---------------------------------------------------------------------------

static pthread_t gl_thread;
static volatile int gl_thread_ready = 0;
static volatile int gl_should_quit = 0;

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;

// Present hook used by fmv.c to flip each decoded cutscene frame to the window.
void wrapper_fmv_present(void) { eglSwapBuffersHook(s_dpy, s_surf); }

static void update_input(void); // pumped from the render thread (see frame loop)

// True while the engine's loading screen is on screen (boot scene load + level
// loads), false in menus/gameplay -- gates the CPU boost so FastLoad never
// downclocks the GPU mid-game. Driven by g_ls_tick, stamped every frame by the
// UI_LoadingScreen_Module::Module_Update vtable hook (hooks/game.c) and cleared on
// Module_Exit; the freshness window rides brief load hitches.
extern volatile uint64_t g_ls_tick;
static int loading_screen_active(void) {
  const uint64_t t = g_ls_tick;
  if (!t) return 0;
  return (armGetSystemTick() - t) < (armGetSystemTickFreq() / 3); // ~333 ms
}

static void *gl_thread_main(void *arg) {
  (void)arg;
  pthr_ensure_fake_tls();
  pthr_mark_render_thread();
  // dedicate a CPU core to the render thread so it runs in parallel with the
  // engine's worker threads (texture loader, etc.) rather than serializing onto
  // one core and deadlocking on the engine's render-device critical section.
  pthr_pin_render_core();

  // 1. EGL bring-up via the wrapper hooks (mimics the Android GLSurfaceView GL
  //    thread; the engine imports none of eglGetDisplay/Initialize/Window/Swap)
  s_dpy = eglGetDisplayHook(EGL_DEFAULT_DISPLAY);
  eglInitializeHook(s_dpy, NULL, NULL);
  eglBindAPIHook(EGL_OPENGL_ES_API);

  const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE,
  };
  EGLConfig cfg = NULL;
  EGLint ncfg = 0;
  eglChooseConfigHook(s_dpy, cfg_attr, &cfg, 1, &ncfg);
  eglGetConfigAttrib(s_dpy, cfg, EGL_CONFIG_ID, &g_fake_eglconfig_id);

  NWindow *nw = nwindowGetDefault();
  nwindowSetDimensions(nw, screen_width, screen_height);
  s_surf = eglCreateWindowSurfaceHook(s_dpy, cfg, (EGLNativeWindowType)nw, NULL);

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_ctx = eglCreateContextHook(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);

  eglMakeCurrentHook(s_dpy, s_surf, s_surf, s_ctx);
  // force the real bind now (the hooks above only set virtual state); this makes
  // the thread the real context owner so the engine's eglSwapInterval/queries in
  // nativeInit have a current context.
  glViewportHook(0, 0, screen_width, screen_height);
  eglSwapIntervalHook(s_dpy, 1); // 60 fps

  // 2. engine bring-up on this thread (context current)
  fz.init(fake_env, fz_class, fake_eglcfg, fake_activity);
  fz.resize(fake_env, fz_class, screen_width, screen_height);
  fz.coldBoot(fake_env, fz_class);
  fz.windowFocusChanged(fake_env, fz_class, 1);
  fz.resume(fake_env, fz_class);

  gl_thread_ready = 1;

  // Frame loop: the wrapper drives presentation (the engine never calls
  // eglSwapBuffers). Present only frames where the engine actually drew into the
  // window framebuffer (gl_draw_fbo0/fbon counters); otherwise hold the last
  // good frame, which stops the loading/intro flicker-to-black.
  extern volatile int gl_draw_fbo0_count, gl_draw_fbon_count;
  int prev_fbo0 = gl_draw_fbo0_count, prev_fbon = gl_draw_fbon_count;
  while (!gl_should_quit) {
    // Cutscene playback: when startMoviePlayback is called from an engine worker
    // thread, jni_fake posts the .mp4 name here and blocks; we own the GL context,
    // so we decode + present it (fmv_play blocks until it ends/skips), then unblock
    // the engine. (If the JNI call was already on this thread, it played inline.)
    if (g_fmv_request) {
      g_fmv_request = 0;
      fmv_play(g_fmv_name);
      g_fmv_done = 1;
      prev_fbo0 = gl_draw_fbo0_count; prev_fbon = gl_draw_fbon_count;
      continue; // the engine is blocked; skip a game render this frame
    }

    // Cutscene skip fallback: after a movie (or for movies with no file) tell the
    // engine the movie is not playing for a few frames so it advances to gameplay.
    if (g_movie_skip_request > 0) {
      g_movie_skip_request--;
      if (fz.updateMovieInfo) fz.updateMovieInfo(fake_env, fz_class, 0, 0);
    }

    // Auto-answer a modal alert dialog a frame after it is raised, so the engine
    // stops blocking on the (never-arriving) async user choice. Delivered from
    // the render thread, between frames -- not reentrantly from the JNI stub.
    if (g_dialog_response_request > 0 && --g_dialog_response_request == 0 &&
        fz.setAlertDialogResponse) {
      fz.setAlertDialogResponse(fake_env, fz_class, DIALOG_AUTO_RESPONSE);
    }

    // Pump input on the render thread, right before render(): it shares the engine's
    // controller critical section with render()'s poll, so a cross-thread push would
    // deadlock under the single-GL-context handover.
    update_input();

    // HintStone Module_Init is dispatched by Fusion on a worker, but its Show
    // method starts UI animation and polls input. Run that final step here,
    // serialized with update_input and render, to avoid the worker-thread lock.
    hintstone_service_render_thread();

    fz.render(fake_env, fz_class);
    const int now_fbo0 = gl_draw_fbo0_count, now_fbon = gl_draw_fbon_count;
    int present = (now_fbo0 != prev_fbo0 || now_fbon != prev_fbon);

    // "Streaming" = texture-loader threads doing GL uploads (the real makeCurrent
    // count climbs): true during loading and in-game streaming, false in steady
    // gameplay. Gates the flicker fix and the CPU boost.
    extern volatile int egl_real_makecurrent_count;
    const int now_mc = egl_real_makecurrent_count;
    static int last_mc = -1, since_mc = 100000;
    if (now_mc != last_mc) { since_mc = 0; last_mc = now_mc; }
    else if (since_mc < 100000) since_mc++;

    // While streaming, the engine draws backgrounds black until their texture
    // uploads; drop a mostly-black frame and hold the last good one. glReadPixels
    // is gated to streaming so it never stalls steady gameplay.
    extern int egl_frame_black_count(void);
    if (since_mc < 20 && present && egl_frame_black_count() >= 13)
      present = 0;

    if (present)
      eglSwapBuffersHook(s_dpy, s_surf);
    else
      svcSleepThread(16000000ull); // held frame: keep the last good image

    // CPU boost during loading ONLY: ApmCpuBoostMode_FastLoad maxes the CPU but
    // downclocks the GPU, so it must never run in gameplay. Gated on the loading-
    // screen module being on screen (loading_screen_active / the g_ls_tick vtable
    // hook in hooks/game.c) so in-game streaming can't trip it.
    {
      const int loading = loading_screen_active();
      static int boosted = 1; // main() boosts through engine bring-up
      if (loading && !boosted)      { cpu_boost(1); boosted = 1; }
      else if (!loading && boosted) { cpu_boost(0); boosted = 0; }
    }

    prev_fbo0 = now_fbo0; prev_fbon = now_fbon;
    egl_gl_service_handover(); // let a waiting loader thread in at the boundary
  }

  if (fz.touchCancelAll)
    fz.touchCancelAll(fake_env, fz_class);
  fz.pause(fake_env, fz_class);
  egl_gl_ownership_release();
  return NULL;
}

// ---------------------------------------------------------------------------
// input pump (render thread): native controller + Android-compatible touch
// ---------------------------------------------------------------------------

static PadState pad;

// Native controller button bits, consumed by fnaController_ProcessJoypadController.
// Recovered by disassembling that function. UI modules do not consistently turn
// the analog stick into d-pad input, so the four digital bits must be sent too.
enum {
  PAD_DLEFT  = 0x001,  // engine slot 14 d-pad left
  PAD_DRIGHT = 0x002,  // engine slot 15 d-pad right
  PAD_DUP    = 0x004,  // engine slot 12 d-pad up
  PAD_DDOWN  = 0x008,  // engine slot 13 d-pad down
  PAD_A      = 0x020,  // engine slot 17 "A"  (action / use / UI-cancel)
  PAD_B      = 0x010,  // engine slot 18 "B"  (jump / UI-confirm)
  PAD_X      = 0x080,  // engine slot 16 "X"  (attack / modifier)
  PAD_Y      = 0x040,  // engine slot 19 "Y"  (special / grapple)
  PAD_SELECT = 0x200,  // engine select/back input -> Minus
  PAD_L      = 0x400,  // engine slot 8  "L"  shoulder
  PAD_R      = 0x800,  // engine slot 10 "R"  shoulder
  PAD_START  = 0x100,  // engine slot 6  START / pause
};

// Fusion's processed touch buffer has eleven entries. Its native SetData uses
// pointer as an unchecked array index, so never pass hid's finger_id directly:
// map each live Switch finger to one of these safe, stable Fusion pointer IDs.
#define MAX_FUSION_TOUCHES 11
typedef struct {
  int active;
  int seen;
  u32 switch_id;
  float x, y;
} TrackedTouch;

static TrackedTouch tracked_touches[MAX_FUSION_TOUCHES];
static int tracked_touch_count = 0;

static int find_touch(u32 switch_id) {
  for (int i = 0; i < MAX_FUSION_TOUCHES; i++)
    if (tracked_touches[i].active && tracked_touches[i].switch_id == switch_id)
      return i;
  return -1;
}

static int alloc_touch(void) {
  for (int i = 0; i < MAX_FUSION_TOUCHES; i++)
    if (!tracked_touches[i].active)
      return i;
  return -1;
}

static void touch_position(const HidTouchState *touch, float *x, float *y) {
  // hid touch coordinates are always in the Switch panel's 1280x720 space;
  // Fusion expects coordinates in the surface passed to nativeResize.
  *x = (float)touch->x * (float)screen_width / 1280.0f;
  *y = (float)touch->y * (float)screen_height / 720.0f;
}

static void clear_tracked_touches(void) {
  memset(tracked_touches, 0, sizeof(tracked_touches));
  tracked_touch_count = 0;
}

static void feed_touchscreen(void) {
  if (!fz.touchGestureStart || !fz.touchGestureEnd || !fz.touchDown ||
      !fz.touchUp || !fz.touchMove)
    return;

  HidTouchScreenState state = {0};
  const size_t samples = hidGetTouchScreenStates(&state, 1);
  int count = samples ? state.count : 0;
  if (count < 0) count = 0;
  if (count > 16) count = 16;

  for (int i = 0; i < MAX_FUSION_TOUCHES; i++)
    tracked_touches[i].seen = 0;

  for (int i = 0; i < count; i++) {
    const HidTouchState *touch = &state.touches[i];
    int slot = find_touch(touch->finger_id);
    float x, y;
    touch_position(touch, &x, &y);

    // libnx includes the final coordinates in an End sample. Preserve those,
    // but leave the slot unseen so the release pass below emits Up/GestureEnd.
    if (touch->attributes & HidTouchAttribute_End) {
      if (slot >= 0) {
        tracked_touches[slot].x = x;
        tracked_touches[slot].y = y;
      }
      continue;
    }

    if (slot < 0) {
      slot = alloc_touch();
      if (slot < 0)
        continue; // safe cap: Fusion cannot represent more simultaneous points

      TrackedTouch *tracked = &tracked_touches[slot];
      tracked->active = 1;
      tracked->seen = 1;
      tracked->switch_id = touch->finger_id;
      tracked->x = x;
      tracked->y = y;

      if (tracked_touch_count++ == 0) {
        fz.touchGestureStart(fake_env, fz_class, slot, x, y, 1.0f);
      } else {
        fz.touchDown(fake_env, fz_class, slot, x, y, 1.0f);
      }
    } else {
      TrackedTouch *tracked = &tracked_touches[slot];
      tracked->seen = 1;
      tracked->x = x;
      tracked->y = y;
      fz.touchMove(fake_env, fz_class, slot, x, y, 1.0f);
    }
  }

  int remaining = 0;
  for (int i = 0; i < MAX_FUSION_TOUCHES; i++)
    if (tracked_touches[i].active && tracked_touches[i].seen)
      remaining++;

  if (tracked_touch_count > 0 && remaining == 0) {
    // Android closes the final pointer with GestureEnd, which releases every
    // native touch and prevents a stale held point from owning later modal UI.
    for (int i = 0; i < MAX_FUSION_TOUCHES; i++) {
      if (tracked_touches[i].active) {
        fz.touchGestureEnd(fake_env, fz_class, i, tracked_touches[i].x,
                           tracked_touches[i].y, 1.0f);
        break;
      }
    }
    clear_tracked_touches();
  } else {
    for (int i = 0; i < MAX_FUSION_TOUCHES; i++) {
      TrackedTouch *tracked = &tracked_touches[i];
      if (tracked->active && !tracked->seen) {
        fz.touchUp(fake_env, fz_class, i, tracked->x, tracked->y, 1.0f);
        memset(tracked, 0, sizeof(*tracked));
        tracked_touch_count--;
      }
    }
  }
}

// circle->square stick remap so diagonals reach full deflection
static void stick_c2s(float *x, float *y) {
  const float ax = fabsf(*x), ay = fabsf(*y);
  const float m = (ax > ay) ? ax : ay;
  if (m < 1e-6f) return;
  const float s = sqrtf(*x * *x + *y * *y) / m;
  *x *= s; *y *= s;
  if (*x > 1.f) *x = 1.f; else if (*x < -1.f) *x = -1.f;
  if (*y > 1.f) *y = 1.f; else if (*y < -1.f) *y = -1.f;
}

// Push the Switch pad into the engine via the native controller ABI. The d-pad
// reinforces the left stick for movement and is also sent through the native
// digital slots used directly by modal UI modules.
static void feed_controller(void) {
  if (!fz.controllerSetData)
    return;
  const u64 down = padGetButtons(&pad);

  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  float lx = (float)ls.x / 32767.0f, ly = (float)ls.y / 32767.0f;
  // d-pad reinforces the stick
  if (down & HidNpadButton_Left)  lx = -1.f;
  if (down & HidNpadButton_Right) lx =  1.f;
  if (down & HidNpadButton_Down)  ly = -1.f;
  if (down & HidNpadButton_Up)    ly =  1.f;
  stick_c2s(&lx, &ly);

  int mask = 0;
  if (down & HidNpadButton_A) mask |= PAD_B;
  if (down & HidNpadButton_B) mask |= PAD_A;
  if (down & HidNpadButton_X) mask |= PAD_X;
  if (down & HidNpadButton_Y) mask |= PAD_Y;
  if (down & (HidNpadButton_L | HidNpadButton_ZL)) mask |= PAD_L;
  if (down & (HidNpadButton_R | HidNpadButton_ZR)) mask |= PAD_R;
  if (down & HidNpadButton_Plus) mask |= PAD_START;
  if (down & HidNpadButton_Minus) mask |= PAD_SELECT;
  if (down & HidNpadButton_Left) mask |= PAD_DLEFT;
  if (down & HidNpadButton_Right) mask |= PAD_DRIGHT;
  if (down & HidNpadButton_Up) mask |= PAD_DUP;
  if (down & HidNpadButton_Down) mask |= PAD_DDOWN;

  // SetData negates the incoming Y before storing (Android Y is +down), so pass
  // -ly to land the engine's stick at +up == Switch stick up.
  fz.controllerSetData(1 /*connected*/, mask, lx, -ly);
}

static void update_input(void) {
  padUpdate(&pad);
  feed_touchscreen();
  feed_controller();
}

int main(int argc, char **argv) {
  saltynx_start();
  atexit(saltynx_stop);
  pthr_pin_worker_core();
  cpu_boost(1);

  // resolve the install directory (from argv[0]/cwd) before touching any file
  resolve_base_dir(argc, argv);

  if (read_config(g_config_path) < 0)
    write_config(g_config_path);

  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);

  extern char *fake_heap_start;
  const unsigned heap_mb =
      (unsigned)(((char *)heap_so_base - fake_heap_start) / (1024 * 1024));

  if (heap_mb < 1500)
    fatal_error("Not enough memory (%u MB).\n\n"
                "Launch hbmenu over a game (hold R while\n"
                "starting any installed title), then start\n"
                "this port from there.", heap_mb);

  // create the SD output dirs, then extract (first run) + index the assets
  // before any asset access
  mkdir(g_save_path, 0777);
  mkdir(g_write_path, 0777);
  mkdir(g_cache_path, 0777);
  mkdir(g_assets_dir, 0777);
  const char *packs[] = {
    g_pack_path[0], g_pack_path[1], g_pack_path[2], g_pack_path[3],
  };
  if (asset_vfs_init(g_assets_dir, packs, 4) < 0)
    fatal_error("Could not read the game assets.\nCopy the APK's assets/assetpack1..4\n"
                "into\n%s/ (first run extracts them;\nneeds ~1.7 GB free).", g_base);

  if (so_load(&game_mod, g_so_path, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  update_imports();
  so_relocate(&game_mod);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  patch_game();
  resolve_fusion_exports();

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  // fake TLS must exist before any game code runs (static ctors read the
  // stack-guard cookie); init_array below is game code
  pthr_install_fake_tls();
  so_execute_init_array(&game_mod);
  so_free_temp(&game_mod);

  jni_init();
  fz_class      = jni_make_object("com/wbgames/LEGOgame/GameGLSurfaceView");
  fake_activity = jni_make_object("com/wb/goog/lnjgo/GameActivity");
  fake_eglcfg   = jni_make_object("javax/microedition/khronos/egl/EGLConfig");

  run_setup_phase();

  // Initialise input BEFORE the render thread starts: it pumps the pad itself from
  // inside its frame loop, so `pad` must be ready before gl_thread_ready is reached.
  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  // spawn the GL thread (real pthread so its identity matches the egl ownership
  // machine's pthread_self bookkeeping) and wait for engine bring-up
  if (pthread_create(&gl_thread, NULL, gl_thread_main, NULL) != 0)
    fatal_error("Could not start the render thread.");
  while (!gl_thread_ready)
    svcSleepThread(2000000ull); // 2 ms

  while (appletMainLoop()) {
    // The main thread only nurses the applet lifecycle: input and rendering run on
    // the GL thread, the CPU boost is driven from there. It holds no GL context, so
    // the handover check is a cheap TLS no-op here.
    egl_gl_service_handover();
    svcSleepThread(1000000000ull / 60);
  }

  gl_should_quit = 1;
  pthread_join(gl_thread, NULL);

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
