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
	m_nDestLeft (0),
	m_nDestWidth (0),
	m_nColumnMapSize (0),
	m_pBackBuffer (0),
	m_bDoubleBuffered (FALSE),
	m_bFrontIsFirst (FALSE),
	m_bPaddingCleared (FALSE),
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
	 * Clear the top and bottom bars once.
	 *
	 * The picture does not fill the framebuffer height any more -- it is a 4:3
	 * rectangle centred vertically -- so these margins have to be black.  Only
	 * cleared when the geometry leaves any, and cleared over the full width
	 * because the side padding below covers the picture rows only.
	 */
	if (m_nDestTop > 0)
	{
		unsigned nRowBytes = m_nWidth * sizeof (u16);

		memset (pFrame, 0, (uintptr) m_nDestTop * nRowBytes);

		u16 *pBelow = pFrame + (uintptr) (m_nDestTop + m_nDestHeight) * m_nPitchPixels;
		memset (pBelow, 0,
			(uintptr) (m_nHeight - m_nDestTop - m_nDestHeight) * nRowBytes);
	}

	/*
	 * Scale the picture.
	 *
	 * Both maps are precomputed in ComputeGeometry(), so this loop is one table
	 * lookup and one palette lookup per destination pixel with no arithmetic to
	 * speak of.  That matters: at 960x720 this writes around 690,000 pixels per
	 * frame, and a division per pixel here was costing real frame time.
	 */
	/*
	 * Scale the picture, one row at a time into a cached staging buffer.
	 *
	 * The framebuffer is not cacheable from the ARM, so every store to it is its
	 * own bus transaction.  Writing the row pixel by pixel straight into it
	 * measured at ~31 ms per frame -- 95% of the frame budget, against 1.5 ms for
	 * the emulation itself -- because ~690,000 individual 16-bit stores were each
	 * paying for a separate transaction.
	 *
	 * Building the row in ordinary cached memory and then copying it with one
	 * memcpy() turns that into a small number of wide, burst stores: Circle's
	 * memcpy uses 128-bit transfers, so the transaction count drops by roughly an
	 * order of magnitude while the same bytes are written.
	 */
	static u16 s_RowBuffer[MaxDestinationWidth];

	const bool bStage = m_nColumnMapSize <= MaxDestinationWidth;

	for (unsigned nY = 0; nY < m_nRowMapSize; nY++)
	{
		const u8 *pSource = pScreen + (uintptr) m_RowMap[nY] * ScreenStride;
		u16 *pRow = pFrame
			  + (uintptr) (nY + m_nDestTop) * m_nPitchPixels
			  + m_nDestLeft;

		if (!bStage)
		{
			for (unsigned nX = 0; nX < m_nColumnMapSize; nX++)
			{
				pRow[nX] = Palette565[pSource[ScreenXOrigin + m_ColumnMap[nX]]];
			}

			continue;
		}

		for (unsigned nX = 0; nX < m_nColumnMapSize; nX++)
		{
			s_RowBuffer[nX] =
				Palette565[pSource[ScreenXOrigin + m_ColumnMap[nX]]];
		}

		memcpy (pRow, s_RowBuffer, m_nColumnMapSize * sizeof (u16));
	}

	/*
	 * Black side padding.
	 *
	 * Cleared ONCE, not every frame.  Nothing ever writes to the margins, so
	 * re-clearing them each frame spends writes on bytes that cannot have
	 * changed: at 960 wide in a 1280 framebuffer that is 320x720 pixels, about a
	 * quarter of all pixel traffic per frame.  The framebuffer is not cacheable
	 * from the ARM, so those writes were measurably expensive -- the emulator
	 * ran at 29 fps against a 50 fps target.
	 *
	 * The flag is cleared again by anything that repaints the whole buffer, so
	 * the margins are restored after a stage clear or an error report.
	 */
	if (m_nDestLeft > 0 && !m_bPaddingCleared)
	{
		u16 *pLeft = pFrame + (uintptr) m_nDestTop * m_nPitchPixels;
		u16 *pRight = pLeft + m_nDestLeft + m_nColumnMapSize;
		unsigned nRightWidth = m_nWidth - m_nDestLeft - m_nColumnMapSize;

		for (unsigned nY = 0; nY < m_nDestHeight; nY++)
		{
			memset (pLeft, 0, m_nDestLeft * sizeof (u16));
			memset (pRight, 0, nRightWidth * sizeof (u16));

			pLeft += m_nPitchPixels;
			pRight += m_nPitchPixels;
		}

		m_bPaddingCleared = TRUE;
	}
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
	m_bPaddingCleared = FALSE;

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

	// Halve each colour component about nPercent/100 of the way towards black.
	unsigned nKeep = 100 - nPercent;

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

				unsigned nR = ((nPixel >> 11) & 0x1f) * nKeep / 100;
				unsigned nG = ((nPixel >>  5) & 0x3f) * nKeep / 100;
				unsigned nB = ( nPixel        & 0x1f) * nKeep / 100;

				pRow[nCol] = (u16) ((nR << 11) | (nG << 5) | nB);
			}

			continue;
		}

		memcpy (s_DimRow, pRow, nWidth * sizeof (u16));

		for (unsigned nCol = 0; nCol < nWidth; nCol++)
		{
			u16 nPixel = s_DimRow[nCol];

			unsigned nR = ((nPixel >> 11) & 0x1f) * nKeep / 100;
			unsigned nG = ((nPixel >>  5) & 0x3f) * nKeep / 100;
			unsigned nB = ( nPixel        & 0x1f) * nKeep / 100;

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
