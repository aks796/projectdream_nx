// ga_hook.s -- the stub a hooked function of the game's code (GameAssembly, Project Dream's code memory) enters by
// a branch at its first instruction (pd_shim.c ga_hooks_install): every argument register saved, the port's handler
// called with x0-x2 as they came, all of them back, then the function's own first instruction (copied in at
// ga_hook_displaced) and a branch back to its second (at ga_hook_back). 128 bytes kept clear below the stack pointer
// first (the Mac code's red zone). Copied into the stub area at the start of the game's code memory, within a branch's
// reach of every function; the handler's address goes at ga_hook_lit.
//
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.

  .section .rodata.ga_hook, "a"
  .balign 8
  .global ga_hook_tpl, ga_hook_displaced, ga_hook_back, ga_hook_lit, ga_hook_tpl_end
ga_hook_tpl:
  sub sp, sp, #0x80
  sub sp, sp, #0x130
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp x8, x9, [sp, #64]
  stp x10, x11, [sp, #80]
  stp x12, x13, [sp, #96]
  stp x14, x15, [sp, #112]
  stp x16, x17, [sp, #128]
  stp x18, x30, [sp, #144]
  stp q0, q1, [sp, #160]
  stp q2, q3, [sp, #192]
  stp q4, q5, [sp, #224]
  stp q6, q7, [sp, #256]
  ldr x16, ga_hook_lit
  blr x16
  ldp q6, q7, [sp, #256]
  ldp q4, q5, [sp, #224]
  ldp q2, q3, [sp, #192]
  ldp q0, q1, [sp, #160]
  ldp x18, x30, [sp, #144]
  ldp x16, x17, [sp, #128]
  ldp x14, x15, [sp, #112]
  ldp x12, x13, [sp, #96]
  ldp x10, x11, [sp, #80]
  ldp x8, x9, [sp, #64]
  ldp x6, x7, [sp, #48]
  ldp x4, x5, [sp, #32]
  ldp x2, x3, [sp, #16]
  ldp x0, x1, [sp, #0]
  add sp, sp, #0x130
  add sp, sp, #0x80
ga_hook_displaced:
  nop
ga_hook_back:
  nop
  .balign 8
ga_hook_lit:
  .quad 0
ga_hook_tpl_end:
