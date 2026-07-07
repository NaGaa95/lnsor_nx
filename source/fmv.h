/* fmv.h -- cutscene (.mp4) player for LEGO Ninjago: Shadow of Ronin.
 *
 * Shadow of Ronin's native fnaFMV_* layer plays cutscene video by asking the
 * Java side to run the .mp4 through its MediaPlayer (the startMoviePlayback
 * JNI). jni_fake.c intercepts that call and posts a request here; the wrapper
 * render thread (the sole GL-context owner) decodes the .mp4 with ffmpeg and
 * draws it on a quad.
 */
#pragma once

// Cutscene request handshake. The startMoviePlayback JNI handler copies the movie
// name, sets g_fmv_request, and blocks on g_fmv_done; the render thread (main.c)
// sees the request, plays the movie, then sets g_fmv_done. (If the JNI call is
// already on the render thread, it plays directly instead -- see jni_fake.c.)
extern volatile int  g_fmv_request;
extern volatile int  g_fmv_done;
extern volatile int  g_fmv_active;
extern char          g_fmv_name[256];

// Play one cutscene, blocking until it ends or is skipped. MUST be called on the
// render thread (the GL-context owner).
void fmv_play(const char *want);

// Audio-thread hook (sl_switch.c): fills `stream` with the cutscene's PCM while a
// movie plays and returns 1 (game mix silenced); returns 0 otherwise.
int fmv_audio_override(void *stream, int len_bytes);
