//
// video_out.h
//
// Screen output for the bare-metal Atari XL/XE emulator.
//
// The emulated machine produces a 336x240 field of Atari colour indices.  This
// module turns that into an HDMI picture that fills a modern monitor:
//
//   * a 1280x720 (or 1920x1080) RGB565 framebuffer, so the Pi can be plugged
//     straight into any HDMI display;
//   * the Atari picture is scaled to the full screen height and padded left and
//     right with black, which preserves the roughly 4:3 shape of a real Atari
//     display instead of stretching it to 16:9;
//   * the whole 240-line field is kept, so PAL machines do not lose the bottom
//     of their display.
//
// The module also provides the few drawing primitives the OSD menu needs, so
// that the menu does not have to know how the framebuffer is laid out.
//
#ifndef _atari_video_out_h
#define _atari_video_out_h

#include <circle/bcmframebuffer.h>
#include <circle/font.h>
#include <circle/types.h>

/// Output resolution.
enum TAtariDisplayMode
{
	AtariDisplayMode720p = 0,	///< 1280x720
	AtariDisplayMode1080p,		///< 1920x1080
	AtariDisplayModeUnknown
};

/// A colour in the framebuffer's native RGB565 format.
typedef u16 TAtariColor;

#define ATARI_COLOR_RGB(r, g, b) \
	((TAtariColor) ((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

#define ATARI_COLOR_BLACK   ATARI_COLOR_RGB (0x00, 0x00, 0x00)
#define ATARI_COLOR_WHITE   ATARI_COLOR_RGB (0xff, 0xff, 0xff)
#define ATARI_COLOR_GREY    ATARI_COLOR_RGB (0x80, 0x80, 0x80)
#define ATARI_COLOR_DARK    ATARI_COLOR_RGB (0x18, 0x18, 0x20)
#define ATARI_COLOR_PANEL   ATARI_COLOR_RGB (0x30, 0x34, 0x40)
#define ATARI_COLOR_SELECT  ATARI_COLOR_RGB (0x00, 0x60, 0xc0)
#define ATARI_COLOR_TITLE   ATARI_COLOR_RGB (0xff, 0xd0, 0x40)

class CAtariVideoOut
{
public:
	CAtariVideoOut (void);
	~CAtariVideoOut (void);

	/// \param Mode Requested output resolution
	/// \return Initialization successful?
	/// \note If the requested resolution is not available the next one down is
	///	  tried, so a 1080p request still produces a picture on a 720p display.
	boolean Initialize (TAtariDisplayMode Mode = AtariDisplayMode720p);

	/// \return Is the framebuffer ready to be drawn into?
	boolean IsInitialized (void) const	{ return m_bInitialized; }

	/// \return The resolution actually obtained
	TAtariDisplayMode GetMode (void) const	{ return m_Mode; }
	unsigned GetWidth (void) const		{ return m_nWidth; }
	unsigned GetHeight (void) const		{ return m_nHeight; }

	/// Width of the scaled Atari picture, excluding the side padding
	unsigned GetPictureWidth (void) const	{ return m_nDestWidth; }

	/// Height of the scaled Atari picture, excluding the top and bottom bars
	unsigned GetPictureHeight (void) const	{ return m_nDestHeight; }

	/*
	 * Where the picture starts, in framebuffer pixels.
	 *
	 * The OSD has to centre on the *picture*, not on the framebuffer: the
	 * picture is a 4:3 rectangle inside a 16:9 display, so it begins at x=160 on
	 * a 1280-wide framebuffer and anything centred on the framebuffer itself sits
	 * visibly to the left of the picture.
	 */
	unsigned GetPictureLeft (void) const	{ return m_nDestLeft; }
	unsigned GetPictureTop (void) const	{ return m_nDestTop; }

	/*
	 * Pixel scaling.
	 *
	 * 0 fits a 4:3 picture to the framebuffer height, which is how the display
	 * has always been scaled; 2 and 3 give every source pixel a square block of
	 * that size instead.  The choice is applied immediately: recomputing the
	 * geometry is arithmetic, and the margins are cleared again on the next fill.
	 *
	 * A factor that does not fit the framebuffer is reduced until it does, so
	 * GetPixelScale() may return less than was asked for; 0 means the 4:3 fit.
	 */
	void SetAspect (int nScale);

	/*
	 * Makes the black margins be cleared again on the next fill.
	 *
	 * Called when the OSD closes: a panel wider than the picture is drawn over the
	 * margins, and those are otherwise written once and never revisited, so the
	 * strips where the panel was would keep its pixels.
	 */
	void InvalidatePadding (void);
	unsigned GetPixelScale (void) const	{ return m_nScale; }

	/*
	 * Presentation
	 */

	/// Converts and scales one Atari field into the framebuffer.
	/// \param pScreen The emulated screen: 240 rows of 384 bytes, one byte per
	///		   pixel holding an Atari colour index.  Columns 24..359
	///		   are the part a real television shows.
	/// \param pPalette 256 entries packed 0x00RRGGBB
	/// \note Does not present the frame; call Present() when the OSD, if any,
	///	  has also been drawn.
	void FillScreen (const u8 *pScreen, const u32 *pPalette);

	/// Makes the frame being drawn the visible one.
	/// \note With double buffering this is a window-offset change and costs
	///	  nothing.  Without it, the picture is already visible and this
	///	  only re-points the composition buffer.
	void Present (void);

	/*
	 * Copies the visible half into the half being composed.	 *
	 * Needed when the picture is about to stop being redrawn but the OSD keeps
	 * being drawn over it -- STATIC_OSD.  Without this the two halves hold
	 * different pictures, so each OSD redraw swaps the background and the area
	 * around the panel appears to flicker between two frames.
	 *
	 * A one-off cost of one buffer's worth of pixels: it happens when the OSD
	 * opens, not per frame.
	 */
	void CopyFrontToBack (void);

	/*
	 * Fills the whole visible screen with one colour and presents it.
	 *
	 * This is a diagnostic, not part of the emulator's output.  It is how the
	 * kernel shows that the framebuffer works and which stage of start-up has
	 * been reached: if the machine stops responding, the colour left on screen
	 * says how far it got, without needing a serial cable or a debugger.
	 *
	 * \note Both halves of the double buffer are filled, so that whichever one
	 *	 is visible shows the colour.
	 */
	void FillSolid (TAtariColor Color);

	/// \return Is the display double buffered (tear-free)?
	boolean IsDoubleBuffered (void) const	{ return m_bDoubleBuffered; }

	/*
	 * Drawing primitives for the OSD menu
	 *
	 * All of these draw into the frame currently being composed, so they must
	 * be called after FillScreen() and before Present().
	 */

	/// Fills a rectangle with a solid colour.
	void DrawRect (unsigned nX, unsigned nY, unsigned nWidth, unsigned nHeight,
		       TAtariColor Color);

	/// Draws a one-pixel rectangle outline.
	void DrawRectOutline (unsigned nX, unsigned nY, unsigned nWidth, unsigned nHeight,
			      TAtariColor Color);

	/// Dims a rectangle towards black, so a menu can be read over the picture.
	void DimRect (unsigned nX, unsigned nY, unsigned nWidth, unsigned nHeight,
		      unsigned nPercent);

	/// Draws a single line of text.
	/// \param nDoubleSize Draw at twice the height, for headings
	void DrawText (unsigned nX, unsigned nY, TAtariColor Color, const char *pText,
		       boolean bDoubleSize = FALSE);

	/*
	 * The font every text call uses.
	 *
	 * Circle supplies several -- Font6x7 through Font12x22 -- and any TFont
	 * table can be passed, so the OSD can be re-fonted without touching the
	 * drawing code.  Callers that lay out rows derive their pitch from
	 * GetTextHeight(), so changing the font here changes the whole menu
	 * coherently.
	 */
	void SetFont (const TFont &rFont)	{ m_pFont = &rFont; }

	/// Height of one text line in pixels.
	unsigned GetTextHeight (void) const;
	/// Width of one character cell in pixels.
	unsigned GetTextWidth (void) const;
	/// Width in pixels of the string PText.
	unsigned GetTextWidth (const char *pText) const;

	/// Draws a filled rectangle behind PText, sized to the text plus margin,
	/// and then the text on top of it.  Used for the menu's selection bar.
	void DrawTextBar (unsigned nX, unsigned nY, TAtariColor BackColor,
			  TAtariColor TextColor, const char *pText);

private:
	// Screen geometry
	void ComputeGeometry (void);

	/* Blacks both halves, for a change that moves the picture rectangle. */
	void ClearBuffers (void);

	// Raw framebuffer access for the current composition buffer
	u16 *BackBuffer (void) const;
	u16 *FrontBuffer (void) const;

	void PutPixel (unsigned nX, unsigned nY, TAtariColor Color);

private:
	CBcmFrameBuffer *m_pFrameBuffer;

	boolean m_bInitialized;
	TAtariDisplayMode m_Mode;

	unsigned m_nWidth;
	unsigned m_nHeight;
	unsigned m_nPitchPixels;	// in pixels, not bytes

	/*
	 * Where the scaled Atari picture sits inside the framebuffer.
	 *
	 * The picture is a 4:3 rectangle, so it may be letterboxed as well as
	 * pillarboxed: on a 16:9 display that is 960x720 inside 1280x720, leaving
	 * bars at the sides only, but on a taller display bars appear top and bottom
	 * as well.
	 */
	unsigned m_nDestLeft;
	unsigned m_nDestTop;
	unsigned m_nDestWidth;
	unsigned m_nDestHeight;

	// Per output column: which of the 336 visible Atari pixels it takes.
	// Per output row: which of the 240 source rows it takes.
	// Both are built once, because the scaling never changes after
	// initialization.  Doing these lookups here rather than in the pixel loop
	// removes a division per destination pixel, which at 960x720 and 50 frames
	// a second is worth having back.
	u16 m_ColumnMap[1920];
	unsigned m_nColumnMapSize;
	u16 m_RowMap[1080];
	unsigned m_nRowMapSize;

	/*
	 * The same mapping as runs, for the scaler.
	 *
	 * Each entry is one source pixel and the number of destination pixels it
	 * owns: at 720p a column owns two or three of the 960, a row always three of
	 * the 720.  Walking these instead of the destination lets the scaling loop
	 * do one palette lookup per source pixel rather than one per destination
	 * pixel, and -- the point -- one dependent chain per source pixel.
	 */
	static const unsigned MaxSourceRuns = 384;	// one per source pixel at most

	u16 m_ColumnRunSource[MaxSourceRuns];
	u8  m_ColumnRunLength[MaxSourceRuns];
	unsigned m_nColumnRuns;

	u16 m_RowRunSource[MaxSourceRuns];
	u16 m_RowRunFirst[MaxSourceRuns];
	u8  m_RowRunLength[MaxSourceRuns];
	unsigned m_nRowRuns;

	u16 *m_pBackBuffer;
	boolean m_bDoubleBuffered;	// frame is composed in an off-screen half
	boolean m_bFrontIsFirst;	// which half the display currently shows

	/*
	 * Whether the black margins beside the picture are already cleared.
	 *
	 * Nothing writes to them after the first frame, so clearing them again is
	 * wasted work -- about a quarter of the pixels in a frame.  Reset by
	 * anything that repaints the whole buffer.
	 */
	/* Requested pixel scale: 0 for the 4:3 fit, 2 or 3 for integer blocks. */
	int		m_nScaleRequest;

	/* TRUE when both halves have to be blanked by the next fill. */
	boolean		m_bClearPending;

	/* The scale actually in use, after fitting: 0 for the 4:3 fit. */
	unsigned	m_nScale;

	boolean m_bPaddingCleared;

	// The font all text is drawn with; see SetFont().
	const TFont *m_pFont;
};

#endif
