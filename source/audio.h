/* audio.h -- captured global music and sound volumes for the Switch port
 *
 * g_globalMusicGain / g_globalSoundGain are captured via hooks on
 * geMusic_SetGlobalVolume / geSound_SetGlobalVolume (see source/hooks/audio.c).
 * The original engine setters still run. The mixer uses the music gain directly
 * and uses the sound gain for immediate muting; sound player levels already
 * include the engine's master volume and positional attenuation.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __AUDIO_H__
#define __AUDIO_H__

/* Master volume for music (ANDROIDFD) players; set by geMusic_SetGlobalVolume. */
extern volatile float g_globalMusicGain;
/* Master volume for SFX (BUFFERQUEUE) players; set by geSound_SetGlobalVolume. */
extern volatile float g_globalSoundGain;
/* Non-zero when the respective hook was successfully installed. */
extern volatile int   g_musicHookActive;
extern volatile int   g_soundHookActive;

/* Installs geMusic/geSound_SetGlobalVolume hooks.
 * Must be called after so_relocate() but before so_finalize(). */
void patch_audio(void);

#endif /* __AUDIO_H__ */
