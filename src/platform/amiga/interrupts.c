/*
 * Interrupt servers for the display flip and the keyboard (irq_servers.s).
 *
 * Why: the frame loop cannot be relied on to look at the keyboard or to flip
 * at the right moment when a frame takes longer than a VBlank. The servers
 * read the keyboard byte whenever CIA-A signals one (PORTS, level 2) and at
 * every VBlank, and put the next buffer on screen at the start of a VBlank.
 */

#include <exec/interrupts.h>
#include <exec/nodes.h>
#include <hardware/intbits.h>
#include <proto/exec.h>
#include "platform/amiga/interrupts.h"

extern void irq_vblank_code(void);
extern void irq_ports_code(void);

volatile struct irq_state irq_state;

static struct Interrupt vblank_server;
static struct Interrupt ports_server;
static int installed;

int interrupts_install(void) {
	irq_state.pending = 0;
	irq_state.last_raw = *(volatile UBYTE *)0xBFEC01;
	irq_state.write = 0;

	vblank_server.is_Node.ln_Type = NT_INTERRUPT;
	vblank_server.is_Node.ln_Pri = 0;
	vblank_server.is_Node.ln_Name = (char *)"ppengine vblank";
	vblank_server.is_Data = (APTR)&irq_state;
	vblank_server.is_Code = (void (*)(void))irq_vblank_code;

	ports_server.is_Node.ln_Type = NT_INTERRUPT;
	ports_server.is_Node.ln_Pri = -10;  /* after the OS's CIA handler */
	ports_server.is_Node.ln_Name = (char *)"ppengine keyboard";
	ports_server.is_Data = (APTR)&irq_state;
	ports_server.is_Code = (void (*)(void))irq_ports_code;

	AddIntServer(INTB_VERTB, &vblank_server);
	AddIntServer(INTB_PORTS, &ports_server);
	installed = 1;
	return 1;
}

void interrupts_remove(void) {
	if (!installed) {
		return;
	}
	irq_state.pending = 0;
	RemIntServer(INTB_PORTS, &ports_server);
	RemIntServer(INTB_VERTB, &vblank_server);
	installed = 0;
}
