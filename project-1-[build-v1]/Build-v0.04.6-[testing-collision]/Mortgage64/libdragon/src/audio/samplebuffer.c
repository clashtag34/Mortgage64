/**
 * @file samplebuffer.c
 * @brief Sample buffer
 * @ingroup mixer
 */

#include "mixer.h"
#include "samplebuffer.h"
#include "n64sys.h"
#include "n64types.h"
#include "utils.h"
#include "rspq.h"
#include "../rspq/rspq_internal.h"
#include "debug.h"
#include <string.h>

/** @brief Set to 1 to activate debug logs */
#define MIXER_TRACE   0

#if MIXER_TRACE
/** @brief like debugf(), but writes only if #MIXER_TRACE is not 0 */
#define tracef(fmt, ...)  debugf(fmt, ##__VA_ARGS__)
#else
/** @brief like debugf(), but writes only if #MIXER_TRACE is not 0 */
#define tracef(fmt, ...)  ({ })
#endif

void samplebuffer_init(samplebuffer_t *buf, uint8_t* uncached_mem, int nbytes, int state_size) {
	memset(buf, 0, sizeof(samplebuffer_t));

	// Store the buffer pointer as uncached address. We don't want to access
	// it with CPU as we want to build samples with RSP, and all APIs assume
	// that content is committed to RDRAM (not cache).
	assertf(UncachedAddr(uncached_mem) == uncached_mem, 
		"specified buffer must be in the uncached segment.\nTry using malloc_uncached() to allocate it");
	buf->ptr_and_flags = (uint32_t)uncached_mem;
	assert((buf->ptr_and_flags & 7) == 0);
	buf->size = nbytes;

	if (state_size) {
		buf->state = uncached_mem + nbytes;
		buf->state_size = state_size;
	} 
		
	buf->wnext = -1;
}

void samplebuffer_set_bps(samplebuffer_t *buf, int bits_per_sample) {
	assert(bits_per_sample == 8 || bits_per_sample == 16 || bits_per_sample == 32);
	assertf(buf->widx == 0 && buf->ridx == 0 && buf->wpos == 0,
		"samplebuffer_set_bps can only be called on an empty samplebuffer");

	int nbytes = buf->size << SAMPLES_BPS_SHIFT(buf);

	int bps = bits_per_sample == 8 ? 0 : (bits_per_sample == 16 ? 1 : 2);
	buf->ptr_and_flags = SAMPLES_PTR_MAKE(SAMPLES_PTR(buf), bps);
	buf->size = nbytes >> bps;
}

void samplebuffer_set_waveform(samplebuffer_t *buf, waveform_t *wave, WaveformRead read) {
	buf->wave = wave;
	buf->wv_read = read;
	assert(wave->state_size <= buf->state_size);
}

bool samplebuffer_is_inited(samplebuffer_t *buf)
{
	return SAMPLES_PTR(buf) != NULL;
}

void samplebuffer_close(samplebuffer_t *buf) {
	void *ptr = SAMPLES_PTR(buf);
	if (ptr)
		free_uncached(ptr);
	memset(buf, 0, sizeof(samplebuffer_t));
}

void* samplebuffer_get(samplebuffer_t *buf, int wpos, int *wlen) {
	// ROUNDUP8_BPS rounds up the specified number of samples
	// (given the bps shift) so that they span an exact multiple
	// of 8 bytes. This will be applied to the number of samples
	// requested to wv_read(), to make sure that we always
	// keep the sample buffer filled with a multiple of 8 bytes.
	// This is not strictly required because dma_read() can do wonders,
	// but it results in slightly faster DMA transfers and its almost free
	// to do here.
	/// @cond
	#define ROUNDUP8_BPS(nsamples, bps) \
		(((nsamples)+((8>>(bps))-1)) >> (3-(bps)) << (3-(bps)))
	/// @endcond

	int bps = SAMPLES_BPS_SHIFT(buf);

	tracef("samplebuffer_get: wpos=%x wlen=%x\n", wpos, *wlen);

	if (buf->widx == 0 || wpos < buf->wpos || wpos > buf->wpos+buf->widx) {
		// If the requested position is totally outside
		// the existing range (and not even consecutive),
		// Flush the buffer and decode from scratch.
		// Normally this is because of seeking, but might
		// also be just because we discarded all the contents
		// of the buffer, so be sure to set seeking only if
		// the position is different from what we expected.
		tracef("samplebuffer_get: flushing buffer: buf->widx=%x buf->wpos=%x buf->wnext=%x\n", buf->widx, buf->wpos, buf->wnext);
		bool seeking = wpos != buf->wnext;	
		samplebuffer_flush(buf);
		buf->wpos = wpos;
		// Avoid setting a position that is odd, because it would case a
		// 2-byte phase change in the sample buffer, which would make 
		// impossible to call dma_read.
		int len = *wlen;
		if ((buf->wpos << bps) & 1) {
			buf->wpos--; len++;
		}
		buf->wv_read(buf->wave->ctx, buf, buf->wpos, ROUNDUP8_BPS(len, bps), seeking);
		buf->wnext = buf->wpos + buf->widx;
	} else {
		// Record first sample that we still need to keep in the sample
		// buffer. This is important to do now because decoder_read might
		// push more samples than required into the buffer and force
		// to compact the buffer. We thus need to know which samples
		// are still required.
		buf->ridx = wpos - buf->wpos;

		// Part of the requested samples are already in the sample buffer.
		// Check how many we can reuse. For instance, if there's a waveform
		// loop, the whole loop might already be in the sample buffer, so
		// no further decoding is necessary.
		int reuse = buf->wpos + buf->widx - wpos;

		// If the existing samples are not enough, read the missing
		// through the callback.
		if (reuse < *wlen) {
			tracef("samplebuffer_get: read missing: reuse=%x wpos=%x wlen=%x\n", reuse, wpos, *wlen);
			assertf(wpos+reuse == buf->wnext, "wpos:%x reuse:%x buf->wnext:%x", wpos, reuse, buf->wnext);
			buf->wv_read(buf->wave->ctx, buf, wpos+reuse, ROUNDUP8_BPS(*wlen-reuse, bps), false);
			buf->wnext = buf->wpos + buf->widx;
		}
	}

	assertf(wpos >= buf->wpos && wpos < buf->wpos+buf->widx, 
		"samplebuffer_get: logic error\n"
		"wpos:%x buf->wpos:%x buf->widx:%x", wpos, buf->wpos, buf->widx);

	int idx = wpos - buf->wpos;

	// If the sample buffer contains less samples than requested,
	// report that by updating *wlen. This will cause cracks in the
	// audio as silence will be inserted by the mixer.
	int len = buf->widx - idx;
	if (len < *wlen)
		*wlen = len;

	return SAMPLES_PTR(buf) + (idx << SAMPLES_BPS_SHIFT(buf));
}

void* samplebuffer_append(samplebuffer_t *buf, int wlen) {
	// If the requested number of samples doesn't fit the buffer, we
	// need to make space for it by discarding older samples.
	if (buf->widx + wlen > buf->size) {
		// Make space in the buffer by discarding everything up to the
		// ridx index, which is the first sample that we still need for playback.
		assertf(buf->widx >= buf->ridx,
			"samplebuffer_append: invalid consistency check\n"
			"widx:%x ridx:%x\n", buf->widx, buf->ridx);

		// Rollback ridx until it hit a 8-byte aligned position.
		// This preserves the guarantee that samplebuffer_append
		// will always return a 8-byte aligned pointer, which is
		// good for DMA purposes.
		int ridx = buf->ridx;
		while ((ridx << SAMPLES_BPS_SHIFT(buf)) & 7)
			ridx--;
		samplebuffer_discard(buf, buf->wpos+ridx);
	}

	assertf(((buf->wpos<< SAMPLES_BPS_SHIFT(buf)) % 2) == 0, "buf->wpos:%x", buf->wpos);

	// If there is still not space in the buffer, it means that the
	// buffer is too small for this append call. This is a logic error,
	// so better assert right away.
	// TODO: in principle, we could bubble this error up to the callers,
	// let them fill less samples than requested, and obtain some cracks
	// in the audio. Is it worth it?
	assertf(buf->widx + wlen <= buf->size,
		"samplebuffer_append: buffer too small\n"
		"ridx:%x widx:%x wlen:%x size:%x", buf->ridx, buf->widx, wlen, buf->size);

	void *data = SAMPLES_PTR(buf) + (buf->widx << SAMPLES_BPS_SHIFT(buf));
	buf->widx += wlen;
	return data;
}

void samplebuffer_undo(samplebuffer_t *buf, int wlen) {
	tracef("samplebuffer_truncate: wlen=%x\n", wlen);
	assertf(buf->widx >= wlen, "samplebuffer_append_undo: invalid wlen:%x widx:%x", wlen, buf->widx);
	buf->widx -= wlen;
}

void samplebuffer_discard(samplebuffer_t *buf, int wpos) {
	// Compute the index of the first sample that will be preserved (and thus
	// will be moved to position 0 of the buffer).
	int idx = wpos - buf->wpos;
	if (idx <= 0)
		return;
	if (idx > buf->widx)
		idx = buf->widx;

	// Make sure moving this sample at the beginning of the buffer doesn't change
	// the 2-byte phase of the waveform address. This is not strictly required,
	// but it helps waveform implementations that want to use dma_read().
	if ((idx << SAMPLES_BPS_SHIFT(buf)) & 1) {
		idx--;
		if (idx == 0)
			return;
	}

	tracef("samplebuffer_discard: wpos=%x idx:%x buf->wpos=%x buf->widx=%x\n", wpos, idx, buf->wpos, buf->widx);
	int kept_bytes = (buf->widx - idx) << SAMPLES_BPS_SHIFT(buf);
	if (kept_bytes > 0) {		
		tracef("samplebuffer_discard: compacting buffer, moving 0x%x bytes\n", kept_bytes);

		// Samples are normally generated via RSP. Before moving them with the CPU,
		// we must be sure the RSP is done with.
		// FIXME: this could be avoided with a RSP memmove command, but it remains
		// to be seen what is more efficient.
		bool highpri = rspq_in_highpri();
		if (highpri) rspq_highpri_end();
		rspq_highpri_sync();
		if (highpri) rspq_highpri_begin();

		// FIXME: this violates the zero-copy principle as we do a memmove here.
		// The problem is that the RSP ucode doesn't fully support a circular
		// buffer of samples (and also our samplebuffer_t isn't structured for
		// this). Luckily, this is a rare chance and in most cases just a few
		// samples are moved (in the normal playback case, it should be just 1,
		// as in general a sample could be used more than once for resampling).
		uint8_t *src = SAMPLES_PTR(buf) + (idx << SAMPLES_BPS_SHIFT(buf));
		uint8_t *dst = SAMPLES_PTR(buf);
		assert(((uint32_t)dst & 7) == 0);

		// Optimized copy of samples. We work on uncached memory directly
		// so that we don't need to flush, and use only 64-bits ops. We round up
		// to a multiple of 8 the amount of bytes, as it doesn't matter if we
		// copy more, as long as we're fast.
		// This has been benchmarked to be faster than memmove() + cache flush.
		kept_bytes = ROUND_UP(kept_bytes, 8);
		u_uint64_t *src64 = (u_uint64_t*)src;
		uint64_t *dst64 = (uint64_t*)dst;
		for (int i=0;i<kept_bytes/8;i++)
			*dst64++ = *src64++;
	}

	buf->wpos += idx;
	buf->widx -= idx;
	buf->ridx -= idx;
	if (buf->ridx < 0)
		buf->ridx = 0;
}

void samplebuffer_flush(samplebuffer_t *buf) {
	buf->wpos = buf->widx = buf->ridx = 0;
	buf->wnext = -1;
}
