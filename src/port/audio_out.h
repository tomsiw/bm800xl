/*
 * audio_out.h -- the audio device interface the kernel installs.
 *
 * The emulation core decides the shape of the audio stream (sample rate, mono
 * or stereo, 8- or 16-bit).  Something then has to make the noise, and on this
 * target that is a Circle sound device driven by DMA.  This header is the seam
 * between the two: the kernel implements it, src/port/sound_bridge.c consumes
 * it.
 *
 * The kernel's implementation lives in src/audio/audio_out.cpp and wraps
 * Circle's CPWMSoundBaseDevice (with its RP1 counterpart on the Pi 5).
 *
 * Implementations are called from the emulation loop, that is from the main
 * thread, never from an interrupt.
 */

#ifndef ATARI_PI_AUDIO_OUT_H
#define ATARI_PI_AUDIO_OUT_H

#include <stddef.h>
#include <stdint.h>

/*
 * The interrupt system, which the audio device needs in order to install its
 * DMA completion handler.
 *
 * Named as a class in C++ and as an opaque structure in C, so that this header
 * can be included from both: src/port/sound_bridge.c is C, and everything else
 * that includes it is C++.  The C branch needs the typedef as well as the
 * struct declaration, because in C a struct tag is not a type name.
 */
#ifdef __cplusplus
class CInterruptSystem;
#else
struct CInterruptSystem;
typedef struct CInterruptSystem CInterruptSystem;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_sink audio_sink_t;

struct audio_sink {
	/*
	 * Opens the output device for the given stream shape.
	 *
	 *   sample_rate    samples per second, per channel
	 *   channels       1 (mono) or 2 (stereo)
	 *   sample_size    bytes per sample: 2 for signed 16-bit
	 *
	 * Every sample is a signed 16-bit little-endian integer when sample_size
	 * is 2.  Returns the number of frames the device buffer holds, or 0 if
	 * the device could not be opened.
	 */
	unsigned (*open)(audio_sink_t *self, unsigned sample_rate,
	                 unsigned channels, unsigned sample_size);

	/* Stops and releases the device.  Safe to call when not open. */
	void (*close)(audio_sink_t *self);

	/*
	 * Returns how many bytes can be accepted right now without blocking.
	 * Returning 0 means the device is full.
	 */
	unsigned (*free_bytes)(audio_sink_t *self);

	/*
	 * Writes COUNT bytes of PCM and returns how many were accepted.  Never
	 * blocks.  A short return means the rest was dropped.
	 */
	unsigned (*write)(audio_sink_t *self, const void *data, unsigned count);

	/*
	 * Writes COUNT bytes of digital silence.  Used when a write was short, so
	 * that the corresponding part of the hardware buffer holds silence rather
	 * than stale samples.
	 */
	void (*pad_silence)(audio_sink_t *self, unsigned count);

	/* Implementation private data, handed back as SELF. */
	void *impl;
};

/*
 * The kernel's factory for the audio device.
 *
 * Implemented in src/audio/audio_out.cpp.  Returns NULL if the device cannot be
 * created at all; the emulator then runs silently rather than refusing to
 * start.  This declaration is inside the extern "C" block above, so the symbol
 * name does not depend on which translation unit defines it.
 */
audio_sink_t *ATARI_CreateAudioSink (CInterruptSystem *pInterrupt);

/*
 * Silences the output, or lets it speak again.
 *
 * Muted, the sink still accepts every byte the emulator offers and puts it in
 * the hardware queue as silence instead of as sound.  That is deliberate: the
 * emulated machine has to keep producing samples at the same rate or the DMA
 * runs dry, and a queue that emptied would make the sound stutter for a second
 * after it came back.  Nothing above this call changes -- the emulator, the
 * sound bridge and the pacing all behave as if the output were audible.
 *
 * Called from the main thread, once per frame; safe to call when the sink is
 * closed or not yet created.
 */
void ATARI_AudioSinkSetMuted (audio_sink_t *sink, int muted);

#ifdef __cplusplus
}
#endif

#endif /* ATARI_PI_AUDIO_OUT_H */

