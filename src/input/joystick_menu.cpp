/*
 * joystick_menu.cpp -- see joystick_menu.h.
 */

#include "joystick_menu.h"

#include "../port/atari_port.h"

static int KeyForDirection (unsigned nDirection)
{
	switch (nDirection)
	{
	case ATARI_JOY_UP:	return ATARI_KEY_UP;
	case ATARI_JOY_DOWN:	return ATARI_KEY_DOWN;
	case ATARI_JOY_LEFT:	return ATARI_KEY_LEFT;
	case ATARI_JOY_RIGHT:	return ATARI_KEY_RIGHT;
	default:		return ATARI_KEY_NONE;
	}
}

CAtariMenuJoystick::CAtariMenuJoystick (void)
:	m_bEnabled (FALSE),
	m_bActive (FALSE),
	m_nOwner (-1)
{
	m_pHandle[0] = 0;
	m_pHandle[1] = 0;

	m_nHead = 0;
	m_nTail = 0;

	Reset ();
}

void CAtariMenuJoystick::SetHandles (atari_joystick_t *pPort1,
				     atari_joystick_t *pPort2,
				     boolean bEnabled)
{
	m_pHandle[0] = pPort1;
	m_pHandle[1] = pPort2;
	m_bEnabled = bEnabled;

	SetActive (FALSE);
	Reset ();
}

void CAtariMenuJoystick::SetActive (boolean bActive)
{
	m_bActive = bActive;
	Reset ();
}

void CAtariMenuJoystick::Reset (void)
{
	m_nOwner = -1;
	m_nHead = 0;
	m_nTail = 0;

	for (unsigned n = 0; n < PortCount; n++)
	{
		m_Port[n].bArmed = FALSE;
		m_Port[n].nDirection = 0;
		m_Port[n].bFire = FALSE;
	}
}

unsigned CAtariMenuJoystick::PickDirection (unsigned nRaw,
					    unsigned nCurrent) const
{
	/*
	 * Keep the current direction while it is still held.  Without this a diagonal
	 * would alternate between two keys and the menu would move erratically.
	 */
	if (nCurrent != 0 && (nRaw & nCurrent) != 0)
	{
		return nCurrent;
	}

	if ((nRaw & ATARI_JOY_UP) != 0)		return ATARI_JOY_UP;
	if ((nRaw & ATARI_JOY_DOWN) != 0)	return ATARI_JOY_DOWN;
	if ((nRaw & ATARI_JOY_LEFT) != 0)	return ATARI_JOY_LEFT;
	if ((nRaw & ATARI_JOY_RIGHT) != 0)	return ATARI_JOY_RIGHT;

	return 0;
}

void CAtariMenuJoystick::QueueKey (int nKey, boolean bPressed)
{
	unsigned nNext = (m_nTail + 1) % QueueSize;
	if (nNext == m_nHead)
	{
		return;		/* Full; the menu gets the events already queued. */
	}

	m_Queue[m_nTail].nKey = nKey;
	m_Queue[m_nTail].bPressed = bPressed;
	m_Queue[m_nTail].bShift = FALSE;
	m_Queue[m_nTail].c = 0;
	m_nTail = nNext;
}

void CAtariMenuJoystick::ReleaseOwner (void)
{
	if (m_nOwner < 0)
	{
		return;
	}

	TPortState &Port = m_Port[m_nOwner];

	if (Port.nDirection != 0)
	{
		QueueKey (KeyForDirection (Port.nDirection), FALSE);
	}

	if (Port.bFire)
	{
		QueueKey (ATARI_KEY_RETURN, FALSE);
	}

	Port.nDirection = 0;
	Port.bFire = FALSE;
	m_nOwner = -1;
}

void CAtariMenuJoystick::Poll (void)
{
	if (!m_bActive || !m_bEnabled)
	{
		return;
	}

	for (unsigned n = 0; n < PortCount; n++)
	{
		atari_joystick_t *pHandle = m_pHandle[n];
		boolean bPresent = pHandle != 0
				&& pHandle->is_present (pHandle) != 0;

		if (!bPresent)
		{
			if (m_nOwner == (int) n)
			{
				ReleaseOwner ();
			}

			m_Port[n].bArmed = FALSE;
			m_Port[n].nDirection = 0;
			m_Port[n].bFire = FALSE;
			continue;
		}

		unsigned nRaw = pHandle->get_directions (pHandle);
		boolean bFire = pHandle->get_trigger (pHandle) == ATARI_JOY_TRIG_FIRED;

		/*
		 * A controller that was held when the OSD opened is not armed until it
		 * has been neutral once.  Otherwise opening the menu with a direction
		 * held would move the selection without a fresh press.
		 */
		if (!m_Port[n].bArmed)
		{
			if (nRaw == 0 && !bFire)
			{
				m_Port[n].bArmed = TRUE;
			}
			continue;
		}

		unsigned nDirection = PickDirection (nRaw, m_Port[n].nDirection);

		/*
		 * The first controller with a fresh control owns the menu session, so
		 * two pads cannot move the selection against each other.
		 */
		if (m_nOwner < 0 && (nDirection != 0 || bFire))
		{
			m_nOwner = (int) n;
			m_Port[n].nDirection = 0;
			m_Port[n].bFire = FALSE;
		}

		if (m_nOwner != (int) n)
		{
			continue;
		}

		if (nDirection != m_Port[n].nDirection)
		{
			if (m_Port[n].nDirection != 0)
			{
				QueueKey (KeyForDirection (m_Port[n].nDirection), FALSE);
			}

			if (nDirection != 0)
			{
				QueueKey (KeyForDirection (nDirection), TRUE);
			}

			m_Port[n].nDirection = nDirection;
		}

		if (bFire != m_Port[n].bFire)
		{
			QueueKey (ATARI_KEY_RETURN, bFire);
			m_Port[n].bFire = bFire;
		}
	}
}

boolean CAtariMenuJoystick::ConsumeEvent (TMenuKeyEvent *pEvent)
{
	if (pEvent == 0 || m_nHead == m_nTail)
	{
		return FALSE;
	}

	*pEvent = m_Queue[m_nHead];
	m_nHead = (m_nHead + 1) % QueueSize;

	return TRUE;
}
