/* saltynx.h -- the FPS and resolution of the console overlays (Status Monitor
 * Overlay and its forks) for this port, through SaltyNX's shared page (saltynx.c). */
#ifndef SALTYNX_H
#define SALTYNX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* At startup: connects to SaltyNX once and finds or makes the FPS block. Without SaltyNX it does nothing. */
void saltynx_init(void);
/* Every presented frame, with the size of the picture the game draws and of the screen: the overlay's "alive"
 * mark, the frame times and the resolutions. Once a second it also publishes the frame rate (and keeps looking
 * for the block while there is none). */
void saltynx_present(uint32_t width, uint32_t height, uint32_t screen_width, uint32_t screen_height);

#ifdef __cplusplus
}
#endif

#endif
