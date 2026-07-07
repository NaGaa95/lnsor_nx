/* fd_stream.h -- ffmpeg-backed streaming decoder for OpenSL ES ANDROIDFD players
 *
 * The TT-Fusion engine streams its music (and some audio) as compressed files
 * (mp3) handed to OpenSL ES via SL_DATALOCATOR_ANDROIDFD. The vendored Wilhelm
 * build has no decoder, so those players were silent. This module decodes the
 * file (via libavformat/libavcodec/libswresample) to 44100 Hz stereo s16 and
 * streams it into the player's OpenSL buffer queue, which the existing
 * OUTPUTMIXEXT software mixer already knows how to play.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FD_STREAM_H__
#define __FD_STREAM_H__

#ifdef USE_FDSTREAM

// forward decl to avoid pulling the whole OpenSL header here
struct CAudioPlayer_struct;

// Create a streaming decoder for an ANDROIDFD player from its FD data locator
// (this->mDataSource.mLocator.mFD). Returns 0 on success; stores an opaque
// handle in ap->mFdStream. Does not start playback.
int  fdstream_create(struct CAudioPlayer_struct *ap);

// Begin/resume feeding the player's buffer queue (called on SL_PLAYSTATE_PLAYING).
void fdstream_start(struct CAudioPlayer_struct *ap);

// Pause feeding (SL_PLAYSTATE_PAUSED) or stop+rewind (SL_PLAYSTATE_STOPPED).
void fdstream_pause(struct CAudioPlayer_struct *ap);
void fdstream_stop(struct CAudioPlayer_struct *ap);

// Enable/disable looping (the engine drives this through SL_IID_SEEK SetLoop).
void fdstream_set_loop(struct CAudioPlayer_struct *ap, int loop);

// Tear down the decoder + thread (called from the player's destroy hook).
void fdstream_destroy(struct CAudioPlayer_struct *ap);

#endif // USE_FDSTREAM
#endif
