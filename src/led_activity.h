/*
 * led_activity.h -- what the board's activity LED says.
 *
 * Three states, and only one of them is invisible:
 *
 *   IDLE    on          the machine is alive and not reading the card
 *   ACCESS  off         a file is being read or written
 *   ERROR   1 Hz blink  something failed and the picture may be gone
 *
 * The LED is the one indicator that still works when the framebuffer does not,
 * so the error blink is driven by its own thread rather than by the frame loop:
 * the interesting failures are the ones that stop the frame loop.
 *
 * Circle's SD driver would otherwise blink the LED itself around every block
 * transfer, which is the behaviour this replaces -- see the CEMMCDevice
 * construction in kernel.cpp, which passes no LED for that reason.
 */

#ifndef ATARI_PI_LED_ACTIVITY_H
#define ATARI_PI_LED_ACTIVITY_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lit, from the first frame.
 *
 * "Lit" is the resting state: an unlit LED means something is happening, not
 * that nothing is.  Nothing is blinked at start-up -- a pattern there would say
 * nothing that is not already visible on the screen, and the one Circle offers
 * blocks for over a second.
 */
void ATARI_LED_Start (void);

/*
 * A file operation is starting or finishing.
 *
 * Called from the file system layer, in task context, on every open, read,
 * write, close and directory read.  The LED goes off at once; it comes back on
 * from ATARI_LED_Poll() once the burst has stopped.
 */
void ATARI_LED_Activity (void);

/* Called once per frame: brings the LED back on after a burst of file I/O. */
void ATARI_LED_Poll (void);

/*
 * Something failed.
 *
 * Starts the 1 Hz blink, once, and leaves it running: an error is not expected
 * to be recovered from.  Safe to call more than once.
 */
void ATARI_LED_Error (void);

#ifdef __cplusplus
}
#endif

#endif
