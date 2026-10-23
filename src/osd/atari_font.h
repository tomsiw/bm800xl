/*
 * atari_font.h -- the OSD's font.
 *
 * The Atari character set, 8x8 glyphs drawn at twice their size, compiled into
 * the kernel as data.  Nothing is read from a ROM at run time: the menu looks
 * the same whatever OS image is loaded and whatever state the machine is in.
 */

#ifndef ATARI_PI_ATARI_FONT_H
#define ATARI_PI_ATARI_FONT_H

#include <circle/font.h>

/// 16x16 glyphs for characters 0x20..0x7F, for CAtariVideoOut::SetFont().
extern const TFont AtariFont;

#endif
