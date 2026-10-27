/*
 * usb_key_reports.h -- the one place that listens to the USB keyboards.
 *
 * Circle's CUSBKeyboardDevice carries a *single* raw key-status handler: the
 * registration is a pointer in the device object, so a second driver that
 * registered would silently displace the first.  Two of this project's drivers
 * need those reports -- the Atari keyboard, and the keyboard joystick -- so
 * neither of them owns the device.  This module does: it finds each keyboard the
 * USB stack has registered, claims the one handler, and forwards every report to
 * whatever has registered as a listener.
 *
 * That keeps the two drivers independent of each other, which is the whole point:
 * the joystick resolves `Joy1_*` strokes for itself and knows nothing about
 * CUSBAtariKeyboard, and the keyboard knows nothing about joysticks -- a stroke
 * bound to a joystick action simply produces nothing there.
 *
 * Reports arrive from the USB interrupt, so a listener runs in interrupt context:
 * it must not block, allocate, or log.
 */

#ifndef ATARI_PI_USB_KEY_REPORTS_H
#define ATARI_PI_USB_KEY_REPORTS_H

#include <circle/spinlock.h>
#include <circle/types.h>
#include <circle/usb/usbkeyboard.h>

/* Keyboards tracked at once.  The extra ones would share no slots. */
#define USB_KEY_REPORTS_MAX_DEVICES	4

/* Listeners that can be attached.  Two are used; the rest is headroom. */
#define USB_KEY_REPORTS_MAX_LISTENERS	4

/*
 * Called for every report from every tracked keyboard.
 *
 * \param pContext   What was given with AddListener.
 * \param nSlot      Which keyboard it came from, so a listener can keep per
 *		     keyboard state.
 * \param ucModifiers The HID modifier byte.
 * \param RawKeys    Six HID usage codes, 0 for an empty slot.
 */
typedef void (*TUsbKeyReportListener) (void *pContext, unsigned nSlot,
				       unsigned char ucModifiers,
				       const unsigned char RawKeys[6]);

/*
 * Called when a keyboard starts or stops being read in a slot.
 *
 * A listener with per-slot state uses this to forget it.  The empty report that
 * a disappearing keyboard produces is enough to release keys, but not enough to
 * forget a slot entirely.
 */
typedef void (*TUsbKeyDeviceListener) (void *pContext, unsigned nSlot,
				      boolean bPresent);

class CUSBKeyReports
{
public:
	static CUSBKeyReports *Get (void);

	/*
	 * Picks up keyboards that have appeared, and forgets those that have gone.
	 *
	 * Called once per frame from task level.  Enumeration is asynchronous, so a
	 * keyboard plugged in later is picked up on the next call; a listener that
	 * cares is told when a keyboard it was reading disappears, so that its state
	 * does not stay stuck on the last report.
	 */
	void Update (void);

	/* \return FALSE when no room is left for another listener. */
	boolean AddListener (TUsbKeyReportListener pListener, void *pContext,
			     const char *pName);

	/* Called when a keyboard appears in a slot, and when it goes. */
	boolean AddDeviceListener (TUsbKeyDeviceListener pListener, void *pContext,
				   const char *pName);

	/*
	 * Taking a listener off again.
	 *
	 * A driver that is not in use should not be receiving reports: the keyboard
	 * joystick listens only while its interface is the selected one, and these
	 * are how it says so.  Both match on PContext, which each driver passes as
	 * its own object, and after they return no further report reaches it.
	 */
	void RemoveListener (void *pContext);
	void RemoveDeviceListener (void *pContext);

	/* \return How many keyboards are being listened to. */
	unsigned GetDeviceCount (void) const	{ return m_nDevices; }

public:
	/*
	 * Called by the per-slot trampolines below; public only because they are
	 * free functions, since a registered raw handler carries no context.
	 */
	static void Handler (unsigned nSlot, unsigned char ucModifiers,
			     const unsigned char RawKeys[6]);

private:
	CUSBKeyReports (void);

	/* Forgets one slot, telling the listeners it has gone. */
	void ClearSlot (unsigned nSlot, boolean bTellListeners);

	/* A device slot, and the keyboard it is reading. */
	struct TSlot
	{
		void		*pKeyboard;	/* CUSBKeyboardDevice * */
		boolean		bActive;
	};

	struct TListener
	{
		TUsbKeyReportListener	pListener;
		TUsbKeyDeviceListener	pDeviceListener;
		void			*pContext;
	};

	/* Tells the device listeners about a slot. */
	void NotifyDevice (unsigned nSlot, boolean bPresent);



	/*
	 * Guards the listener list, which task context changes and the USB
	 * interrupt reads.  Reports are dispatched holding it, so a listener
	 * removed here cannot be called after this returns.
	 */
	CSpinLock	m_Lock;

	TListener	m_Listener[USB_KEY_REPORTS_MAX_LISTENERS];
	unsigned	m_nListeners;
	unsigned	m_nDeviceListeners;

	TSlot		m_Slot[USB_KEY_REPORTS_MAX_DEVICES];
	unsigned	m_nDevices;

	/* One per slot: the raw handler carries no context of its own. */
	static TKeyStatusHandlerRaw *s_pTrampoline[USB_KEY_REPORTS_MAX_DEVICES];

	static CUSBKeyReports *s_pThis;
};

#endif
