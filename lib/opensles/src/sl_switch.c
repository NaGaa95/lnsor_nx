/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** \file sl_switch.c Nintendo Switch platform backend (SDL2 audio).
 *
 * Replaces the AOSP SDL 1.2 backend (SDL.c): same pull model, the SDL audio
 * thread drains the software mixer via IOutputMixExt_FillBuffer. SDL2 still
 * ships the legacy SDL_OpenAudio/SDL_PauseAudio API used by IEngine.c, and on
 * the Switch it resamples our 44100 Hz mix to audout's native 48 kHz.
 */

#include "sles_allinclusive.h"

#include <SDL2/SDL.h>
#include <switch.h>

#include "pthr.h" // pthr_ensure_fake_tls

// Sample frames per callback. MUST stay 1024: smaller buffers wedge the SDL
// Switch audout backend forever in SWITCHAUDIO_PlayDevice (no completions ->
// total silence, and every mixer-acknowledge handshake hangs with it).
#define SWITCH_AUDIO_SAMPLES 1024

/** \brief usec process clock backing the LSWTCS IPlay::GetPosition additions
 *  (drop-in for the Vita's sceKernelGetProcessTimeLow) */

SceUInt32 sceKernelGetProcessTimeLow(void)
{
    static u64 freq;
    if (freq == 0)
        freq = armGetSystemTickFreq();
    return (SceUInt32)(armGetSystemTick() / (freq / 1000000ull));
}

/** \brief Called by SDL to fill the next audio output buffer */

// SDL output sample rate (set in SDL_open); the cutscene player resamples to it.
int g_fmv_out_rate = 44100;
// fmv.c: while a cutscene plays, overwrites `stream` with the movie's audio and
// returns 1 (so the game mix is silenced). No-op (returns 0) otherwise.
extern int fmv_audio_override(void *stream, int len_bytes);

static void SDLCALL SDL_callback(void *context, Uint8 *stream, int len)
{
    assert(len > 0);
    // SDL's audio thread was not created through the game-thread trampoline; the
    // mixer invokes game buffer-queue callbacks which read the bionic stack-guard
    // cookie from TPIDR_EL0. Pin it to a background core (off the render/main
    // cores) once, so a heavy frame doesn't stutter the renderer.
    pthr_ensure_fake_tls();
    static int first = 1;
    if (first) {
        pthr_pin_bg_core();
        first = 0;
    }
    IEngine *thisEngine = (IEngine *) context;
    interface_lock_shared(thisEngine);
    COutputMix *outputMix = thisEngine->mOutputMix;
    interface_unlock_shared(thisEngine);
    if (NULL != outputMix) {
        SLOutputMixExtItf OutputMixExt = &outputMix->mOutputMixExt.mItf;
        IOutputMixExt_FillBuffer(OutputMixExt, stream, (SLuint32) len);
    } else {
        memset(stream, 0, (size_t) len);
    }
    // A cutscene's audio replaces the game mix while it plays (the game mix above
    // still runs to keep its buffer queues drained, but is overwritten here).
    fmv_audio_override(stream, len);
}

/** \brief Called during slCreateEngine to initialize the audio output */

void SDL_open(IEngine *thisEngine)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
        return;

    SDL_AudioSpec fmt;
    SDL_zero(fmt);
    fmt.freq = &_opensles_user_freq != NULL ? _opensles_user_freq : 44100;
    fmt.format = AUDIO_S16;
    fmt.channels = STEREO_CHANNELS;
    fmt.samples = SWITCH_AUDIO_SAMPLES;
    fmt.callback = SDL_callback;
    fmt.userdata = (void *) thisEngine;

    if (SDL_OpenAudio(&fmt, NULL) < 0)
        return;
    g_fmv_out_rate = fmt.freq;   // cutscene player resamples its audio to this
    SDL_PauseAudio(0);
}

/** \brief Called during Object::Destroy for engine to shutdown the audio output */

void SDL_close(void)
{
    SDL_CloseAudio();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
