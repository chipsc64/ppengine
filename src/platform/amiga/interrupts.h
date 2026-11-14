#ifndef __PLATFORM_AMIGA_INTERRUPTS_H
#define __PLATFORM_AMIGA_INTERRUPTS_H

#include <exec/types.h>

/* Shared with irq_servers.s: keep the layout (offsets listed there). */
struct irq_state {
	ULONG pending;
	ULONG displayed;
	ULONG flips;
	UBYTE last_raw;
	UBYTE write;
	UBYTE ring[16];
};

extern volatile struct irq_state irq_state;

int interrupts_install(void);   /* VBlank and PORTS servers; OS still running */
void interrupts_remove(void);

#endif
