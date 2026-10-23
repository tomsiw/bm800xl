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
//  2. The samples travel through Circle's own queue.  They are written with
//     Write() from the emulation loop, and the base class drains that queue in
//     GetChunk(), which runs in the DMA interrupt handler -- a place where
//     nothing may allocate, log or wait.  The queue is therefore the only
//     meeting point between the two threads, and it is also what performs the
//     IEC958 framing.  Left to itself the queue holds the stream exactly as the
//     emulator produces it; for a mono stream the same sample goes to both
//     output channels.
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

// Bytes of digital silence written at a time when the stream runs dry.
enum
{
	AudioChunkWords		= 1920,		// 32-bit words per DMA chunk
	AudioQueueMsecs		= 100,		// size of Circle's output queue
	AudioZeroChunkBytes	= 256,		// silence is fed in chunks of this size
	AudioCancelTimeout	= CLOCKHZ	// how long close() waits for the DMA
};

namespace
{

class CAtariAudioDevice : public CHDMISoundBaseDevice
{
public:
	CAtariAudioDevice (CInterruptSystem *pInterrupt, unsigned nSampleRate,
			   unsigned nChannels);

	/// \return Stream bytes that write() accepts right now
	unsigned FreeBytes (void);

	/// \param pData Stream samples (signed 16-bit, interleaved)
	/// \param nCount Size in bytes
	/// \return Number of bytes taken, which is a whole number of frames
	unsigned WriteStream (const void *pData, unsigned nCount);

	/// \brief Pushes COUNT bytes of digital silence
	/// \note Stops early when the queue is full; never waits
	void PadSilence (unsigned nCount);

	/*
	 * Silences the output while still consuming the emulator's samples.
	 *
	 * Used while the OSD menu is open.  The samples are still taken at the rate
	 * the emulated machine produces them -- the queue must not run dry, or the
	 * sound that comes back is late and broken -- but what goes into it is
	 * silence instead of the Atari's output.
	 */
	void SetMuted (boolean bMuted)	{ m_bMuted = bMuted; }

private:
	/// \return Bytes of silence accepted, at most COUNT
	unsigned WriteSilence (unsigned nCount);

	unsigned	m_nChannels;	// channels of the emulated stream: 1 or 2
	u32		m_nSilence;
	boolean		m_bSwapChannels;
	boolean		m_bMuted;
};

CAtariAudioDevice::CAtariAudioDevice (CInterruptSystem *pInterrupt, unsigned nSampleRate,
				      unsigned nChannels)
:	CHDMISoundBaseDevice (pInterrupt, nSampleRate, AudioChunkWords),
	m_nChannels (nChannels)
{
	/*
	 * This class no longer overrides GetChunk().  CHDMISoundBaseDevice produces
	 * SoundFormatIEC958, and the base class's own GetChunk builds the S/PDIF
	 * subframes -- control channel, parity and preamble -- while draining the
	 * queue that Write() fills.  Overriding it bypassed exactly that encoding,
	 * which is what made the sound distorted.
	 *
	 * Silence is zero here, not a mid-scale duty value.  The PWM's "average the
	 * duty cycle to 1.5 V" has no analogue in a sample stream: zero is silence,
	 * and a mid-scale value would be a loud DC offset.
	 */
	m_nSilence = 0;
	m_bSwapChannels = FALSE;
	m_bMuted = FALSE;
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
	if (m_bMuted)
	{
		/*
		 * Silence of the same length, not a dropped write.
		 *
		 * The queue is what the emulator is paced against: a write that never
		 * arrived would leave the queue empty, the DMA would underrun, and the
		 * first second after the menu closes would stutter.  So the bytes are
		 * still taken, and only their contents are replaced.
		 */
		return WriteSilence (nCount);
	}

	int nResult = Write (pData, nCount);

	return nResult > 0 ? (unsigned) nResult : 0;
}

unsigned CAtariAudioDevice::WriteSilence (unsigned nCount)
{
	// Bounded chunks, like PadSilence(), so a caller offering more than fits is
	// not held up; the size is a whole number of frames for both channel counts.
	static const u8 ZeroPCM[AudioZeroChunkBytes] = { };

	unsigned nDone = 0;

	while (nDone < nCount)
	{
		unsigned nChunk = nCount - nDone;
		if (nChunk > sizeof ZeroPCM)
		{
			nChunk = (unsigned) sizeof ZeroPCM;
		}

		int nResult = Write (ZeroPCM, nChunk);
		if (nResult <= 0)
		{
			break;
		}

		nDone += (unsigned) nResult;
	}

	return nDone;
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

	// The queue is the buffer the emulator is told about, and the one the
	// samples themselves travel through.
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

	// Report the queue length: it is the only buffer between the core and the
	// hardware now that the ring is gone.
	return s_pDevice->GetQueueSizeFrames ();
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

extern "C" void ATARI_AudioSinkSetMuted (audio_sink_t *pSink, int bMuted)
{
	// Safe when nothing is open: the kernel calls this every frame from the
	// first one, which may be before the sound device exists.
	if (s_pDevice == 0 || pSink == 0)
	{
		return;
	}

	DeviceOf (pSink)->SetMuted (bMuted != 0);
}
