/*
 * host_services.h -- hooks the kernel installs into the emulation layer.
 *
 * These are not part of the interface the kernel *uses* (that is atari_port.h);
 * they are the pieces the kernel has to *supply*, because they need hardware
 * the emulation layer does not own:
 *
 *   - a monotonic clock and an idle function (the Circle timer),
 *   - the audio output device (a Circle sound device),
 *   - the current keyboard state (USB HID or the GPIO keyboard matrix).
 *
 * They are collected here so that neither atari_port.h nor audio_out.h has to
 * know about the other.
 */

#ifndef ATARI_PI_HOST_SERVICES_H
#define ATARI_PI_HOST_SERVICES_H

#include <stdint.h>

#include "atari_port.h"
#include "audio_out.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Installs the device used for all audio.  Passing NULL disables audio output,
 * and the emulation core then runs silently.
 */
void ATARI_SetHostAudioSink(audio_sink_t *sink);

/*
 * Rebuilds the core's audio timing from a known state.
 *
 * A long blocking operation while the OSD is open -- a settings save or the
 * thirty-second runtime-log write -- can leave the core's own sync buffer full.
 * Sound_Update() then sleeps one hardware buffer at a time inside
 * Atari800_Frame(), and once production and consumption match again a full
 * buffer never drains by itself.  Pausing and rebuilding the sync buffer makes
 * the next frames start from a known fill instead.  The audio device is not
 * reopened: only the core's buffer and its timing estimate are reset.
 */
void ATARI_AudioReset (void);

#ifdef __cplusplus
}
#endif

#endif /* ATARI_PI_HOST_SERVICES_H */
