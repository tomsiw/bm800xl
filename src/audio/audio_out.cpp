//
// audio_out.cpp
//
// HDMI audio output for the bare-metal Atari XL/XE emulator: the kernel side of
// the interface in src/port/audio_out.h.
//
// The emulation core produces signed 16-bit PCM, one emulated frame at a time,
// and Circle's CHDMISoundBaseDevice carries it out over the HDMI link.  Three
// things are worth knowing before reading the code:
//
//  1. HDMI audio is a sample stream, not a duty cycle.  The device declares
//     SoundFormatIEC958 and takes one signed 32-bit sample per channel per word,
//     building the S/PDIF subframes itself.  Each 16-bit sample is therefore
//     written left-justified in the top half of a word, and digital silence is
//     zero.  (The PWM path this driver used to take needed mid-scale duty values
//     instead; that distinction is why this code looks the way it does.)
//
//  2. The samples travel through a ring buffer of this driver's own.  GetChunk()
//     runs in the DMA interrupt handler, where nothing may allocate, log or
//     wait, and the emulator calls write() from the main thread, so the ring is
//     the only meeting point between the two.  It holds the stream exactly as
//     the emulator produces it; GetChunk() left-justifies each sample and, for a
//     mono stream, feeds the same sample to both output channels.
//
//  3. The sample rate and chunk size are fixed in CHDMISoundBaseDevice's
//     constructor, and the chunk must be a whole number of IEC958 blocks (384
//     words).  A different stream shape therefore needs a different object: the
//     device is constructed into static storage on open() and destroyed again on
//     close(), never reconfigured in place.
//
#include "../port/audio_out.h"

#include <circle/interrupt.h>
#include <circle/new.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/util.h>

#include <assert.h>

// 16 384 samples, that is 32 KB of PCM.  At 48 kHz - the highest rate the
// platform layer in src/port/sound_bridge.c accepts - the ring holds 170 ms of
// a stereo stream or 341 ms of a mono one, and it is still more than the 100 ms
// the stream is sized for at the core's 65 535 Hz ceiling.
enum
{
	AudioRingSamples	= 16384,
	AudioChunkWords		= 1920,		// 32-bit words per DMA chunk
	AudioQueueMsecs		= 100,		// output buffer reported to the emulator
	AudioZeroChunkBytes	= 256,		// silence is fed in chunks of this size
	AudioCancelTimeout	= CLOCKHZ	// how long close() waits for the DMA
};

// The ring positions are wrapped by masking, not by comparing, so the size has
// to be a power of two.
ASSERT_STATIC ((AudioRingSamples & (AudioRingSamples - 1)) == 0);

namespace
{

class CAtariAudioDevice : public CHDMISoundBaseDevice
{
public:
	CAtariAudioDevice (CInterruptSystem *pInterrupt, unsigned nSampleRate,
			   unsigned nChannels);

	/// \return Frames of the emulated stream the ring buffer can hold
	unsigned GetRingFrames (void) const	{ return AudioRingSamples / m_nChannels; }

	/// \return Stream bytes that write() accepts right now
	unsigned FreeBytes (void);

	/// \param pData Stream samples (signed 16-bit, interleaved)
	/// \param nCount Size in bytes
	/// \return Number of bytes taken, which is a whole number of frames
	unsigned WriteStream (const void *pData, unsigned nCount);

	/// \brief Pushes COUNT bytes of digital silence
	/// \note Stops early when the ring is full; never waits
	void PadSilence (unsigned nCount);

protected:
	/// \brief Fills one DMA chunk with samples, or with digital silence if

private:

private:
	s16		m_Ring[AudioRingSamples];
	volatile unsigned m_nIn;	// next sample the producer writes
	volatile unsigned m_nOut;	// next sample GetChunk() consumes
	unsigned	m_nChannels;	// channels of the emulated stream: 1 or 2
	u32		m_nSilence;
	boolean		m_bSwapChannels;
};

CAtariAudioDevice::CAtariAudioDevice (CInterruptSystem *pInterrupt, unsigned nSampleRate,
				      unsigned nChannels)
:	CHDMISoundBaseDevice (pInterrupt, nSampleRate, AudioChunkWords),
	m_nIn (0),
	m_nOut (0),
	m_nChannels (nChannels)
{
	/*
	 * CHDMISoundBaseDevice produces SoundFormatIEC958: it takes one signed
	 * 32-bit sample per channel per word and builds the S/PDIF subframes itself.
	 * GetChunk() therefore writes each 16-bit sample left-justified in the top
	 * half of a word.
	 *
	 * Silence is zero here, not a mid-scale duty value.  The PWM's "average the
	 * duty cycle to 1.5 V" has no analogue in a sample stream: zero is silence,
	 * and a mid-scale value would be a loud DC offset.
	 */
	m_nSilence = 0;
	m_bSwapChannels = FALSE;

	/*
	 * Start from silence.
	 *
	 * The object is constructed in place in s_DeviceMemory, so the first open
	 * happens to see zeros, but reopening the sink constructs a new device over
	 * the same memory -- and m_Ring is not part of the initialiser list above.
	 * Without this, the first chunks after a reopen play whatever the previous
	 * session left in the ring.
	 */
	memset (m_Ring, 0, sizeof m_Ring);
}

//
// Producer side, called from the emulation loop
//

/*
 * These hand the stream to Circle's own queue instead of to a ring of ours.
 *
 * The class used to override GetChunk(), which meant GetChunkInternal() -- the
 * only place that inserts IEC958 subframe framing (control channel, parity,
 * preamble) -- was never called, and the DMA was given raw PCM where the
 * hardware expects framed S/PDIF subframes.  That is audible as distortion that
 * no amount of rate, buffer or framing tuning can remove.
 *
 * Without the override, the base class drains its queue through that path and
 * encodes the stream itself, which is what the queue exists for.
 */
unsigned CAtariAudioDevice::FreeBytes (void)
{
	// Space left in Circle's queue, in whole frames.  A partial frame would break
	// the channel interleaving of everything written after it.
	unsigned nFrameSize = m_nChannels * sizeof (s16);
	unsigned nUsed = GetQueueFramesAvail ();
	unsigned nSize = GetQueueSizeFrames ();

	unsigned nFreeFrames = nSize > nUsed ? nSize - nUsed : 0;

	return nFreeFrames * nFrameSize;
}
unsigned CAtariAudioDevice::WriteStream (const void *pData, unsigned nCount)
{
	int nResult = Write (pData, nCount);

	return nResult > 0 ? (unsigned) nResult : 0;
}

void CAtariAudioDevice::PadSilence (unsigned nCount)
{
	// All-zero PCM is digital silence: in a sample stream zero is the absence of
	// signal, so the padding is simply zeroed samples.  The chunks are bounded so
	// that a caller offering more than fits is not delayed, and the loop stops as
	// soon as the queue is full.
	static const u8 ZeroPCM[AudioZeroChunkBytes] = { };

	unsigned nMaxChunk = (unsigned) sizeof ZeroPCM;

	while (nCount > 0)
	{
		unsigned nChunk = nCount < nMaxChunk ? nCount : nMaxChunk;

		unsigned nWritten = WriteStream (ZeroPCM, nChunk);
		if (nWritten == 0)
		{
			break;
		}

		nCount -= nWritten;
	}
}

//
// Consumer side, called from the DMA interrupt handler
//

//
// Lifetime
//
// There is exactly one device.  It is constructed in place when the stream is
// opened and destroyed again when it is closed, because the sample rate is a
// property of the object rather than of a call.
//

u8 s_DeviceMemory[sizeof (CAtariAudioDevice)]
	__attribute__ ((aligned (alignof (CAtariAudioDevice))));

CAtariAudioDevice *s_pDevice;		// 0 while the output is closed
CInterruptSystem *s_pInterrupt;		// handed over by the factory

CAtariAudioDevice *DeviceOf (audio_sink_t *pSelf)
{
	return static_cast<CAtariAudioDevice *> (pSelf->impl);
}

void SinkClose (audio_sink_t *pSelf)
{
	(void) pSelf;

	if (s_pDevice == 0)
	{
		return;			// safe to call when nothing is open
	}

	s_pDevice->Cancel ();

	// Cancel() takes effect after the chunk that is being played at the time,
	// and the device's destructor insists that the DMA has really stopped, so
	// wait for it.  On the RP1 (Pi 5) variant Cancel() is immediate and this
	// loop runs once.
	unsigned nStart = CTimer::GetClockTicks ();
	while (   s_pDevice->IsActive ()
	       && CTimer::GetClockTicks () - nStart < AudioCancelTimeout)
	{
		// Wait for the DMA to finish; one chunk is a few dozen milliseconds.
	}

	s_pDevice->~CAtariAudioDevice ();
	s_pDevice = 0;
}

unsigned SinkOpen (audio_sink_t *pSelf, unsigned nSampleRate,
		   unsigned nChannels, unsigned nSampleSize)
{
	// Signed 16-bit PCM, mono or stereo, that is all this device takes.
	if (s_pInterrupt == 0 || nSampleRate == 0 || nSampleSize != 2
	    || (nChannels != 1 && nChannels != 2))
	{
		return 0;
	}

	// Opening an open device replaces it: the sample rate and the channel
	// count are compiled into the hardware configuration.
	SinkClose (pSelf);

	s_pDevice = new (s_DeviceMemory) CAtariAudioDevice (s_pInterrupt, nSampleRate,
							    nChannels);

	// The queue is what defines the buffer length the emulator is told about;
	// the samples themselves go through the ring buffer above.
	if (!s_pDevice->AllocateQueue (AudioQueueMsecs))
	{
		SinkClose (pSelf);
		return 0;
	}

	s_pDevice->SetWriteFormat (SoundFormatSigned16, nChannels);

	if (!s_pDevice->Start ())
	{
		SinkClose (pSelf);
		return 0;
	}

	// Report the shorter of the two buffers: the emulator must not be led to
	// believe it can hand over more than the ring buffer will hold.
	unsigned nFrames = s_pDevice->GetQueueSizeFrames ();
	unsigned nRingFrames = s_pDevice->GetRingFrames ();
	if (nFrames > nRingFrames)
	{
		nFrames = nRingFrames;
	}

	return nFrames;
}

unsigned SinkFreeBytes (audio_sink_t *pSelf)
{
	return s_pDevice != 0 ? DeviceOf (pSelf)->FreeBytes () : 0;
}

unsigned SinkWrite (audio_sink_t *pSelf, const void *pData, unsigned nCount)
{
	if (s_pDevice == 0 || pData == 0 || nCount == 0)
	{
		return 0;
	}

	return DeviceOf (pSelf)->WriteStream (pData, nCount);
}

void SinkPadSilence (audio_sink_t *pSelf, unsigned nCount)
{
	if (s_pDevice == 0 || nCount == 0)
	{
		return;
	}

	DeviceOf (pSelf)->PadSilence (nCount);
}

// The sink the kernel installs.  It lives in static storage, as does the
// device its impl field points at.
audio_sink_t s_Sink =
{
	SinkOpen,
	SinkClose,
	SinkFreeBytes,
	SinkWrite,
	SinkPadSilence,
	s_DeviceMemory
};

}	// namespace

extern "C" audio_sink_t *ATARI_CreateAudioSink (CInterruptSystem *pInterrupt)
{
	if (pInterrupt == 0)
	{
		// The PWM device hands chunks to a completion handler that runs from
		// the interrupt system, so without one there is no way to supply
		// samples at all.
		return 0;
	}

	s_pInterrupt = pInterrupt;

	return &s_Sink;
}
