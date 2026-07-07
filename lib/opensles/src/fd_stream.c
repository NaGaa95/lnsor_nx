/* fd_stream.c -- ffmpeg-backed streaming decoder for ANDROIDFD players.
 * See fd_stream.h. Decodes the file to 44100 Hz stereo s16 and feeds the
 * player's OpenSL buffer queue from a dedicated thread.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifdef USE_FDSTREAM

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "sles_allinclusive.h"
#include "fd_stream.h"

#ifdef __SWITCH__
#include "pthr.h"
#endif

#define OUT_RATE     44100
#define OUT_CH       2
#define FRAME_BYTES  4              // stereo s16
#define NBUF         4
#define BUFSZ        (8192 * FRAME_BYTES)  // ~186 ms per buffer @ 44100
#define AVIO_BUF_SZ  16384

typedef struct FdStream {
    CAudioPlayer *ap;

    int      fd;        // own dup into the container
    int64_t  base, len; // byte window inside the container
    int64_t  pos;       // absolute read cursor

    AVFormatContext *fmt;
    AVIOContext     *avio;
    AVCodecContext  *cctx;
    SwrContext      *swr;
    AVPacket        *pkt;
    AVFrame         *frame;
    int              sidx;

    volatile int     loop;
    volatile int     playing;
    volatile int     quit;
    int              need_rewind;
    int              ended;

    uint8_t  *bufs[NBUF];
    int       write_idx;

    pthread_t        thr;
    int              thr_started;
} FdStream;

// --- AVIO over the fd window -----------------------------------------------

static int read_cb(void *opaque, uint8_t *buf, int size) {
    FdStream *s = (FdStream *)opaque;
    int64_t remaining = (s->base + s->len) - s->pos;
    if (remaining <= 0) return AVERROR_EOF;
    if (size > remaining) size = (int)remaining;
    if (lseek(s->fd, s->pos, SEEK_SET) < 0) return AVERROR_EOF;
    int r = (int)read(s->fd, buf, size);
    if (r <= 0) return AVERROR_EOF;
    s->pos += r;
    return r;
}

static int64_t seek_cb(void *opaque, int64_t off, int whence) {
    FdStream *s = (FdStream *)opaque;
    if (whence == AVSEEK_SIZE) return s->len;
    int64_t np;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: np = off; break;
        case SEEK_CUR: np = (s->pos - s->base) + off; break;
        case SEEK_END: np = s->len + off; break;
        default: return -1;
    }
    if (np < 0) np = 0;
    if (np > s->len) np = s->len;
    s->pos = s->base + np;
    return np;
}

// --- decode -----------------------------------------------------------------

static void rewind_decoder(FdStream *s) {
    av_seek_frame(s->fmt, s->sidx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(s->cctx);
    s->pos = s->base;
}

// Fill out[] with up to max_bytes of interleaved s16 stereo @44100.
// Returns bytes produced (0 only at true end with no loop).
static int decode_fill(FdStream *s, uint8_t *out, int max_bytes) {
    int got = 0;
    if (s->need_rewind) { rewind_decoder(s); s->need_rewind = 0; s->ended = 0; }

    while (got < max_bytes) {
        int r = avcodec_receive_frame(s->cctx, s->frame);
        if (r == 0) {
            uint8_t *outp = out + got;
            int cap = (max_bytes - got) / FRAME_BYTES;
            int conv = swr_convert(s->swr, &outp, cap,
                                   (const uint8_t **)s->frame->extended_data,
                                   s->frame->nb_samples);
            av_frame_unref(s->frame);
            if (conv > 0) got += conv * FRAME_BYTES;
            continue;
        }
        if (r == AVERROR(EAGAIN)) {
            int rp = av_read_frame(s->fmt, s->pkt);
            if (rp < 0) {
                avcodec_send_packet(s->cctx, NULL); // drain
                while (got < max_bytes &&
                       avcodec_receive_frame(s->cctx, s->frame) == 0) {
                    uint8_t *outp = out + got;
                    int cap = (max_bytes - got) / FRAME_BYTES;
                    int conv = swr_convert(s->swr, &outp, cap,
                                           (const uint8_t **)s->frame->extended_data,
                                           s->frame->nb_samples);
                    av_frame_unref(s->frame);
                    if (conv > 0) got += conv * FRAME_BYTES;
                }
                if (s->loop) { rewind_decoder(s); continue; }
                s->ended = 1;
                break;
            }
            if (s->pkt->stream_index == s->sidx)
                avcodec_send_packet(s->cctx, s->pkt);
            av_packet_unref(s->pkt);
            continue;
        }
        break; // decode error
    }
    return got;
}

// --- decoder thread: keep the player's buffer queue topped up --------------
// We poll the queue depth (mState.count) instead of using a buffer-queue
// completion callback -- registering a callback during CreateAudioPlayer faults
// (the object lock isn't live yet). Enqueue itself takes the object lock, which
// is safe once the player is realized/playing.

static void *thread_fn(void *arg) {
    FdStream *s = (FdStream *)arg;
#ifdef __SWITCH__
    pthr_pin_worker_core();
#endif
    SLBufferQueueItf bq = (SLBufferQueueItf)&s->ap->mBufferQueue;
    while (!s->quit) {
        if (!s->playing) { usleep(5000); continue; }
        // how many buffers are still queued? (read the field directly; the mixer
        // updates it under lock, a slightly stale read just delays one enqueue)
        if ((int)s->ap->mBufferQueue.mState.count >= NBUF) { usleep(3000); continue; }

        int n = decode_fill(s, s->bufs[s->write_idx], BUFSZ);
        if (n <= 0) { usleep(10000); continue; } // ended (no loop): idle

        if (IBufferQueue_Enqueue(bq, s->bufs[s->write_idx], n) == SL_RESULT_SUCCESS)
            s->write_idx = (s->write_idx + 1) % NBUF;
        else
            usleep(3000);
    }
    return NULL;
}

// --- public API -------------------------------------------------------------

int fdstream_create(CAudioPlayer *ap) {
    SLDataLocator_AndroidFD *fd = &ap->mDataSource.mLocator.mFD;
    uint8_t *aviobuf = NULL;
    const AVCodec *codec = NULL;
    AVChannelLayout out_ch;
    int i;

    if (fd->fd < 0) return -1;

    FdStream *s = (FdStream *)calloc(1, sizeof(*s));
    if (!s) return -1;
    s->ap = ap;
    s->base = fd->offset;
    s->len  = (fd->length > 0) ? fd->length : INT64_MAX;
    s->pos  = s->base;
    s->sidx = -1;
    s->fd = -1;

    s->fd = dup((int)fd->fd);
    if (s->fd < 0) goto fail;

    for (i = 0; i < NBUF; i++) {
        s->bufs[i] = (uint8_t *)malloc(BUFSZ);
        if (!s->bufs[i]) goto fail;
    }

    aviobuf = (uint8_t *)av_malloc(AVIO_BUF_SZ);
    if (!aviobuf) goto fail;
    s->avio = avio_alloc_context(aviobuf, AVIO_BUF_SZ, 0, s, read_cb, NULL, seek_cb);
    if (!s->avio) { av_free(aviobuf); goto fail; }

    s->fmt = avformat_alloc_context();
    if (!s->fmt) goto fail;
    s->fmt->pb = s->avio;
    s->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&s->fmt, NULL, NULL, NULL) < 0) { s->fmt = NULL; goto fail; }
    if (avformat_find_stream_info(s->fmt, NULL) < 0) goto fail;

    s->sidx = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (s->sidx < 0 || !codec) goto fail;

    s->cctx = avcodec_alloc_context3(codec);
    if (!s->cctx) goto fail;
    avcodec_parameters_to_context(s->cctx, s->fmt->streams[s->sidx]->codecpar);
    if (avcodec_open2(s->cctx, codec, NULL) < 0) goto fail;

    av_channel_layout_default(&out_ch, OUT_CH);
    if (swr_alloc_set_opts2(&s->swr, &out_ch, AV_SAMPLE_FMT_S16, OUT_RATE,
                            &s->cctx->ch_layout, s->cctx->sample_fmt,
                            s->cctx->sample_rate, 0, NULL) < 0) goto fail;
    if (swr_init(s->swr) < 0) goto fail;

    s->pkt = av_packet_alloc();
    s->frame = av_frame_alloc();
    if (!s->pkt || !s->frame) goto fail;

    // the queue we feed is plain 44100/2/16; no conversion needed in Enqueue.
    // NOTE: samplerate is in milliHertz here (matches mSampleRateMilliHz), so a
    // bare 44100 means 44.1 Hz and makes the mixer upsample 1000x into garbage.
    ap->mBufferQueue.samplerate = OUT_RATE * 1000;
    ap->mBufferQueue.channels = OUT_CH;
    ap->mBufferQueue.bps = 16;

    // ANDROIDFD players don't expose the BufferQueue interface, so its back-
    // pointer to the lockable object was never set -> Enqueue's lock would deref
    // null. Point it at the player's (already-initialised) object.
    ap->mBufferQueue.mThis = &ap->mObject;

    ap->mFdStream = s;
    return 0;

fail:
    ap->mFdStream = NULL;
    if (s->frame) av_frame_free(&s->frame);
    if (s->pkt) av_packet_free(&s->pkt);
    if (s->swr) swr_free(&s->swr);
    if (s->cctx) avcodec_free_context(&s->cctx);
    if (s->fmt) avformat_close_input(&s->fmt);
    if (s->avio) { av_free(s->avio->buffer); avio_context_free(&s->avio); }
    for (i = 0; i < NBUF; i++) free(s->bufs[i]);
    if (s->fd >= 0) close(s->fd);
    free(s);
    return -1;
}

void fdstream_start(CAudioPlayer *ap) {
    FdStream *s = (FdStream *)ap->mFdStream;
    if (!s) return;
    s->playing = 1;
    if (!s->thr_started) {
        s->thr_started = 1;
        pthread_create(&s->thr, NULL, thread_fn, s);
    }
}

void fdstream_pause(CAudioPlayer *ap) {
    FdStream *s = (FdStream *)ap->mFdStream;
    if (s) s->playing = 0;
}

void fdstream_stop(CAudioPlayer *ap) {
    FdStream *s = (FdStream *)ap->mFdStream;
    if (!s) return;
    s->playing = 0;
    s->need_rewind = 1;
}

void fdstream_set_loop(CAudioPlayer *ap, int loop) {
    FdStream *s = (FdStream *)ap->mFdStream;
    if (s) s->loop = loop ? 1 : 0;
}

void fdstream_destroy(CAudioPlayer *ap) {
    FdStream *s = (FdStream *)ap->mFdStream;
    if (!s) return;
    ap->mFdStream = NULL;

    s->quit = 1;
    s->playing = 0;
    if (s->thr_started) pthread_join(s->thr, NULL);

    if (s->frame) av_frame_free(&s->frame);
    if (s->pkt) av_packet_free(&s->pkt);
    if (s->swr) swr_free(&s->swr);
    if (s->cctx) avcodec_free_context(&s->cctx);
    if (s->fmt) avformat_close_input(&s->fmt);
    if (s->avio) { av_free(s->avio->buffer); avio_context_free(&s->avio); }
    for (int i = 0; i < NBUF; i++) free(s->bufs[i]);
    if (s->fd >= 0) close(s->fd);
    free(s);
}

#endif // USE_FDSTREAM
