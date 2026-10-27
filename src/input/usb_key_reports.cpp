/*
 * usb_key_reports.cpp -- see usb_key_reports.h.
 *
 * The shape of this file is dictated by one detail of Circle's API: the raw
 * key-status handler is a plain function pointer with no context argument, so it
 * cannot say which keyboard a report came from.  The device is registered with a
 * per-slot trampoline instead -- the same trick the keyboard driver used before
 * it, kept because it is the only way to attribute a report to a device.
 *
 * Removing a keyboard is reported too, and matters: without it a listener's last
 * bits would survive an unplug, and a joystick would stay held down.
 */

#include "usb_key_reports.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/usb/usbkeyboard.h>

static const char FromReports[] = "usbreports";

CUSBKeyReports *CUSBKeyReports::s_pThis = 0;

CUSBKeyReports *CUSBKeyReports::Get (void)
{
	static CUSBKeyReports Instance;

	return &Instance;
}

CUSBKeyReports::CUSBKeyReports (void)
:	m_nListeners (0),
	m_nDeviceListeners (0),
	m_nDevices (0)
{
	for (unsigned i = 0; i < USB_KEY_REPORTS_MAX_DEVICES; i++)
	{
		m_Slot[i].pKeyboard = 0;
		m_Slot[i].bActive = FALSE;
	}

	for (unsigned i = 0; i < USB_KEY_REPORTS_MAX_LISTENERS; i++)
	{
		m_Listener[i].pListener = 0;
		m_Listener[i].pDeviceListener = 0;
		m_Listener[i].pContext = 0;
	}

	s_pThis = this;
}

boolean CUSBKeyReports::AddListener (TUsbKeyReportListener pListener,
				     void *pContext, const char *pName)
{
	if (pListener == 0 || m_nListeners >= USB_KEY_REPORTS_MAX_LISTENERS)
	{
		CLogger::Get ()->Write (FromReports, LogWarning,
					"%s: no room for another key listener",
					pName != 0 ? pName : "?");

		return FALSE;
	}

	m_Lock.Acquire ();

	m_Listener[m_nListeners].pListener = pListener;
	m_Listener[m_nListeners].pDeviceListener = 0;
	m_Listener[m_nListeners].pContext = pContext;
	m_nListeners++;

	m_Lock.Release ();

	return TRUE;
}

boolean CUSBKeyReports::AddDeviceListener (TUsbKeyDeviceListener pListener,
					   void *pContext, const char *pName)
{
	if (pListener == 0 || m_nListeners >= USB_KEY_REPORTS_MAX_LISTENERS)
	{
		CLogger::Get ()->Write (FromReports, LogWarning,
					"%s: no room for another device listener",
					pName != 0 ? pName : "?");

		return FALSE;
	}

	m_Lock.Acquire ();

	m_Listener[m_nListeners].pListener = 0;
	m_Listener[m_nListeners].pDeviceListener = pListener;
	m_Listener[m_nListeners].pContext = pContext;
	m_nListeners++;

	m_Lock.Release ();

	return TRUE;
}

/*
 * Removes the listener whose context is PContext, keeping the array dense.
 *
 * A listener may remove itself from the code that is being called through it, so
 * the list is only touched under the lock: after this returns, no further report
 * reaches it.
 */
void CUSBKeyReports::RemoveListener (void *pContext)
{
	m_Lock.Acquire ();

	for (unsigned i = 0; i < m_nListeners; i++)
	{
		if (m_Listener[i].pContext == pContext && m_Listener[i].pListener != 0)
		{
			for (unsigned j = i + 1; j < m_nListeners; j++)
			{
				m_Listener[j - 1] = m_Listener[j];
			}

			m_nListeners--;

			break;
		}
	}

	m_Lock.Release ();
}

void CUSBKeyReports::RemoveDeviceListener (void *pContext)
{
	m_Lock.Acquire ();

	for (unsigned i = 0; i < m_nListeners; i++)
	{
		if (m_Listener[i].pContext == pContext
		    && m_Listener[i].pDeviceListener != 0)
		{
			for (unsigned j = i + 1; j < m_nListeners; j++)
			{
				m_Listener[j - 1] = m_Listener[j];
			}

			m_nListeners--;

			break;
		}
	}

	m_Lock.Release ();
}

void CUSBKeyReports::NotifyDevice (unsigned nSlot, boolean bPresent)
{
	m_Lock.Acquire ();

	for (unsigned i = 0; i < m_nListeners; i++)
	{
		if (m_Listener[i].pDeviceListener != 0)
		{
			m_Listener[i].pDeviceListener (m_Listener[i].pContext,
						       nSlot, bPresent);
		}
	}

	m_Lock.Release ();
}

void CUSBKeyReports::ClearSlot (unsigned nSlot, boolean bTellListeners)
{
	if (nSlot >= USB_KEY_REPORTS_MAX_DEVICES || !m_Slot[nSlot].bActive)
	{
		return;
	}

	m_Slot[nSlot].pKeyboard = 0;
	m_Slot[nSlot].bActive = FALSE;

	if (m_nDevices > 0)
	{
		m_nDevices--;
	}

	/*
	 * Tell every listener, with no keys held: whatever it was showing for this
	 * keyboard is over.  A joystick that stayed held after an unplug could not
	 * be released by the player without resetting the machine.
	 */
	if (bTellListeners)
	{
		static const unsigned char None[6] = { 0, 0, 0, 0, 0, 0 };

		for (unsigned i = 0; i < m_nListeners; i++)
		{
			if (m_Listener[i].pListener != 0)
			{
				m_Listener[i].pListener (m_Listener[i].pContext,
							 nSlot, 0, None);
			}
		}

		NotifyDevice (nSlot, FALSE);
	}
}

void CUSBKeyReports::Handler (unsigned nSlot, unsigned char ucModifiers,
			      const unsigned char RawKeys[6])
{
	CUSBKeyReports *pThis = s_pThis;
	if (pThis == 0 || nSlot >= USB_KEY_REPORTS_MAX_DEVICES)
	{
		return;
	}

	/* Copied, because a listener may only read what it was given. */
	unsigned char Keys[6];
	for (unsigned i = 0; i < 6; i++)
	{
		Keys[i] = RawKeys[i];
	}

	pThis->m_Lock.Acquire ();

	for (unsigned i = 0; i < pThis->m_nListeners; i++)
	{
		if (pThis->m_Listener[i].pListener != 0)
		{
			pThis->m_Listener[i].pListener (pThis->m_Listener[i].pContext,
							nSlot, ucModifiers, Keys);
		}
	}

	pThis->m_Lock.Release ();
}

/*
 * One trampoline per slot: the raw handler is called with no context, so the slot
 * has to be baked into the function that is registered.
 */
#define USB_KEY_REPORTS_TRAMPOLINE(n)						\
	static void Trampoline##n (unsigned char ucModifiers,			\
				   const unsigned char RawKeys[6])		\
	{									\
		CUSBKeyReports::Handler (n, ucModifiers, RawKeys);		\
	}

USB_KEY_REPORTS_TRAMPOLINE (0)
USB_KEY_REPORTS_TRAMPOLINE (1)
USB_KEY_REPORTS_TRAMPOLINE (2)
USB_KEY_REPORTS_TRAMPOLINE (3)

TKeyStatusHandlerRaw *CUSBKeyReports::s_pTrampoline[USB_KEY_REPORTS_MAX_DEVICES] =
{
	Trampoline0, Trampoline1, Trampoline2, Trampoline3
};

void CUSBKeyReports::Update (void)
{
	/*
	 * Drop the slots whose keyboard has gone.  The device handle is only used
	 * to find our own slot again, so a stale pointer is compared, never
	 * dereferenced.
	 */
	for (unsigned nSlot = 0; nSlot < USB_KEY_REPORTS_MAX_DEVICES; nSlot++)
	{
		if (!m_Slot[nSlot].bActive)
		{
			continue;
		}

		boolean bStillThere = FALSE;

		for (unsigned nIndex = 1; nIndex <= USB_KEY_REPORTS_MAX_DEVICES; nIndex++)
		{
			CDevice *pDevice = CDeviceNameService::Get ()
				->GetDevice ("ukbd", nIndex, FALSE);

			if (pDevice != 0 && pDevice == (CDevice *) m_Slot[nSlot].pKeyboard)
			{
				bStillThere = TRUE;
				break;
			}
		}

		if (!bStillThere)
		{
			ClearSlot (nSlot, TRUE);
		}
	}

	/*
	 * Take on anything new.  A keyboard that is already in a slot is skipped by
	 * comparing the device, not by its index, because the indices shift when one
	 * is unplugged.
	 */
	for (unsigned nIndex = 1; nIndex <= USB_KEY_REPORTS_MAX_DEVICES; nIndex++)
	{
		CDevice *pDevice = CDeviceNameService::Get ()
			->GetDevice ("ukbd", nIndex, FALSE);
		if (pDevice == 0)
		{
			continue;
		}

		boolean bKnown = FALSE;
		for (unsigned nSlot = 0; nSlot < USB_KEY_REPORTS_MAX_DEVICES; nSlot++)
		{
			if (m_Slot[nSlot].bActive && m_Slot[nSlot].pKeyboard == pDevice)
			{
				bKnown = TRUE;
				break;
			}
		}

		if (bKnown)
		{
			continue;
		}

		unsigned nFree = USB_KEY_REPORTS_MAX_DEVICES;
		for (unsigned nSlot = 0; nSlot < USB_KEY_REPORTS_MAX_DEVICES; nSlot++)
		{
			if (!m_Slot[nSlot].bActive)
			{
				nFree = nSlot;
				break;
			}
		}

		if (nFree == USB_KEY_REPORTS_MAX_DEVICES)
		{
			// More keyboards than slots: the extra ones contribute nothing.
			continue;
		}

		CUSBKeyboardDevice *pKeyboard = (CUSBKeyboardDevice *) pDevice;

		m_Slot[nFree].pKeyboard = pKeyboard;
		m_Slot[nFree].bActive = TRUE;
		m_nDevices++;

		NotifyDevice (nFree, TRUE);

		/*
		 * Raw mode: HID usage codes are what usbkeys.cfg is written in, and a
		 * report arrives on every change instead of as characters.  This is the
		 * device's only raw handler -- the keyboard driver is a listener here
		 * now, not the owner.
		 */
		pKeyboard->RegisterKeyStatusHandlerRaw (s_pTrampoline[nFree], FALSE);
	}
}
