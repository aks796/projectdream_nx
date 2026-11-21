/* linux_svc_entry.s -- the way from a patched svc #0 into linux_svc()
 *
 * The linker (so_util.c) made each svc #0 in game code a branch to a stub of
 * its own next to the module:
 *
 *     stp  x16, x30, [sp, #-16]!
 *     ldr  x16, =linux_svc_entry
 *     blr  x16
 *     ldp  x16, x30, [sp], #16
 *     b    <the svc> + 4
 *
 * A Linux system call changes x0 and nothing else, and the code around the
 * svc counts on that (libil2cpp's flattened code keeps values in x9 and up
 * across it). So everything C may clobber is saved here and put back: every
 * general register, the flags, FPSR, FPCR and all of q0-q31 (C keeps only the
 * low halves of q8-q15). Only x0 comes back changed: linux_svc()'s result.
 *
 * Frame: x0-x30, nzcv, fpsr, fpcr (34 x 8 = 272 bytes), then q0-q31 (512).
 * x30 in it is the return into the stub: linux_svc() finds the site from it.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

	.equ	FRAME_Q, 272
	.equ	FRAME_SIZE, 784

	.section .text.linux_svc_entry, "ax", %progbits
	.global	linux_svc_entry
	.type	linux_svc_entry, %function
	.balign	16
linux_svc_entry:
	sub	sp, sp, #FRAME_SIZE
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	mrs	x9, nzcv
	stp	x30, x9, [sp, #240]
	mrs	x9, fpsr
	mrs	x10, fpcr
	stp	x9, x10, [sp, #256]
	add	x9, sp, #FRAME_Q
	stp	q0, q1, [x9, #0]
	stp	q2, q3, [x9, #32]
	stp	q4, q5, [x9, #64]
	stp	q6, q7, [x9, #96]
	stp	q8, q9, [x9, #128]
	stp	q10, q11, [x9, #160]
	stp	q12, q13, [x9, #192]
	stp	q14, q15, [x9, #224]
	stp	q16, q17, [x9, #256]
	stp	q18, q19, [x9, #288]
	stp	q20, q21, [x9, #320]
	stp	q22, q23, [x9, #352]
	stp	q24, q25, [x9, #384]
	stp	q26, q27, [x9, #416]
	stp	q28, q29, [x9, #448]
	stp	q30, q31, [x9, #480]

	mov	x0, sp
	bl	linux_svc
	str	x0, [sp, #0]

	add	x9, sp, #FRAME_Q
	ldp	q0, q1, [x9, #0]
	ldp	q2, q3, [x9, #32]
	ldp	q4, q5, [x9, #64]
	ldp	q6, q7, [x9, #96]
	ldp	q8, q9, [x9, #128]
	ldp	q10, q11, [x9, #160]
	ldp	q12, q13, [x9, #192]
	ldp	q14, q15, [x9, #224]
	ldp	q16, q17, [x9, #256]
	ldp	q18, q19, [x9, #288]
	ldp	q20, q21, [x9, #320]
	ldp	q22, q23, [x9, #352]
	ldp	q24, q25, [x9, #384]
	ldp	q26, q27, [x9, #416]
	ldp	q28, q29, [x9, #448]
	ldp	q30, q31, [x9, #480]
	ldp	x9, x10, [sp, #256]
	msr	fpsr, x9
	msr	fpcr, x10
	ldr	x9, [sp, #248]
	msr	nzcv, x9
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #FRAME_SIZE
	ret
	.size	linux_svc_entry, . - linux_svc_entry
