/*
 * sound_bridge.c -- the audio half of the platform layer.
 *
 * The emulation core produces POKEY audio through its own mixer and hands the
 * result to the platform as 16-bit signed PCM.  This file implements the
 * platform side of that contract (the PLATFORM_Sound* functions declared in
 * the core's platform.h) and forwards the samples to whatever audio device the
 * kernel has installed.
 *
 * The device itself lives in the kernel, so it is hidden behind a single
 * interface here.  That keeps the emulation core's timing decisions -- sample
 * rate, buffer length, mono or stereo -- with the core, while the mechanism for
 * making a noise stays with the hardware layer.
 *
 * The contract, quoting the core's platform.h:
 *
 *   - PLATFORM_SoundSetup() may adjust *setup to describe what the hardware can
 *     actually do, and returns whether output was opened.  Output starts
 *     paused; the caller then calls PLATFORM_SoundContinue().
 *   - PLATFORM_SoundAvailable() reports how much can be written without
 *     blocking.  If it returns at least one buffer's worth, the core believes
 *     an underrun has occurred.
 *   - PLATFORM_SoundWrite() accepts up to one buffer's worth of samples.
 */

#include "config.h"

#include <string.h>

#include "platform.h"
#include "sound.h"

/* For Log_print: a failed or unusual audio setup is otherwise entirely silent. */
#include "log.h"

#include "audio_out.h"

/*
 * The core's sound module keeps the PCM it has produced in a buffer described
 * by these globals, which upstream's library front end normally defines.  This
 * project does not compile that front end, so they live here: this file is what
 * feeds the samples to the audio device, so it is also the natural owner of the
 * buffer description.
 *
 * The buffer itself is allocated by the core in Sound_Setup().
 */
UBYTE *LIBATARI800_Sound_array;
unsigned int sound_array_fill;
unsigned int sound_hw_buffer_size;
double sample_residual;

/*
 * The device the kernel installed, and the shape of the stream it accepts.
 */
static audio_sink_t *s_sink;

/* Parameters the core negotiated with us. */
static unsigned s_sample_rate;
static unsigned s_channels;
static unsigned s_sample_size;		/* bytes per sample: 1 or 2 */
static unsigned s_buffer_bytes;		/* one full hardware buffer, in bytes */
static int s_running;
static int s_device_open;		/* the sink below is open and playing */

void ATARI_SetHostAudioSink(audio_sink_t *sink)
{
	s_sink = sink;
}

/*
 * Opens the audio device.
 *
 * The core asks for a buffer size in frames; we report what the device can
 * actually provide and let the core adapt, which is exactly what the contract
 * above is for.
 */
int PLATFORM_SoundSetup(Sound_setup_t *setup)
{
	unsigned frames;

	if (setup == 0) {
		return FALSE;
	}

	/* POKEY's mixer supports these formats; refuse anything else outright. */
	if (setup->sample_size != 1 && setup->sample_size != 2) {
		return FALSE;
	}
	if (setup->channels != 1 && setup->channels != 2) {
		return FALSE;
	}
	if (setup->freq < 8000 || setup->freq > 48000) {
		return FALSE;
	}

	if (s_sink == 0 || s_sink->open == 0) {
		/*
		 * No audio device is available.  Reporting failure is the correct
		 * behaviour: the core then sets Sound_enabled to FALSE and stops
		 * generating samples, so the emulation keeps running silently
		 * instead of stalling.
		 */
		s_running = 0;
		return FALSE;
	}

	/*
	 * Reuse the device that is already open when the shape has not changed.
	 *
	 * Opening the sink closes and rebuilds the Circle device behind it -- a new
	 * CHDMISoundBaseDevice, its DMA channel and its clock setup -- and
	 * reopening the HDMI sink on this hardware hangs.  A power cycle
	 * re-initialises the emulator, which asks for the sound setup again; with
	 * the same rate, channel count and sample size there is nothing to rebuild,
	 * and all the core wants back is the buffer size it was given.
	 */
	if (s_device_open
	    && setup->freq == s_sample_rate
	    && setup->channels == s_channels
	    && setup->sample_size == s_sample_size) {
		setup->buffer_frames = s_buffer_bytes / (s_channels * s_sample_size);
		s_running = 0;		/* output starts paused, as on a new open */

		Log_print("Audio: reusing the open device (%u Hz, %u channel(s))",
		          s_sample_rate, s_channels);
		return TRUE;
	}

	frames = s_sink->open(s_sink, setup->freq, setup->channels, setup->sample_size);

	if (frames == 0) {
		s_running = 0;

		/*
		 * Say what was asked for.  A failed open is otherwise completely
		 * silent: the core sets Sound_enabled to FALSE and the machine runs on
		 * without a sound, which is exactly what a working-but-unheard audio
		 * device looks like from the outside.
		 */
		Log_print("Audio: the device refused %u Hz, %u channel(s), %u-byte samples",
		          setup->freq, setup->channels, setup->sample_size);
		return FALSE;
	}

	s_sample_rate = setup->freq;
	s_channels = setup->channels;
	s_sample_size = setup->sample_size;
	s_buffer_bytes = frames * s_channels * s_sample_size;

	/* Tell the core the buffer size we actually obtained. */
	setup->buffer_frames = frames;

	s_running = 0;			/* output starts paused */

	/*
	 * The negotiated shape, recorded so that a silent machine can be told from
	 * a machine whose audio went somewhere unexpected.  The buffer figure is in
	 * frames and comes back from the device, so it also shows whether the
	 * hardware accepted the rate that was asked for.
	 */
	Log_print("Audio: opened %u Hz, %u channel(s), %u-byte samples, %u frames",
	          s_sample_rate, s_channels, s_sample_size, frames);

	s_device_open = 1;

	return TRUE;
}

void PLATFORM_SoundExit(void)
{
	if (s_sink != 0 && s_sink->close != 0) {
		s_sink->close(s_sink);
	}
	s_running = 0;
	s_device_open = 0;
}

void PLATFORM_SoundPause(void)
{
	s_running = 0;
}

void PLATFORM_SoundContinue(void)
{
	s_running = 1;
}

/*
 * How many bytes can be written right now without blocking.
 *
 * The core uses this to decide whether it is falling behind: if a whole buffer
 * is free it assumes the output underran and repeats the last sample frame, so
 * an honest answer matters more than a generous one.
 */
unsigned int PLATFORM_SoundAvailable(void)
{
	if (!s_running || s_sink == 0 || s_sink->free_bytes == 0) {
		return 0;
	}
	return s_sink->free_bytes(s_sink);
}

/*
 * Hands SIZE bytes of PCM to the device.
 *
 * Write() on the underlying device never blocks: it accepts what fits and
 * reports how much it took.  Samples that do not fit are dropped, which is the
 * right behaviour for a real-time audio stream -- delaying the emulation to
 * push them out would make the machine run slow.
 */
void PLATFORM_SoundWrite(UBYTE const *buffer, unsigned int size)
{
	unsigned int accepted;

	if (!s_running || s_sink == 0 || s_sink->write == 0 || size == 0) {
		return;
	}

	/* Never offer the device more than one buffer at a time. */
	if (s_buffer_bytes != 0 && size > s_buffer_bytes) {
		size = s_buffer_bytes;
	}

	accepted = s_sink->write(s_sink, buffer, size);

	/*
	 * If the device could not take everything, pad the remainder with silence
	 * rather than leaving a gap: an unwritten region of the DMA buffer would
	 * otherwise repeat whatever was there before and be audible as a click.
	 */
	if (accepted < size && s_sink->pad_silence != 0) {
		s_sink->pad_silence(s_sink, size - accepted);
	}
}
