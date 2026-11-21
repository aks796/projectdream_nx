/* hook_pre.s -- the stub in front of a function hooked with hook_arm64_pre
 *
 * so_util.c copies this template once per hook into code memory and fills
 * in its two literals: the C function to run first, and the trampoline (the
 * function's moved first instructions, then a branch back into it).
 *
 * The C function gets a pointer to the stub's frame: [0] the caller's x29,
 * [1] the return address into the caller, [2..11] x0-x9 as passed (it may
 * change them). Then the frame is gone and the stub jumps to the
 * trampoline, so the function runs as if it had been called directly: no
 * frame of the port's sits under it when it throws (C++ unwinding, which
 * IL2CPP uses for managed exceptions, finds no unwind tables for the
 * port's code). x16 and x17 are the only registers it leaves changed.
 *
 * Frame: x29, x30, x0-x9 (96 bytes), then q0-q7 (128): 224 bytes.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

	.section .rodata.hook_pre_template, "a", %progbits
	.balign	16
	.global	hook_pre_template
	.global	hook_pre_template_end
hook_pre_template:
	stp	x29, x30, [sp, #-224]!
	mov	x29, sp
	stp	x0, x1, [sp, #16]
	stp	x2, x3, [sp, #32]
	stp	x4, x5, [sp, #48]
	stp	x6, x7, [sp, #64]
	stp	x8, x9, [sp, #80]
	stp	q0, q1, [sp, #96]
	stp	q2, q3, [sp, #128]
	stp	q4, q5, [sp, #160]
	stp	q6, q7, [sp, #192]
	mov	x0, sp
	ldr	x16, 1f
	blr	x16
	ldp	x0, x1, [sp, #16]
	ldp	x2, x3, [sp, #32]
	ldp	x4, x5, [sp, #48]
	ldp	x6, x7, [sp, #64]
	ldp	x8, x9, [sp, #80]
	ldp	q0, q1, [sp, #96]
	ldp	q2, q3, [sp, #128]
	ldp	q4, q5, [sp, #160]
	ldp	q6, q7, [sp, #192]
	ldp	x29, x30, [sp], #224
	ldr	x16, 2f
	br	x16
	.balign	8
1:	.quad	0	// the C function
2:	.quad	0	// the trampoline
hook_pre_template_end:
