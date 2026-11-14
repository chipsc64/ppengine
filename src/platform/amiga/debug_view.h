#ifndef __PLATFORM_AMIGA_DEBUG_VIEW_H
#define __PLATFORM_AMIGA_DEBUG_VIEW_H

#include <inttypes.h>

void debug_view_init(void);                 /* after track_geometry_initialise */
void debug_view_render(uint16_t *buffer);   /* one full frame */

#endif
