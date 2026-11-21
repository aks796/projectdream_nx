/* crash.c -- what a crash leaves in debug.log
 *
 * libnx calls __libnx_exception_handler on a CPU exception in any thread of
 * the process (it needs __nx_exception_stack to exist). It writes the fault
 * kind and address, PC and LR as module+offset, every register, and the
 * frame-pointer chain, then lets the exception stand so Atmosphere still
 * writes its own report.
 *
 * Paused threads would never be resumed after a crash during a garbage
 * collection, so none is left paused (gcstop.c keeps no handle open).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "log.h"
#include "so_util.h"

alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static int readable(uintptr_t a, size_t n) {
  MemoryInfo mi;
  u32 pi;
  for (uintptr_t p = a; p < a + n;) {
    if (R_FAILED(svcQueryMemory(&mi, &pi, p)) || mi.type == MemType_Unmapped || !(mi.perm & Perm_R))
      return 0;
    const uintptr_t end = (uintptr_t)(mi.addr + mi.size);
    if (end <= p)
      return 0;
    p = end;
  }
  return 1;
}

static const char *sym(uintptr_t v, char *buf, size_t n) {
  so_dl_info info;
  if (so_dladdr((void *)v, &info) && info.dli_fbase) {
    const char *name = strrchr(info.dli_fname, '/');
    name = name ? name + 1 : info.dli_fname;
    if (info.dli_sname && info.dli_saddr)
      snprintf(buf, n, "%s+0x%lx (%s+0x%lx)", name, (unsigned long)(v - (uintptr_t)info.dli_fbase),
               info.dli_sname, (unsigned long)(v - (uintptr_t)info.dli_saddr));
    else
      snprintf(buf, n, "%s+0x%lx", name, (unsigned long)(v - (uintptr_t)info.dli_fbase));
  } else {
    snprintf(buf, n, "0x%lx", (unsigned long)v);
  }
  return buf;
}

static const char *kind(u32 desc) {
  switch (desc) {
    case ThreadExceptionDesc_InstructionAbort: return "instruction abort";
    case ThreadExceptionDesc_MisalignedPC: return "misaligned PC";
    case ThreadExceptionDesc_MisalignedSP: return "misaligned SP";
    case ThreadExceptionDesc_SError: return "SError";
    case ThreadExceptionDesc_BadSVC: return "bad SVC";
    case ThreadExceptionDesc_Trap: return "trap";
    case ThreadExceptionDesc_Other: return "data abort / other";
    default: return "?";
  }
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  char a[160], b[160];
  u64 tid = 0;
  svcGetThreadId(&tid, CUR_THREAD_HANDLE);
  log_printf("[crash] ================ %s in thread %llu ================", kind(ctx->error_desc),
             (unsigned long long)tid);
  log_printf("[crash] far 0x%lx  esr 0x%x", (unsigned long)ctx->far.x, ctx->esr);
  log_printf("[crash] pc %s", sym(ctx->pc.x, a, sizeof a));
  log_printf("[crash] lr %s", sym(ctx->lr.x, b, sizeof b));
  log_printf("[crash] sp 0x%lx  fp 0x%lx", (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);
  for (int i = 0; i < 29; i += 4) {
    char line[256];
    int o = 0;
    for (int j = i; j < i + 4 && j < 29; j++)
      o += snprintf(line + o, sizeof line - o, "x%-2d %016lx  ", j, (unsigned long)ctx->cpu_gprs[j].x);
    log_printf("[crash] %s", line);
  }
  // the frame-pointer chain (arm64 code keeps frame pointers)
  uintptr_t fp = ctx->fp.x;
  for (int i = 0; i < 24 && fp && readable(fp, 16); i++) {
    const uintptr_t *f = (const uintptr_t *)fp;
    if (!f[1])
      break;
    log_printf("[crash]   #%d %s", i, sym(f[1], a, sizeof a));
    if (f[0] <= fp)
      break;
    fp = f[0];
  }
  log_flush();
  svcBreak(BreakReason_Panic, 0, 0);
}
