; ppengine interrupt servers (vasm, Motorola syntax)
;
; Both run as exec interrupt servers (AddIntServer). a1 = is_Data = the
; struct irq_state in platform/amiga/interrupts.h. d0, d1, a0, a1 are
; scratch; return with d0 = 0 and the Z flag set so the rest of the chain
; still runs.
;
; struct irq_state offsets:
;   0  ULONG pending    chunky buffer to show at the next VBlank, 0 = none
;   4  ULONG displayed  buffer on screen
;   8  ULONG flips      buffers put on screen
;  12  UBYTE last_raw   last value read from CIA-A SDR ($BFEC01)
;  13  UBYTE write      ring write index (0-15)
;  14  UBYTE ring[16]   raw keyboard bytes, read by keyboard.c

	SECTION	code,CODE

	XDEF	_irq_vblank_code
	XDEF	_irq_ports_code

; VBlank: show the pending buffer (chunky pointer $DFF1EC, KB
; core/saga-registers/chunky-plane.md), then look at the keyboard.
_irq_vblank_code:
	move.l	(a1),d0
	beq.s	.no_flip
	move.l	d0,$dff1ec
	move.l	d0,4(a1)
	clr.l	(a1)
	addq.l	#1,8(a1)
.no_flip:
	bsr.s	poll_keyboard
	moveq	#0,d0
	rts

; Level 2 (PORTS): CIA-A raises it when the keyboard sends a byte; the
; keyboard handler of the OS takes care of the handshake. We only read SDR,
; never the CIA ICR, so nothing is taken away from the OS.
_irq_ports_code:
	bsr.s	poll_keyboard
	moveq	#0,d0
	rts

poll_keyboard:
	move.b	$bfec01,d0
	cmp.b	12(a1),d0
	beq.s	.same
	move.b	d0,12(a1)
	moveq	#0,d1
	move.b	13(a1),d1
	move.b	d0,14(a1,d1.w)
	addq.b	#1,d1
	and.b	#15,d1
	move.b	d1,13(a1)
.same:
	rts
