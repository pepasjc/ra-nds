@ coopSwitch(u32** save, u32* next): cooperative context switch (coop.c).
@ Each context keeps its own interrupt-disable bits (I, F): calico's code
@ may block with interrupts disabled (its kernel gives every thread its own
@ PSR), and that must not leak into the host or other threads.  The CPU
@ mode stays the caller's: a host may call from system or another mode,
@ and switching modes here would switch banked stack pointers under us.
@ Frame on the stack, lowest first: cpsr, r4-r11, lr
	.arm
	.section .text.coopSwitch, "ax", %progbits
	.global coopSwitch
	.type coopSwitch, %function
coopSwitch:
	push {r4-r11, lr}
	mrs r2, cpsr
	push {r2}
	str sp, [r0]
	mov sp, r1
	pop {r2}
	mrs r3, cpsr
	bic r3, r3, #0xC0
	and r2, r2, #0xC0
	orr r3, r3, r2
	msr cpsr_c, r3
	pop {r4-r11, lr}
	bx lr

@ coopCpsr(): the current CPSR (for a new context's initial frame)
	.section .text.coopCpsr, "ax", %progbits
	.global coopCpsr
	.type coopCpsr, %function
coopCpsr:
	mrs r0, cpsr
	bx lr
