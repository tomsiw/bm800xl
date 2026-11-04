//
// video_out.cpp
//
// Screen output for the bare-metal Atari XL/XE emulator.  See video_out.h for
// what this module is responsible for.
//
// Two things are worth knowing before reading the code:
//
//  1. The framebuffer is addressed directly, as a physical address handed over
//     by the firmware.  Circle maps DRAM identity, so that address can be used
//     as a pointer.  Going through SetArea() instead would copy every frame a
//     second time through the DMA engine, which an emulator cannot afford.
//
//  2. Double buffering is done the way Circle supports it: the framebuffer is
//     allocated twice as tall as the screen, and the visible window is moved
//     between the two halves with SetVirtualOffset().  All drawing therefore
//     happens at y = 0 in the back half, and Present() only has to update the
//     window offset.
//
#include "video_out.h"

#include <circle/chargenerator.h>
#include <circle/macros.h>
#include <circle/screen.h>
#include <circle/util.h>

#include <assert.h>

// The part of the emulated screen that a television would show.
static const unsigned ScreenStride	= 384;
static const unsigned ScreenHeight	= 240;
static const unsigned ScreenXOrigin	= 24;
static const unsigned ScreenVisibleWidth = 336;

/*
 * Widest destination row that can be staged, matching the size of
 * CAtariVideoOut::m_ColumnMap.  A wider geometry than this falls back to
 * writing the framebuffer directly.
 */
static const unsigned MaxDestinationWidth = 1920;

/* The longest run of destination rows one source row owns, with room to spare. */
static const unsigned MaxRunRows = 8;

/*
 * The scaled row darkened, built for the runs that need it.
 *
 * The dark values come from a palette built once per frame, so the row costs one
 * lookup per source pixel rather than three multiplies per destination pixel.
 * The buffer is still staged and copied like the lit row, which keeps the
 * framebuffer write pattern -- and the audio and pacing interaction that goes
 * with it -- the same as the renderer it replaces.
 */
static u16 s_RowBufferDim[MaxDestinationWidth];

/* Staging for one destination row, used to turn per-pixel uncached writes into
 * burst copies.  Shared by the screen scaler and the menu's dimming, which are
 * the two places that touch large numbers of framebuffer pixels. */
static u16 s_DimRow[MaxDestinationWidth];


CAtariVideoOut::CAtariVideoOut (void)
:	m_pFrameBuffer (0),
	m_bInitialized (FALSE),
	m_Mode (AtariDisplayModeUnknown),
	m_nWidth (0),
	m_nHeight (0),
	m_nPitchPixels (0),
	m_nScaleRequest (0),
	m_nScale (0),
	m_bClearPending (FALSE),
	m_bScanlines (FALSE),
	m_nScanlineNumerator (230),
	m_nScanlinePeriod (1),
	m_nDestLeft (0),
	m_nDestWidth (0),
	m_nColumnMapSize (0),
	m_pBackBuffer (0),
	m_bDoubleBuffered (FALSE),
	m_bFrontIsFirst (FALSE),
	m_bMarginsCleared (FALSE),
	m_pFont (&Font8x16)
{
}

CAtariVideoOut::~CAtariVideoOut (void)
{
	delete m_pFrameBuffer;
	m_pFrameBuffer = 0;
}

boolean CAtariVideoOut::Initialize (TAtariDisplayMode Mode)
{
	assert (!m_bInitialized);

	// Try the requested resolution, then fall back to the other one.  A monitor
	// that cannot do 1080p should still produce a picture rather than nothing.
	static const struct {
		unsigned		nWidth;
		unsigned		nHeight;
		TAtariDisplayMode	Mode;
	} Modes[] = {
		{ 1920, 1080, AtariDisplayMode1080p },
		{ 1280,  720, AtariDisplayMode720p  },
	};

	unsigned nFirst = Mode == AtariDisplayMode1080p ? 0 : 1;

	for (unsigned nTry = 0; nTry < 2; nTry++)
	{
		unsigned nIndex = (nFirst + nTry) % 2;

		// Virtual height is twice the screen height: the second half is the
		// back buffer for double buffering.
		CBcmFrameBuffer *pFrameBuffer =
			new CBcmFrameBuffer (Modes[nIndex].nWidth, Modes[nIndex].nHeight, 16,
					     Modes[nIndex].nWidth, Modes[nIndex].nHeight * 2,
					     0, FALSE);
		if (pFrameBuffer == 0)
		{
			continue;
		}

		if (!pFrameBuffer->Initialize ())
		{
			delete pFrameBuffer;
			continue;
		}

		// A 16 bpp request is what this code can draw into.  Circle reports
		// back the depth that was *requested*, not the one the firmware
		// granted, so there is nothing to verify here -- on the Pi 5 the
		// firmware's framebuffer is fixed at 16 bpp, which is what we want
		// anyway.  If a board came up with swapped colours, that is a
		// firmware configuration issue (framebuffer_swap), not something
		// detectable from here.
		m_pFrameBuffer = pFrameBuffer;
		m_Mode = Modes[nIndex].Mode;
		m_nWidth = pFrameBuffer->GetWidth ();
		m_nHeight = pFrameBuffer->GetHeight ();
		m_nPitchPixels = pFrameBuffer->GetPitch () / 2;

		ComputeGeometry ();

		//
		// Double buffering
		//
		// The cheap way to double buffer is to make the framebuffer twice as
		// tall as the screen and page-flip by moving the visible window with
		// SetVirtualOffset().  That works on the Raspberry Pi 1 to 4, but
		// Circle's own drawing layer deliberately does not use it on the
		// Pi 5, and the behaviour of the virtual offset there is not
		// established.  Rather than risk a stuck or absent picture on the
		// Pi 5, this uses the page-flip only where it is known to work, and
		// otherwise draws straight into the visible buffer.
		//
		// The cost on the Pi 5 is possible tearing at the top of the screen,
		// which is far better than no picture at all.
		//
		m_bFrontIsFirst = TRUE;
#if RASPPI <= 4
		m_bDoubleBuffered = pFrameBuffer->SetVirtualOffset (0, m_nHeight);
#else
		m_bDoubleBuffered = FALSE;
#endif
		if (!m_bDoubleBuffered)
		{
			pFrameBuffer->SetVirtualOffset (0, 0);
		}
		m_pBackBuffer = BackBuffer ();

		m_bInitialized = TRUE;
		return TRUE;
	}

	return FALSE;
}

//
// Geometry
//
// The picture is scaled to fill the screen height and centred horizontally,
// with black on both sides.  That is the "keep the original aspect ratio and
// pad the sides" behaviour: an Atari display is about 4:3, a modern monitor is
// 16:9, and stretching 4:3 to 16:9 would make every circle an ellipse.
//
void CAtariVideoOut::ComputeGeometry (void)
{
	/*
	 * The picture fills the framebuffer height and is centred horizontally,
	 * with black bars at the sides.
	 *
	 * The source is 336x240 and 720 rows are drawn from those 240, so the
	 * vertical scale is exactly 3.  To keep the machine's pixels the right shape
	 * the horizontal scale has to be 3 as well, which puts all 336 visible
	 * columns into 336 * 3 = 1008... except that the Atari's own pixels are not
	 * square at 4:3.  Its 336x240 is a 1.4 aspect, so scaling both axes by the
	 * same factor reproduces the real machine rather than distorting it.
	 *
	 * In practice, on a 1280x720 framebuffer, that is a 960-wide picture: the
	 * same horizontal scale the display is normally given, with the full height
	 * used.  An earlier version derived the height from the width and produced
	 * 960x600, which compressed the picture vertically and made every character
	 * look wide -- the fault was vertical, not horizontal.
	 */
	m_nDestHeight = m_nHeight;
	m_nDestWidth = m_nHeight * 4 / 3;

	/*
	 * Integer scaling, when that is what was asked for.
	 *
	 * The two modes give every source pixel a square block: 336x240 becomes
	 * 672x480 or 1008x720, so the picture is smaller than the framebuffer and
	 * sits in a border rather than filling it.  The result is pixel-exact, but it
	 * is not 4:3 -- 336/240 is 1.4 -- which is the trade the setting offers.
	 *
	 * The factor is reduced until it fits rather than clipping, and the factor
	 * actually used is left in m_nScale so the caller can report it.
	 */
	m_nScale = 0;

	if (m_nScaleRequest >= 2)
	{
		unsigned nScale = m_nScaleRequest;

		while (   nScale > 1
		       && (   ScreenVisibleWidth * nScale > m_nWidth
			   || ScreenHeight * nScale > m_nHeight))
		{
			nScale--;
		}

		m_nScale = nScale;
		m_nDestWidth  = ScreenVisibleWidth * nScale;
		m_nDestHeight = ScreenHeight * nScale;
	}

	/* Never exceed the framebuffer; a tall narrow display could run out. */
	if (m_nDestWidth > m_nWidth)
	{
		m_nDestWidth = m_nWidth;
		m_nDestHeight = m_nWidth * 3 / 4;
	}

	/*
	 * Centre it.  Both of these have to be assigned: the picture is positioned
	 * by m_nDestLeft and m_nDestTop, and leaving either at its constructor value
	 * of zero draws the picture against an edge instead of in the middle.
	 */
	m_nDestLeft = (m_nWidth - m_nDestWidth) / 2;
	m_nDestTop = (m_nHeight - m_nDestHeight) / 2;

	/*
	 * How many destination rows one Atari line becomes: the period of the
	 * scanline pattern.  A scanline is the gap between two of the machine's 240
	 * lines, so the pattern repeats once per upscaled line -- three rows at 720p,
	 * five at 1080p.  Rounded, because 1080/240 is 4.5 and the pattern has to be
	 * a whole number of rows.
	 */
	m_nScanlinePeriod = (m_nDestHeight + ScreenHeight / 2) / ScreenHeight;
	if (m_nScanlinePeriod < 1)
	{
		m_nScanlinePeriod = 1;
	}

	/*
	 * The column map maps every visible source column onto a destination column,
	 * so the source is stretched across the whole picture width rather than
	 * cropped.  It is built once here, not per frame: the mapping never changes,
	 * and rebuilding it inside the pixel loop would cost a division per pixel.
	 */
	m_nColumnMapSize = m_nDestWidth;
	assert (m_nColumnMapSize <= sizeof m_ColumnMap / sizeof m_ColumnMap[0]);

	for (unsigned nX = 0; nX < m_nColumnMapSize; nX++)
	{
		unsigned nSource = (unsigned) (((u32) nX * ScreenVisibleWidth)
					       / m_nDestWidth);
		if (nSource >= ScreenVisibleWidth)
		{
			nSource = ScreenVisibleWidth - 1;
		}
		m_ColumnMap[nX] = (u16) nSource;
	}

	/*
	 * Row map, for the same reason: the destination row decides which source row
	 * it shows, and doing that division per row per frame is pure waste.  The
	 * picture no longer fills the framebuffer height, so rows outside it are
	 * left black.
	 */
	m_nRowMapSize = m_nDestHeight;
	for (unsigned nY = 0; nY < m_nRowMapSize; nY++)
	{
		unsigned nSource = (unsigned) (((u32) nY * ScreenHeight) / m_nDestHeight);
		if (nSource >= ScreenHeight)
		{
			nSource = ScreenHeight - 1;
		}
		m_RowMap[nY] = (u16) nSource;
	}

	/*
	 * Integer scaling needs no maps at all: every source column owns exactly
	 * m_nScale destination columns and every source row exactly m_nScale
	 * destination rows, so the runs are written straight out.  The rest of this
	 * function exists for the 4:3 mapping, where the scale is 2.86 by 3 and the
	 * runs vary.
	 */
	if (m_nScale >= 2)
	{
		m_nColumnMapSize = m_nDestWidth;
		m_nRowMapSize = m_nDestHeight;

		m_nColumnRuns = ScreenVisibleWidth;
		for (unsigned nX = 0; nX < m_nColumnRuns; nX++)
		{
			m_ColumnRunSource[nX] = (u16) nX;
			m_ColumnRunLength[nX] = (u8) m_nScale;
		}

		m_nRowRuns = ScreenHeight;
		for (unsigned nY = 0; nY < m_nRowRuns; nY++)
		{
			m_RowRunSource[nY] = (u16) nY;
			m_RowRunFirst[nY] = (u16) (nY * m_nScale);
			m_RowRunLength[nY] = (u8) m_nScale;
		}

		return;
	}

	/*
	 * Run maps: the same mapping, read the other way round.
	 *
	 * The two loops above answer "which source pixel does this destination pixel
	 * show".  The scaler wants the opposite -- "how many destination pixels does
	 * this source pixel own" -- because then it can walk the source once and
	 * write runs, instead of walking 960 destination pixels and chasing three
	 * dependent loads for each of them.  On an in-order core that dependency
	 * chain was the whole cost of the fill: 16.4 ms of a 20 ms frame.
	 *
	 * Both scales are non-integer in general -- 336 columns become 960, so most
	 * columns own three destination pixels and some own two -- so a run is a
	 * count, not a constant.  Summing them must reproduce the destination size
	 * exactly, which is what keeps the picture from drifting; the assertions
	 * below state that.
	 */
	m_nColumnRuns = 0;
	{
		unsigned nX = 0;

		while (nX < m_nColumnMapSize && m_nColumnRuns < MaxSourceRuns)
		{
			unsigned nSource = m_ColumnMap[nX];
			unsigned nRun = 1;

			while (   nX + nRun < m_nColumnMapSize
			       && m_ColumnMap[nX + nRun] == nSource)
			{
				nRun++;
			}

			m_ColumnRunSource[m_nColumnRuns] = (u16) nSource;
			m_ColumnRunLength[m_nColumnRuns] = (u8) nRun;
			m_nColumnRuns++;

			nX += nRun;
		}

		assert (nX == m_nColumnMapSize);
	}

	m_nRowRuns = 0;
	{
		unsigned nY = 0;

		while (nY < m_nRowMapSize && m_nRowRuns < MaxSourceRuns)
		{
			unsigned nSource = m_RowMap[nY];
			unsigned nRun = 1;

			while (   nY + nRun < m_nRowMapSize
			       && m_RowMap[nY + nRun] == nSource)
			{
				nRun++;
			}

			m_RowRunSource[m_nRowRuns] = (u16) nSource;
			m_RowRunFirst[m_nRowRuns] = (u16) nY;
			m_RowRunLength[m_nRowRuns] = (u8) nRun;
			m_nRowRuns++;

			nY += nRun;
		}

		assert (nY == m_nRowMapSize);
	}
}

/*
 * Applies a pixel scale.
 *
 * Live, and deliberately cheap: the geometry is arithmetic and the run maps are
 * rebuilt in the same pass, so nothing of the framebuffer changes and no frame is
 * lost.  The one thing that has to follow is the black margins -- they are
 * cleared once and remembered, so a change to the picture rectangle makes them
 * stale and they must be cleared again on the next fill.
 */
void CAtariVideoOut::SetAspect (int nScale)
{
	if (nScale == m_nScaleRequest)
	{
		return;
	}

	m_nScaleRequest = nScale;

	ComputeGeometry ();

	/*
	 * Both halves are blacked, not just the margins of the one being drawn.
	 *
	 * The margins are normally cleared once and remembered, which is fine while
	 * the picture rectangle never changes.  It changes here, and the half that is
	 * not currently being composed still holds the previous picture: its rows
	 * reached further sideways, so the sides would alternate between that older
	 * picture and black on every flip -- visible as a flickering border with the
	 * middle of the screen perfectly steady.
	 *
	 * The blanking is DEFERRED to the next fill rather than done here.  While the
	 * menu is open the picture behind it is not being repainted -- with
	 * STATIC_OSD it is not repainted at all -- so blanking now would leave the
	 * screen empty until the menu closed.  The first frame that draws the picture
	 * again does it, which is the first frame after the OSD closes.
	 */
	m_bMarginsCleared = FALSE;
	m_bClearPending = TRUE;
}

/*
 * Turns the scanline effect on and off, live.
 *
 * Off is the code path that existed before this option: whole scaled rows,
 * copied.  On builds a darkened copy of a scaled row from the dark palette for
 * each run that has a dark destination row and copies it where the pattern calls
 * for it.
 */
void CAtariVideoOut::SetScanlines (boolean bEnabled)
{
	if (bEnabled == m_bScanlines)
	{
		return;
	}

	m_bScanlines = bEnabled;
}

/* Brightness of the dark lines, as a percentage: 85, 90 or 95. */
void CAtariVideoOut::SetScanlineFactor (int nPercent)
{
	if (nPercent < 50)
	{
		nPercent = 50;
	}
	if (nPercent > 100)
	{
		nPercent = 100;
	}

	m_nScanlineNumerator = (unsigned) (nPercent * 256 / 100);
}

/*
 * The OSD-close entry point: the panel may have been drawn over the margins, so
 * clear them in both halves now rather than waiting for the next fill.
 */
void CAtariVideoOut::InvalidatePadding (void)
{
	ClearMargins ();
}

void CAtariVideoOut::ClearMargins (void)
{
	/*
	 * Cleared in BOTH halves, now, rather than asking for it on the next fill.
	 *
	 * The next fill clears the margins of whichever half it happens to be
	 * composing and then sets the once-only flag, so the other half keeps
	 * whatever was in its margins until something repaints it whole.  That is
	 * invisible while only the picture ever writes there -- and it is a real
	 * fault once the OSD can: an integer scale makes the picture narrower than
	 * the panel, the panel is drawn over the margins, and after the menu closed
	 * the strips alternated between black and the panel's edges on every flip
	 * while the middle of the screen was perfectly steady.
	 *
	 * One bounded pass over the margin pixels.  The OSD close path calls this
	 * directly; FillScreen() calls it once when the flag says the margins are
	 * stale.
	 */
	u16 *pFirst = (u16 *) (uintptr) m_pFrameBuffer->GetBuffer ();
	unsigned nHalf = m_bDoubleBuffered
		       ? m_nPitchPixels * m_nHeight : 0;
	unsigned nHalves = m_bDoubleBuffered ? 2 : 1;

	for (unsigned nBuffer = 0; nBuffer < nHalves; nBuffer++)
	{
		u16 *pFrame = pFirst + (uintptr) nHalf * nBuffer;

		/* Above and below the picture: the full width. */
		for (unsigned nY = 0; nY < m_nDestTop; nY++)
		{
			memset (pFrame + (uintptr) nY * m_nPitchPixels, 0,
				m_nWidth * sizeof (u16));
		}

		for (unsigned nY = m_nDestTop + m_nDestHeight; nY < m_nHeight; nY++)
		{
			memset (pFrame + (uintptr) nY * m_nPitchPixels, 0,
				m_nWidth * sizeof (u16));
		}

		/* Left and right of it. */
		if (m_nDestLeft > 0)
		{
			u16 *pLeft = pFrame + (uintptr) m_nDestTop * m_nPitchPixels;
			u16 *pRight = pLeft + m_nDestLeft + m_nColumnMapSize;
			unsigned nRight = m_nWidth - m_nDestLeft - m_nColumnMapSize;

			for (unsigned nY = 0; nY < m_nDestHeight; nY++)
			{
				memset (pLeft, 0, m_nDestLeft * sizeof (u16));

				if (nRight > 0)
				{
					memset (pRight, 0, nRight * sizeof (u16));
				}

				pLeft += m_nPitchPixels;
				pRight += m_nPitchPixels;
			}
		}
	}

	m_bMarginsCleared = TRUE;
}

/*
 * Blacks both halves of the double buffer and shows the first one.
 *
 * The virtual offset is set as well, so that what is displayed is the half that
 * was just cleared rather than the other one.
 */
void CAtariVideoOut::ClearBuffers (void)
{
	assert (m_pFrameBuffer != 0);

	u16 *pBuffer = (u16 *) (uintptr) m_pFrameBuffer->GetBuffer ();
	size_t nBytes = (size_t) m_nPitchPixels * m_nHeight * sizeof (u16);

	memset (pBuffer, 0, nBytes);

	if (m_bDoubleBuffered)
	{
		memset (pBuffer + (uintptr) m_nPitchPixels * m_nHeight, 0, nBytes);

		m_bFrontIsFirst = TRUE;
		m_pFrameBuffer->SetVirtualOffset (0, 0);
	}

	m_pBackBuffer = BackBuffer ();
}

u16 *CAtariVideoOut::BackBuffer (void) const
{
	assert (m_pFrameBuffer != 0);

	// Without double buffering there is only one buffer, and it is both the
	// one being composed and the one on screen.
	if (!m_bDoubleBuffered)
	{
		return (u16 *) (uintptr) m_pFrameBuffer->GetBuffer ();
	}

	u16 *pBuffer = (u16 *) (uintptr) m_pFrameBuffer->GetBuffer ();

	// The back buffer is the half that is not currently visible.
	if (m_bFrontIsFirst)
	{
		pBuffer += (uintptr) m_nPitchPixels * m_nHeight;
	}

	return pBuffer;
}

u16 *CAtariVideoOut::FrontBuffer (void) const
{
	assert (m_pFrameBuffer != 0);

	u16 *pBuffer = (u16 *) (uintptr) m_pFrameBuffer->GetBuffer ();

	if (m_bDoubleBuffered && !m_bFrontIsFirst)
	{
		pBuffer += (uintptr) m_nPitchPixels * m_nHeight;
	}

	return pBuffer;
}

void CAtariVideoOut::FillScreen (const u8 *pScreen, const u32 *pPalette)
{
	assert (m_bInitialized);
	assert (pScreen != 0);
	assert (pPalette != 0);

	/*
	 * A pending aspect change blanks both halves first, so that the half which
	 * still holds the previous picture -- with its wider rows -- becomes black
	 * margins rather than a flickering border.
	 */
	if (m_bClearPending)
	{
		ClearBuffers ();
		m_bMarginsCleared = TRUE;
		m_bClearPending = FALSE;
	}

	/*
	 * Margins are written once, not every frame.
	 *
	 * They are the top and bottom bars and the side strips, and the only things
	 * that write there are the OSD panel and FillSolid().  Both dirty them
	 * explicitly (InvalidatePadding() on menu close, FillSolid() below), so a
	 * fill only has to clear them when that flag says they are stale.  Both
	 * halves are cleared, not just the one being composed: the next Present()
	 * makes the other half visible.
	 */
	if (!m_bMarginsCleared)
	{
		ClearMargins ();
	}

	u16 *pFrame = BackBuffer ();

	// Palette indices to RGB565, so the inner loop is a table lookup rather
	// than a shift-and-mask per pixel.  Rebuilding 256 entries per frame is
	// cheaper than watching for palette changes.
	u16 Palette565[256];
	/*
	 * Convert the source line to the framebuffer's 5:6:5 layout.
	 *
	 * The palette is re-converted every frame rather than cached, because the
	 * emulated machine can change a colour at any time (a raster split, a colour
	 * register write mid-frame); 256 entries is a negligible cost next to the
	 * pixel loop below.
	 */
	for (unsigned nIndex = 0; nIndex < 256; nIndex++)
	{
		u32 nRGB = pPalette[nIndex];
		Palette565[nIndex] = (u16) ((((nRGB >> 16) & 0xf8) << 8)
					    | (((nRGB >> 8) & 0xfc) << 3)
					    | ((nRGB & 0xff) >> 3));
	}

	/*
	 * The dark-row palette, built once per frame when scanlines are on.
	 *
	 * The old renderer darkened the already-scaled row, which cost three integer
	 * multiplies per destination pixel of every dark row.  Doing it on the 256
	 * palette entries instead leaves the row build as one lookup per source
	 * pixel; the values are the same, because it is the same expression applied
	 * to the same components.
	 */
	u16 Palette565Dim[256] = { };
	if (m_bScanlines)
	{
		const unsigned nNum = m_nScanlineNumerator;

		for (unsigned nIndex = 0; nIndex < 256; nIndex++)
		{
			const u16 nColour = Palette565[nIndex];

			unsigned nRed   = (((nColour >> 11) & 0x1f) * nNum) >> 8;
			unsigned nGreen = (((nColour >>  5) & 0x3f) * nNum) >> 8;
			unsigned nBlue  = (( nColour        & 0x1f) * nNum) >> 8;

			Palette565Dim[nIndex] = (u16) ((nRed << 11)
						       | (nGreen << 5)
						       | nBlue);
		}
	}

	/*
	 * Scale the picture.
	 *
	 * The source is walked once per source row, not once per destination pixel.
	 *
	 * The earlier version walked all 960 destination pixels and, for each, chased
	 * three dependent loads -- the column map, the source byte, the palette -- on
	 * a core that is in-order and cannot overlap them.  Measured on a Zero 2 W,
	 * that came to 16.4 ms per frame, 92% of the fill and most of a 20 ms frame,
	 * while the copy into the un-cacheable framebuffer -- the part I expected to
	 * dominate -- was only 1.5 ms.
	 *
	 * So the loop now walks the 336 source pixels and writes each one's run of
	 * two or three destination pixels: one dependent chain per source pixel
	 * instead of per destination pixel, and a third fewer chains at that.  The
	 * run lengths come from ComputeGeometry(), and their sum is exactly the
	 * destination width, so the picture cannot drift.
	 *
	 * Each source row is scaled once and then copied to as many destination rows
	 * as it owns -- three at 720p -- which used to be three separate scales of the
	 * same row.
	 */
	static u16 s_RowBuffer[MaxDestinationWidth];

	/*
	 * Where the scanline pattern has got to: rows into the current block, and
	 * whether that block is the dark one.  Frame state, so the blocks keep their
	 * place from one row run to the next rather than restarting at each.
	 */
	static unsigned s_nScanlineRow;
	static boolean s_bScanlineDark;


	const bool bStage = m_nColumnMapSize <= MaxDestinationWidth;


	for (unsigned nRow = 0; nRow < m_nRowRuns; nRow++)
	{
		const u8 *pSource = pScreen
				  + (uintptr) m_RowRunSource[nRow] * ScreenStride;
		const unsigned nRunRows = m_RowRunLength[nRow];

		if (!bStage)
		{
			/*
			 * A destination wider than the staging buffer: write the picture
			 * directly.  Slower, but this geometry never occurs on a display
			 * this emulator supports.
			 */
			for (unsigned nRowIndex = 0; nRowIndex < nRunRows; nRowIndex++)
			{
				u16 *pRow = pFrame
					  + (uintptr) (m_nDestTop + m_RowRunFirst[nRow]
							+ nRowIndex) * m_nPitchPixels
					  + m_nDestLeft;

				unsigned nOut = 0;

				for (unsigned nCol = 0; nCol < m_nColumnRuns; nCol++)
				{
					u16 nColour = Palette565[
						pSource[ScreenXOrigin
							+ m_ColumnRunSource[nCol]]];

					for (unsigned k = 0; k < m_ColumnRunLength[nCol]; k++)
					{
						pRow[nOut++] = nColour;
					}
				}
			}

			continue;
		}

		/*
		 * One scaled row in cached memory, then one burst copy per destination
		 * row that shows it.  With scanlines on, a run may need the lit row, the
		 * dark row, or both: the period equals the run length at 720p and the
		 * integer scales, but not at 1080p 4:3, where a run can straddle the
		 * boundary.
		 */
		boolean bDark[MaxRunRows] = { FALSE };
		boolean bAllDark = m_bScanlines;
		boolean bAnyDark = FALSE;

		if (m_bScanlines)
		{
			unsigned nProbeRow = s_nScanlineRow;
			boolean bProbeDark = s_bScanlineDark;

			for (unsigned nRowIndex = 0; nRowIndex < nRunRows; nRowIndex++)
			{
				bDark[nRowIndex] = bProbeDark;

				if (bProbeDark)
				{
					bAnyDark = TRUE;
				}
				else
				{
					bAllDark = FALSE;
				}

				if (++nProbeRow >= m_nScanlinePeriod)
				{
					nProbeRow = 0;
					bProbeDark = bProbeDark ? FALSE : TRUE;
				}
			}
		}

		const boolean bNeedLit = !bAllDark;
		const boolean bNeedDim = m_bScanlines && bAnyDark;

		if (bNeedLit && bNeedDim)
		{
			// A run that straddles a boundary: build both rows in one pass.
			u16 *pLit = s_RowBuffer;
			u16 *pDim = s_RowBufferDim;

			for (unsigned nCol = 0; nCol < m_nColumnRuns; nCol++)
			{
				const unsigned nSource = pSource[ScreenXOrigin
								+ m_ColumnRunSource[nCol]];
				const u16 nLit = Palette565[nSource];
				const u16 nDim = Palette565Dim[nSource];
				const unsigned nRun = m_ColumnRunLength[nCol];

				for (unsigned k = 0; k < nRun; k++)
				{
					pLit[k] = nLit;
					pDim[k] = nDim;
				}

				pLit += nRun;
				pDim += nRun;
			}
		}
		else if (bNeedDim)
		{
			// Every row in the run is dark: the lit row is not built at all.
			u16 *pDim = s_RowBufferDim;

			for (unsigned nCol = 0; nCol < m_nColumnRuns; nCol++)
			{
				const u16 nDim = Palette565Dim[
					pSource[ScreenXOrigin
						+ m_ColumnRunSource[nCol]]];
				const unsigned nRun = m_ColumnRunLength[nCol];

				for (unsigned k = 0; k < nRun; k++)
				{
					pDim[k] = nDim;
				}

				pDim += nRun;
			}
		}
		else
		{
			// No scanlines, or every row in this run is lit.
			u16 *pLit = s_RowBuffer;

			for (unsigned nCol = 0; nCol < m_nColumnRuns; nCol++)
			{
				const u16 nLit = Palette565[
					pSource[ScreenXOrigin
						+ m_ColumnRunSource[nCol]]];
				const unsigned nRun = m_ColumnRunLength[nCol];

				for (unsigned k = 0; k < nRun; k++)
				{
					pLit[k] = nLit;
				}

				pLit += nRun;
			}
		}

		for (unsigned nRowIndex = 0; nRowIndex < nRunRows; nRowIndex++)
		{
			u16 *pRow = pFrame
				  + (uintptr) (m_nDestTop + m_RowRunFirst[nRow]
						+ nRowIndex) * m_nPitchPixels
				  + m_nDestLeft;

			memcpy (pRow,
				(m_bScanlines && bDark[nRowIndex])
					? s_RowBufferDim : s_RowBuffer,
				m_nColumnMapSize * sizeof (u16));

			if (m_bScanlines)
			{
				if (++s_nScanlineRow >= m_nScanlinePeriod)
				{
					s_nScanlineRow = 0;
					s_bScanlineDark = s_bScanlineDark ? FALSE : TRUE;
				}
			}
		}
	}
}

/*
 * Copies the visible half into the half being composed.
 *
 * Only needed when the picture stops being redrawn while the OSD continues to be
 * drawn over it: the two halves otherwise hold different frames, and every OSD
 * redraw -- which flips -- would swap the background under the panel.
 *
 * The halves are contiguous rows of the same pitch, so this is one memcpy per
 * buffer and nothing else.
 */
void CAtariVideoOut::CopyFrontToBack (void)
{
	assert (m_bInitialized);

	if (!m_bDoubleBuffered)
	{
		return;			// one buffer: nothing to synchronise
	}

	memcpy (BackBuffer (), FrontBuffer (),
		(size_t) m_nPitchPixels * m_nHeight * sizeof (u16));
}

void CAtariVideoOut::Present (void)
{
	assert (m_bInitialized);
	if (m_bDoubleBuffered)
	{
		// Swap the visible window to the half just drawn.  No copying.
		m_bFrontIsFirst = !m_bFrontIsFirst;
		m_pFrameBuffer->SetVirtualOffset (0, m_bFrontIsFirst ? 0 : m_nHeight);
	}

	m_pBackBuffer = BackBuffer ();
}

/*
 * Fills the visible screen with one colour.
 *
 * Both halves of the double buffer are painted, because which half the firmware
 * is showing depends on the virtual offset -- and if start-up has gone wrong,
 * that offset is exactly the sort of thing that may be wrong.  Painting both
 * means the colour appears whichever half is on screen.
 *
 * The fill is deliberately done with memset-friendly row writes rather than
 * through PutPixel: this runs when something else has already failed, and it
 * should not be able to fail too.
 */
void CAtariVideoOut::FillSolid (TAtariColor Color)
{
	if (!m_bInitialized || m_pFrameBuffer == 0)
	{
		return;
	}

	u16 *pBase = (u16 *) (uintptr) m_pFrameBuffer->GetBuffer ();
	if (pBase == 0)
	{
		return;
	}

	/* Two halves when double buffered, one otherwise. */
	unsigned nHalves = m_bDoubleBuffered ? 2 : 1;
	unsigned nTotalRows = m_nHeight * nHalves;

	for (unsigned nY = 0; nY < nTotalRows; nY++)
	{
		u16 *pRow = pBase + (uintptr) nY * m_nPitchPixels;

		for (unsigned nX = 0; nX < m_nWidth; nX++)
		{
			pRow[nX] = Color;
		}
	}

	/*
	 * The margins now hold Color rather than black, so the next frame has to
	 * clear them again.
	 */
	m_bMarginsCleared = FALSE;

	/* Make sure the half being shown is the one that was painted. */
	if (m_bDoubleBuffered)
	{
		m_pFrameBuffer->SetVirtualOffset (0, m_bFrontIsFirst ? 0 : m_nHeight);
	}
}

//
// Drawing primitives for the OSD menu
//

void CAtariVideoOut::PutPixel (unsigned nX, unsigned nY, TAtariColor Color)
{
	if (nX >= m_nWidth || nY >= m_nHeight)
	{
		return;
	}

	u16 *pRow = m_pBackBuffer + (uintptr) nY * m_nPitchPixels;
	pRow[nX] = Color;
}

void CAtariVideoOut::DrawRect (unsigned nX, unsigned nY, unsigned nWidth, unsigned nHeight,
			       TAtariColor Color)
{
	assert (m_bInitialized);

	if (nX >= m_nWidth || nY >= m_nHeight)
	{
		return;
	}
	if (nX + nWidth > m_nWidth)
	{
		nWidth = m_nWidth - nX;
	}
	if (nY + nHeight > m_nHeight)
	{
		nHeight = m_nHeight - nY;
	}

	// Fill alternating bytes to get two pixels per store.
	u32 nPair = ((u32) Color << 16) | Color;
	unsigned nPairs = nWidth / 2;
	boolean bOdd = (nWidth & 1) != 0;

	for (unsigned nRow = 0; nRow < nHeight; nRow++)
	{
		u16 *pRow = m_pBackBuffer + (uintptr) (nY + nRow) * m_nPitchPixels + nX;
		u32 *pPair = (u32 *) pRow;

		for (unsigned n = 0; n < nPairs; n++)
		{
			*pPair++ = nPair;
		}
		if (bOdd)
		{
			pRow[nWidth - 1] = Color;
		}
	}
}

void CAtariVideoOut::DrawRectOutline (unsigned nX, unsigned nY, unsigned nWidth,
				      unsigned nHeight, TAtariColor Color)
{
	if (nWidth == 0 || nHeight == 0)
	{
		return;
	}

	DrawRect (nX, nY, nWidth, 1, Color);
	DrawRect (nX, nY + nHeight - 1, nWidth, 1, Color);
	DrawRect (nX, nY, 1, nHeight, Color);
	DrawRect (nX + nWidth - 1, nY, 1, nHeight, Color);
}

void CAtariVideoOut::DimRect (unsigned nX, unsigned nY, unsigned nWidth, unsigned nHeight,
			      unsigned nPercent)
{
	assert (m_bInitialized);

	if (nX >= m_nWidth || nY >= m_nHeight)
	{
		return;
	}
	if (nX + nWidth > m_nWidth)
	{
		nWidth = m_nWidth - nX;
	}
	if (nY + nHeight > m_nHeight)
	{
		nHeight = m_nHeight - nY;
	}
	if (nPercent > 100)
	{
		nPercent = 100;
	}

	/*
	 * Scale each colour component by nKeep/100, towards black.
	 *
	 * The per-channel results are tabulated once per call: five bits for red
	 * and blue, six for green.  The old inner loop did three integer divisions
	 * per pixel -- 1.3 million of them for a 900x480 panel -- which is most of
	 * what a transparent OSD redraw cost.  A table lookup replaces each one.
	 */
	unsigned nKeep = 100 - nPercent;

	u16 Dim5[32];
	u16 Dim6[64];

	for (unsigned nLevel = 0; nLevel < 32; nLevel++)
	{
		Dim5[nLevel] = (u16) (nLevel * nKeep / 100);
	}

	for (unsigned nLevel = 0; nLevel < 64; nLevel++)
	{
		Dim6[nLevel] = (u16) (nLevel * nKeep / 100);
	}

	/*
	 * Dim row by row through cached memory.
	 *
	 * Dimming is a read-modify-write: every pixel in the panel is read from and
	 * written to the framebuffer, which is not cacheable from the ARM, so each
	 * pixel cost two uncached transactions.  With the menu open this measured at
	 * 44-69 ms per frame -- worse than the whole screen scale before it was
	 * fixed -- and that is what made the OSD flicker: at that cost the frame
	 * misses its slot and the panel is shown half-updated.
	 *
	 * Copying the row in, dimming it in ordinary memory and copying it back turns
	 * the per-pixel traffic into bursts.  A row wider than the staging buffer
	 * falls back to the direct path rather than overrunning it.
	 */
	const bool bStage = nWidth <= MaxDestinationWidth;

	for (unsigned nRow = 0; nRow < nHeight; nRow++)
	{
		u16 *pRow = m_pBackBuffer + (uintptr) (nY + nRow) * m_nPitchPixels + nX;

		if (!bStage)
		{
			for (unsigned nCol = 0; nCol < nWidth; nCol++)
			{
				u16 nPixel = pRow[nCol];

				unsigned nR = Dim5[(nPixel >> 11) & 0x1f];
				unsigned nG = Dim6[(nPixel >>  5) & 0x3f];
				unsigned nB = Dim5[ nPixel        & 0x1f];

				pRow[nCol] = (u16) ((nR << 11) | (nG << 5) | nB);
			}

			continue;
		}

		memcpy (s_DimRow, pRow, nWidth * sizeof (u16));

		for (unsigned nCol = 0; nCol < nWidth; nCol++)
		{
			u16 nPixel = s_DimRow[nCol];

			unsigned nR = Dim5[(nPixel >> 11) & 0x1f];
			unsigned nG = Dim6[(nPixel >>  5) & 0x3f];
			unsigned nB = Dim5[ nPixel        & 0x1f];

			s_DimRow[nCol] = (u16) ((nR << 11) | (nG << 5) | nB);
		}

		memcpy (pRow, s_DimRow, nWidth * sizeof (u16));
	}
}

unsigned CAtariVideoOut::GetTextHeight (void) const
{
	CCharGenerator Generator (*m_pFont);

	return Generator.GetCharHeight ();
}

unsigned CAtariVideoOut::GetTextWidth (void) const
{
	CCharGenerator Generator (*m_pFont);

	return Generator.GetCharWidth ();
}

unsigned CAtariVideoOut::GetTextWidth (const char *pText) const
{
	if (pText == 0)
	{
		return 0;
	}

	return (unsigned) strlen (pText) * GetTextWidth ();
}

void CAtariVideoOut::DrawText (unsigned nX, unsigned nY, TAtariColor Color,
			       const char *pText, boolean bDoubleSize)
{
	assert (m_bInitialized);

	if (pText == 0)
	{
		return;
	}

	CCharGenerator Generator (*m_pFont,
				  CCharGenerator::MakeFlags (bDoubleSize, bDoubleSize));

	unsigned nCharWidth  = Generator.GetCharWidth ();
	unsigned nCharHeight = Generator.GetCharHeight ();
	unsigned nHeightMult = bDoubleSize ? 2 : 1;

	for (unsigned nChar = 0; pText[nChar] != '\0'; nChar++)
	{
		unsigned nCharX = nX + nChar * nCharWidth;

		for (unsigned nLine = 0; nLine < nCharHeight; nLine++)
		{
			CCharGenerator::TPixelLine Line =
				Generator.GetPixelLine (pText[nChar], nLine);

			if (Line == 0)
			{
				continue;
			}

			for (unsigned nYMult = 0; nYMult < nHeightMult; nYMult++)
			{
				unsigned nPixelY = nY + nLine * nHeightMult + nYMult;
				if (nPixelY >= m_nHeight)
				{
					continue;
				}

				u16 *pRow = m_pBackBuffer
					  + (uintptr) nPixelY * m_nPitchPixels;

				for (unsigned nPixelX = 0; nPixelX < nCharWidth; nPixelX++)
				{
					if (Generator.GetPixel (nPixelX, Line))
					{
						unsigned nTargetX = nCharX + nPixelX;
						if (nTargetX < m_nWidth)
						{
							pRow[nTargetX] = Color;
						}
					}
				}
			}
		}
	}
}

void CAtariVideoOut::DrawTextBar (unsigned nX, unsigned nY, TAtariColor BackColor,
				  TAtariColor TextColor, const char *pText)
{
	unsigned nHeight = GetTextHeight ();
	unsigned nWidth = GetTextWidth (pText) + 8;

	DrawRect (nX, nY, nWidth, nHeight, BackColor);
	DrawText (nX + 4, nY, TextColor, pText);
}
