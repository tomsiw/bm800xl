/*
 * led_activity.cpp -- see led_activity.h.
 *
 * The LED belongs to CActLED, which knows how each board wires it; this file
 * only decides when to ask it for what.  It was driven by the SD driver's own
 * blinking before, which is why the LED only ever meant "the card is being
 * read"; the resting state now is ON.
 *
 * Timing is counted in FRAMES, not milliseconds.  A millisecond clock was tried
 * first and its ticks stood still, so the grace period never expired and the LED
 * stayed dark from the first file access onwards.  A frame counter cannot fail
 * that way, and the frame loop is what the LED is describing anyway.
 *
 * There is deliberately no logging here.  Every line of a log is a write to the
 * card, a write to the card is file access, and file access is what this file
 * reacts to -- the two fed each other, buried the card in lines and starved the
 * emulation.  Nothing in this file may write to a file.
 */

#include "led_activity.h"

#include <circle/actled.h>
#include <circle/sched/task.h>
#include <circle/timer.h>
#include <circle/types.h>

/*
 * How many frames after the last file operation the LED comes back on.
 *
 * A file read is a burst of block transfers, not one; without a grace period the
 * LED would flicker inside every burst instead of going dark once.  Six frames is
 * about 120 ms at 50 fps.
 */
static const unsigned LED_IDLE_FRAMES = 6;

/*
 * While a load continues, the LED blinks slowly rather than staying dark.
 *
 * A load that reads one sector per emulated transfer -- a disk image in normal
 * ROM mode -- never leaves a six-frame gap, so "off while busy" would mean dark
 * for the whole load, indistinguishable from a machine that has hung.
 */
static const unsigned LED_ACTIVE_BLINK_FRAMES = 25;
static const unsigned LED_ACTIVE_ON_FRAMES = 3;

/* How often the idle state is put back on the pin, in frames. */
static const unsigned LED_REASSERT_FRAMES = 6;

/* Half of the error blink's period: 500 ms on, 500 ms off. */
static const unsigned LED_ERROR_HALF_MS = 500;

static CActLED *s_pLED;

/* TRUE once an error blink owns the LED: the other states must then leave it. */
static volatile boolean s_bError;

/* TRUE while file access has the LED dark. */
static volatile boolean s_bAccess;

/* Frames since the last file operation. */
static unsigned s_nSinceActivity;

/* Frames spent in the current burst, for the slow blink while it lasts. */
static unsigned s_nAccessFrames;

/* Frames since the idle state was last put on the pin. */
static unsigned s_nSinceOn;


static CActLED *LED (void)
{
	if (s_pLED == 0)
	{
		s_pLED = CActLED::Get ();
	}

	return s_pLED;
}

/*
 * The error blink.
 *
 * A thread rather than a frame-loop counter, because the failures worth blinking
 * for are the ones after which the frame loop no longer runs.
 */
class CLedErrorTask : public CTask
{
public:
	CLedErrorTask (void)
	:	CTask (2048, FALSE)	/* 2048 bytes, taken from the heap */
	{
	}

	void Run (void)
	{
		boolean bOn = FALSE;

		for (;;)
		{
			CTimer::SimpleMsDelay (LED_ERROR_HALF_MS);

			CActLED *pLED = LED ();
			if (pLED == 0)
			{
				continue;
			}

			bOn = bOn ? FALSE : TRUE;

			if (bOn)
			{
				pLED->On ();
			}
			else
			{
				pLED->Off ();
			}
		}
	}
};

void ATARI_LED_Start (void)
{
	CActLED *pLED = LED ();
	if (pLED == 0)
	{
		return;
	}

	/* Lit from the start.  Nothing here blocks, and nothing blinks. */
	pLED->On ();
}

void ATARI_LED_Activity (void)
{
	if (s_bError)
	{
		return;
	}

	s_nSinceActivity = 0;

	CActLED *pLED = LED ();
	if (pLED == 0)
	{
		return;
	}

	pLED->Off ();

	if (!s_bAccess)
	{
		s_nAccessFrames = 0;
	}

	s_bAccess = TRUE;
}

void ATARI_LED_Poll (void)
{
	if (s_bError)
	{
		return;
	}

	if (s_bAccess)
	{
		if (++s_nSinceActivity >= LED_IDLE_FRAMES)
		{
			s_bAccess = FALSE;
			s_nSinceActivity = 0;

			CActLED *pLED = LED ();
			if (pLED != 0)
			{
				pLED->On ();
			}

			s_nAccessFrames = 0;
		}
		else
		{
			/*
			 * The burst continues: blink slowly so a long load looks like work
			 * rather than like a hung machine.
			 */
			unsigned nPhase = ++s_nAccessFrames % LED_ACTIVE_BLINK_FRAMES;

			CActLED *pLED = LED ();
			if (pLED != 0)
			{
				if (nPhase == 0)
				{
					pLED->On ();
				}
				else if (nPhase == LED_ACTIVE_ON_FRAMES)
				{
					pLED->Off ();
				}
			}
		}

		return;
	}

	s_nSinceActivity = 0;

	/*
	 * Idle: put the state back on the pin every few frames.
	 *
	 * The LED is not ours alone -- something in the start-up sequence turns it
	 * off a few seconds in and leaves it dark -- and putting the idle state back
	 * is what recovers from that.  One register write, ten times a second.
	 */
	if (++s_nSinceOn >= LED_REASSERT_FRAMES)
	{
		s_nSinceOn = 0;

		CActLED *pLED = LED ();
		if (pLED != 0)
		{
			pLED->On ();
		}
	}
}

void ATARI_LED_Error (void)
{
	if (s_bError)
	{
		return;
	}

	s_bError = TRUE;

	CActLED *pLED = LED ();
	if (pLED != 0)
	{
		pLED->Off ();
	}

	/*
	 * Started rather than run: this call happens in whatever context found the
	 * failure -- often the frame loop, sometimes a driver's start-up -- and none
	 * of them can afford to sit in a blink loop.
	 */
	static CLedErrorTask *s_pTask = 0;
	if (s_pTask == 0)
	{
		/* Constructing it is enough: the task starts itself. */
		s_pTask = new CLedErrorTask;
	}
}
