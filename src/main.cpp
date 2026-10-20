//
// main.cpp
//
// The entry point Circle calls after it has set the machine up.  It does
// nothing but create the kernel and hand over; everything else is in
// kernel.cpp.
//
#include "kernel.h"

#include <circle/startup.h>

int main (void)
{
	/*
	 * The kernel object lives in static storage, not on the stack.
	 *
	 * It is large -- roughly 70 KB, most of it the emulated machine's state and
	 * the buffers that go with it -- and Circle gives main() a stack of 128 KB
	 * (KERNEL_STACK_SIZE).  That leaves about 58 KB for everything main() calls,
	 * and the deepest paths are exactly the ones that need stack: a disk image
	 * read goes through FatFs and the USB driver, and the menu walks
	 * directories.
	 *
	 * Running out of stack is not reported by anything.  It silently overwrites
	 * whatever lies below the stack, which surfaces as an unexplained hang, or
	 * as a scrambled exception dump, depending on what happened to be there.
	 * That is a very poor failure mode for a fault this easy to remove.
	 *
	 * Declared static inside main rather than at file scope so that the
	 * constructor runs at the point of use, with Circle fully initialised, and
	 * so that it costs .bss rather than data.  There is only ever one kernel
	 * object and it is never destroyed, so nothing is lost by not unwinding it.
	 */
	static CAtariKernel Kernel;

	if (!Kernel.Initialize ())
	{
		// A failed start-up has already been explained on the screen and in
		// the log; there is nothing useful left to do.
		halt ();
		return EXIT_HALT;
	}

	TShutdownMode ShutdownMode = Kernel.Run ();

	switch (ShutdownMode)
	{
	case ShutdownReboot:
		reboot ();
		return EXIT_REBOOT;

	case ShutdownHalt:
	default:
		halt ();
		return EXIT_HALT;
	}
}
