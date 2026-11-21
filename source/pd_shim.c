/* pd_shim.c -- Project Dream's loader (libil2cpp.so) on the Switch (see pd_shim.h)
 *
 * Project Dream's Android build replaces Unity's libil2cpp.so with its own
 * loader: it maps the Mac game's arm64 GameAssembly (a Mach-O) and runs it
 * over bionic, with Darwin's libc, pthreads, Mach calls, objc and
 * CoreFoundation emulated. Almost all of it works here as it is (our bionic
 * shim answers it like an Android phone). Three of its OS-specific parts do
 * not, and are the Switch's own here:
 *
 *  - Code memory. It maps the Mach-O with mmap (read-write), fills it in and
 *    makes its code read-execute with mprotect; its game patches (gp_*) make
 *    code pages writable again, patch them and make them executable. Horizon
 *    has no executable heap: here those mappings are process code memory
 *    (svcMapProcessCodeMemory) at their final address, and mprotect switches
 *    a range between read-write and read-execute (svcSetProcessMemoryPermission;
 *    a read-write-execute request becomes read-write: it is always followed by
 *    the writes, then a read-execute one). The kernel lets a page of code
 *    memory become writable once only (code -> code data, which stays; a
 *    second write request is refused, and Ryubing stops on it): a page asked
 *    to be writable again is unmapped and mapped again from its source pages
 *    (which hold what it held), code once more, then made writable. That
 *    happens in gp_initialize's patches only, once, while the game starts.
 *    The game's own Darwin mmap calls
 *    (d_mmap, from GameAssembly: its GC heap and allocators) stay ordinary
 *    memory (libc_shim.c). Its stubs for imports nobody implements (trap_make,
 *    written into a read-write-execute pool) are made here in so_util's code
 *    memory.
 *
 *  - Stopping threads. The Mac game's Boehm GC stops the world with Mach's
 *    thread_suspend / thread_get_state / thread_resume; the loader does them
 *    with signals (a handler that saves the registers and parks). Horizon has
 *    no signals: d_thread_suspend pauses the thread (svcSetThreadActivity),
 *    reads its registers (svcGetThreadContext3) into the loader's own thread
 *    record where its d_thread_get_state reads them, d_thread_resume lets it
 *    run. As mkt_nx's gcstop.c does for Android's GC.
 *
 *  - Its Java side. GameActivity's ControllerInput publishes the controllers
 *    over JNI (ControllerInput.publish, Apple GCExtendedGamepad's 25 slots):
 *    the console's controller is published here every frame it changes.
 *    Haptics.register asks for the static Java method Haptics.play(kind):
 *    here HD rumble pulses (rumble.c).
 *
 * The loader is not stripped: every function and variable is found by name in
 * its symbol table (read from the file), and each one used is checked to exist
 * before anything is hooked.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <elf.h>
#include <malloc.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "jni.h"
#include "libc_shim.h"
#include "log.h"
#include "paths.h"
#include "pd_shim.h"
#include "pthr.h"
#include "rumble.h"
#include "so_util.h"

#define PAGE 0x1000
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uintptr_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uintptr_t)(a) - 1))

#define PD_LIB "libil2cpp.so"

static so_module *s_mod;
static uintptr_t s_base;
static int s_hooked;

// --- its symbol table --------------------------------------------------------------------

typedef struct {
  const char *name;
  uint64_t value, size;
} PdSym;

static PdSym *s_syms;
static int s_nsyms;
static char *s_strtab;

static int read_symtab(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  Elf64_Ehdr eh;
  int ok = -1;
  Elf64_Shdr *sh = NULL;
  Elf64_Sym *syms = NULL;
  if (fread(&eh, sizeof eh, 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || !eh.e_shoff)
    goto out;
  sh = malloc(sizeof(*sh) * eh.e_shnum);
  if (!sh || fseek(f, (long)eh.e_shoff, SEEK_SET) != 0 || fread(sh, sizeof(*sh), eh.e_shnum, f) != eh.e_shnum)
    goto out;
  for (int i = 0; i < eh.e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh.e_shnum)
      continue;
    const Elf64_Shdr *st = &sh[sh[i].sh_link];
    const int n = (int)(sh[i].sh_size / sizeof(Elf64_Sym));
    syms = malloc(sh[i].sh_size);
    s_strtab = malloc(st->sh_size + 1);
    if (!syms || !s_strtab || fseek(f, (long)sh[i].sh_offset, SEEK_SET) != 0 ||
        fread(syms, sizeof(Elf64_Sym), n, f) != (size_t)n || fseek(f, (long)st->sh_offset, SEEK_SET) != 0 ||
        fread(s_strtab, 1, st->sh_size, f) != st->sh_size)
      goto out;
    s_strtab[st->sh_size] = '\0';
    s_syms = calloc(n, sizeof(PdSym));
    if (!s_syms)
      goto out;
    for (int k = 0; k < n; k++) {
      const int type = ELF64_ST_TYPE(syms[k].st_info);
      if ((type != STT_FUNC && type != STT_OBJECT) || !syms[k].st_value || syms[k].st_name >= st->sh_size)
        continue;
      s_syms[s_nsyms++] = (PdSym){ s_strtab + syms[k].st_name, syms[k].st_value, syms[k].st_size };
    }
    ok = 0;
    break;
  }
out:
  free(syms);
  free(sh);
  fclose(f);
  return ok;
}

// the runtime address of one of its functions or variables (0: none), and its size
static uintptr_t pd_sym(const char *name, size_t *size) {
  for (int i = 0; i < s_nsyms; i++)
    if (!strcmp(s_syms[i].name, name)) {
      if (size)
        *size = (size_t)s_syms[i].size;
      return s_base + (uintptr_t)s_syms[i].value;
    }
  return 0;
}

// --- code memory ------------------------------------------------------------------------------

// Project Dream's loader asks for its hook trampolines a page at a time: about 220 regions at the title (one 119 MB
// image, the rest single pages). A region past the table would never be made executable on the console.
#define MAX_REGIONS 4096

typedef struct {
  uintptr_t addr;
  size_t len;
  uintptr_t src;  // the heap pages it is mapped from (they keep its contents)
  uint8_t *perm;  // each page's permission now: Perm_Rw, Perm_Rx or Perm_R
} Region;

static Region s_regions[MAX_REGIONS];
static int s_nregions;
static Mutex s_cm_lock;
static size_t s_code_bytes;
static unsigned s_remaps;
// d_mmap: the game's own Darwin mmap calls, which stay ordinary memory
static uintptr_t s_dmmap_lo, s_dmmap_hi;

static Region *region_of(uintptr_t a, size_t len) {
  for (int i = 0; i < s_nregions; i++)
    if (a >= s_regions[i].addr && a + len <= s_regions[i].addr + s_regions[i].len)
      return &s_regions[i];
  return NULL;
}

// fresh zeroed process code memory at an address of its own, read-write
static void *code_region_new(size_t len) {
  len = ALIGN_UP(len, PAGE);
  void *src = memalign(PAGE, len);
  uint8_t *perm = malloc(len / PAGE);
  if (!src || !perm) {
    free(src);
    free(perm);
    return NULL;
  }
  memset(src, 0, len);
  armDCacheFlush(src, len);
  virtmemLock();
  void *dst = virtmemFindCodeMemory(len, PAGE);
  Result rc = dst ? svcMapProcessCodeMemory(so_own_process(), (u64)dst, (u64)src, len)
                  : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  virtmemUnlock();
  if (R_SUCCEEDED(rc))
    rc = svcSetProcessMemoryPermission(so_own_process(), (u64)dst, len, Perm_Rw);
  if (R_FAILED(rc)) {
    log_printf("[pd] code memory of %zu KB: 0x%x", len >> 10, rc);
    free(src); // (only when the mapping failed: a mapped source belongs to it)
    free(perm);
    return NULL;
  }
  memset(perm, Perm_Rw, len / PAGE);
  mutexLock(&s_cm_lock);
  const int tracked = s_nregions < MAX_REGIONS;
  if (tracked)
    s_regions[s_nregions++] = (Region){ (uintptr_t)dst, len, (uintptr_t)src, perm };
  s_code_bytes += len;
  const int nregions = s_nregions;
  mutexUnlock(&s_cm_lock);
  if (!tracked) { // (it stays read-write: what runs from it faults)
    log_printf("[pd] code memory %p +%zu KB: more than %d regions, not tracked", dst, len >> 10, MAX_REGIONS);
    free(perm);
  }
  if (nregions % 100 == 0)
    log_printf("[pd] %d code memory regions", nregions);
  if (len >= (1u << 20) || config.log_diag)
    log_printf("[pd] code memory %p +%zu KB (%zu KB in all)", dst, len >> 10, s_code_bytes >> 10);
  return dst;
}

// The kernel's rules for this memory (Mesosphere's KPageTableBase::SetProcessMemoryPermission, UnmapCodeMemory),
// which Ryubing does not keep (its hypervisor maps everything RWX and ignores permission changes):
//  - svcMapProcessCodeMemory maps the pages as AliasCode; svcSetProcessMemoryPermission gives AliasCode pages R or
//    RX freely, or RW once: RW turns them into AliasCodeData;
//  - AliasCodeData pages cannot change permission again (0xd401, invalid current memory: the first hardware run
//    failed every "make it executable" after the loader wrote the image, and jumped into non-executable code).
//    Unmapping and mapping them from their source again makes them AliasCode (the same physical pages: the bytes
//    stay).
// So RW -> R/RX = reopen_pages, then the permission; R/RX -> RW or R <-> RX = the permission alone.

// pages [a, a + n) of r code again (AliasCode): unmapped and mapped from their source
static Result reopen_pages(Region *r, uintptr_t a, size_t n) {
  const uintptr_t src = r->src + (a - r->addr);
  Result rc = svcUnmapProcessCodeMemory(so_own_process(), a, src, n);
  if (R_SUCCEEDED(rc))
    rc = svcMapProcessCodeMemory(so_own_process(), a, src, n);
  s_remaps++;
  return rc;
}

// what the kernel says about the page at a (for the log)
static void log_page(const char *what, uintptr_t a, size_t n, Result rc) {
  MemoryInfo mi = { 0 };
  u32 pi = 0;
  svcQueryMemory(&mi, &pi, a);
  log_printf("[pd] %s %p +0x%zx: 0x%x (block %llx +%llx, type %x, perm %x, attr %x)", what, (void *)a, n, rc,
             (unsigned long long)mi.addr, (unsigned long long)mi.size, mi.type, mi.perm, mi.attr);
}

// ---- the loader's atomics stubs: a fast path for the garbage collector's write barrier
//
// The Mac code's ARMv8.1 atomics (LSE) are rewritten by the loader into branches to stubs of its own at the start of
// the image's code memory (offset 0 to 0x40000; the image itself from 0x40000): 538 of them, each saving scratch
// registers below the Darwin red zone and doing the operation as an exclusive load/store loop. One of them was 2-3%
// of the main thread's time on the console (round 13, at 1785 MHz): Boehm's GC_end_stubborn_change, the write barrier
// of the incremental collector, ORs a page's dirty bit into GC_dirty_pages on every pointer store (relaxed LDSET),
// and the other cores' barriers write the same words, so the cache lines go back and forth between the cores too.
// The bit is nearly always set already. Each relaxed 64-bit LDSET stub gets a copy with a plain load first: when
// every bit of the operand is set in the word already, the OR would write the same value, and the stub returns the
// value without the exclusive loop (a relaxed RMW that changes nothing is a relaxed load: no ordering is lost; the
// flags are not touched). The copies go at the top of the stub area (unused), the old stub's first instruction
// branches to its copy. Done once, while the stubs' pages are still writable, as the loader makes them executable
// (before any of the game's code runs). config fast_atomics 0: the loader's stubs as they are.
#define STUB_AREA 0x40000u
static int s_stubs_done;
static unsigned s_stub_top = STUB_AREA / 4; // (the stub area's free words are used from the top down)

static int saved_reg(const uint32_t *s, unsigned reg) {
  return (s[1] & 31) == reg || ((s[1] >> 10) & 31) == reg || (s[2] & 31) == reg || ((s[2] >> 10) & 31) == reg;
}

static unsigned stubs_fast_or(uint32_t *w, unsigned *seen) {
  const unsigned nw = STUB_AREA / 4;
  unsigned end = nw;
  while (end > 0 && w[end - 1] == 0)
    end--;
  unsigned top = s_stub_top; // the copies, from the top down (16 words each)
  unsigned done = 0;
  *seen = 0;
  for (unsigned i = 0; i + 12 <= end; i++) {
    const uint32_t *s = w + i;
    if (s[0] != 0xd10283ffu) // sub sp, sp, #0xa0
      continue;
    ++*seen;
    if ((s[1] & 0xffff83e0u) != 0xa90003e0u || (s[2] & 0xffff83e0u) != 0xa90103e0u) // stp, stp [sp], [sp, #16]
      continue;
    if ((s[3] & 0xfffffc00u) != 0xc85f7c00u) // ldxr Xr, [Xn] (no acquire)
      continue;
    const unsigned r = s[3] & 31, n = (s[3] >> 5) & 31;
    if ((s[4] & 0xffe0fc00u) != 0xaa000000u || ((s[4] >> 5) & 31) != r) // orr Xw, Xr, Xs
      continue;
    const unsigned src = (s[4] >> 16) & 31, wr = s[4] & 31;
    if ((s[5] & 0xffe0fc00u) != 0xc8007c00u || ((s[5] >> 5) & 31) != n || (s[5] & 31) != wr) // stxr Ws, Xw, [Xn]
      continue;
    const unsigned st = (s[5] >> 16) & 31;
    if (s[6] != (0x35ffffa0u | st)) // cbnz Ws, the ldxr
      continue;
    if ((s[7] & 0xffe0ffe0u) != 0xaa0003e0u || ((s[7] >> 16) & 31) != r) // mov Xt, Xr
      continue;
    if (s[10] != 0x910283ffu || (s[11] & 0xfc000000u) != 0x14000000u) // add sp, sp, #0xa0; b back
      continue;
    if (!saved_reg(s, r) || !saved_reg(s, wr) || !saved_reg(s, st) || wr == src || wr == n || r == n || r == src)
      continue;
    if (top < 16 || top - 16 < end + 16)
      break;
    const unsigned at = top - 16;
    uint32_t *f = w + at;
    f[0] = s[0], f[1] = s[1], f[2] = s[2];
    f[3] = 0xf9400000u | n << 5 | r;               // ldr Xr, [Xn]
    f[4] = 0x8a200000u | r << 16 | src << 5 | wr;  // bic Xw, Xs, Xr: the operand's bits not set yet
    f[5] = 0xb4000000u | 5u << 5 | wr;             // cbz Xw, the mov (all set: no write)
    f[6] = s[3], f[7] = s[4], f[8] = s[5], f[9] = s[6]; // the exclusive loop as it was
    f[10] = s[7], f[11] = s[8], f[12] = s[9], f[13] = s[10];
    const int32_t back = ((int32_t)(s[11] << 6)) >> 6;                      // (words, from the old b)
    const int64_t to = (int64_t)(i + 11) + back - (int64_t)(at + 14);       // the same place, from the copy's b
    f[14] = 0x14000000u | ((uint32_t)to & 0x3ffffffu);
    f[15] = 0xd503201fu; // nop
    w[i] = 0x14000000u | ((uint32_t)((int64_t)at - (int64_t)i) & 0x3ffffffu); // the old stub: to its copy
    top = at;
    done++;
    i += 11;
  }
  s_stub_top = top;
  return done;
}

// ---- hooks in the game's code: HD rumble at the game's moments (sdt_rumble.c)
//
// A branch at each method's first instruction to a stub of its own in the stub area (ga_hook.s: the handler called
// with the method's x0-x2, every register kept, the first instruction run, back to the second). Written with the
// stubs' fast paths, while the loader still has the image's pages writable -- before any of the game's code runs.
// Each method checked by its first instruction (the Mac game's 1.10.1 build; another one: no hook, a log line).
#include "rumble.h"
extern const uint8_t ga_hook_tpl[], ga_hook_displaced[], ga_hook_back[], ga_hook_lit[], ga_hook_tpl_end[];

typedef struct {
  uint32_t at;    // the method in the Mac GameAssembly (its offset in the image is this + STUB_AREA)
  uint32_t first; // its first instruction
  void (*fn)(uint64_t, uint64_t, uint64_t);
  const char *name;
} GaHook;

// (round 18: only the respawn and the goal fired on the console -- OnCharacterTakeDamage, the homing attack's
// ActionOnCharacterHit and the boost trigger are not what play calls; the knockback, a hit object's ReceiveHit, the
// boost's QueueBoost and the swap are. The first ones stay: harmless, each moment spaced.)
// (round 18: only the respawn and the goal fired on the console. What play calls, from Character.TryTakeDamage's own
// branches and a count of 300 methods in the emulator: the hit = Character.TriggerCascade (rings scattered),
// CharacterAbility_Knockback.Trigger, Stumble.TriggerFromHazard, Shield.TryTakeDamage, Character.TriggerHardFailure;
// the boost = its ability's update running (MovementGroundBoostBase / OneShot / RailBoostBase.DoOnUpdate, first frame
// = the boost's start); the air attack = Character.TriggerBurstAttackAir; the swap = Character.BeginSwapStart. The
// round 17 ones stay where harmless, each moment spaced.)
static const GaHook k_ga_hooks[] = {
  { 0x710380, 0xd10383ffu, rumble_on_damage, "Character.TriggerCascade" },
  { 0x722a84, 0xa9bd57f6u, rumble_on_damage, "CharacterAbility_Knockback.Trigger" },
  { 0x7100ec, 0xd10103ffu, rumble_on_damage, "Character.TriggerKnockback" },
  { 0x73cacc, 0xf9404400u, rumble_on_damage, "CharacterEvents.OnCharacterTakeDamage" },
  { 0x729974, 0xa9bd57f6u, rumble_on_stumble, "CharacterAbility_Stumble.TriggerFromHazard" },
  { 0x729058, 0xa9ba6ffcu, rumble_on_shield, "CharacterAbility_Shield.TryTakeDamage" },
  { 0x7101f4, 0xd10143ffu, rumble_on_respawn, "Character.TriggerHardFailure" },
  { 0x75e5e8, 0x6dbb23e9u, rumble_on_respawn, "CharacterState_TriggerRespawn.DoOnEnter" },
  { 0x6ad214, 0xf9401000u, rumble_on_homing_hit, "PlayerDamageableObject.ReceiveHit" },
  { 0x729eec, 0xf9400008u, rumble_on_homing_hit, "CharacterAbility_HomingAttackAirOneShot.ActionOnCharacterHit" },
  { 0x724e3c, 0x6db923e9u, rumble_on_spin_dash, "CharacterAbility_MovementGroundSpinDashCharge.OnLaunch" },
  // (round 19: the boost abilities' DoOnUpdate fired at a jump's peak -- the air attack's path -- and not for the
  // boost: the boost is its states' DoOnEnter, each switching the stamina on, CharacterStamina.Activate's callers)
  { 0xe58e70, 0xa9bc5ff8u, rumble_on_boost_state, "CharacterState_GroundBoostBase.DoOnEnter" },
  { 0xe5f5e4, 0xa9bc5ff8u, rumble_on_rail_boost, "CharacterState_RailMovementBoostBase.DoOnEnter" },
  { 0x5a21c8, 0xa9be4ff4u, rumble_on_boost, "CharacterBoostTrigger.DoTriggerEnter" },
  { 0x712314, 0xa9be4ff4u, rumble_on_switch, "Character.BeginSwapStart" },
  // (SwapCharacter past its checks -- swaps on, another character: a swap going ahead)
  { 0x743870, 0xaa0203f4u, rumble_on_switch, "CharacterManager.SwapCharacter" },
  { 0x597dd4, 0xf9400008u, rumble_on_goal, "MissionReachGoalTracker.Action_ReachedGoal" },
};

static unsigned ga_hooks_install(Region *r) {
  uint32_t *w = (uint32_t *)r->addr;
  const size_t tpl = (size_t)(ga_hook_tpl_end - ga_hook_tpl);
  const unsigned tw = (unsigned)((tpl + 7) / 8 * 2); // (words, the stub 8-byte aligned)
  unsigned done = 0;
  for (size_t i = 0; i < sizeof k_ga_hooks / sizeof *k_ga_hooks; i++) {
    const GaHook *h = &k_ga_hooks[i];
    const uintptr_t off = STUB_AREA + h->at;
    if (off + 4 > r->len || r->perm[off / PAGE] != Perm_Rw || w[off / 4] != h->first) {
      log_printf("[rumble] %s: not the expected code (0x%08x), no rumble there", h->name,
                 off + 4 <= r->len ? w[off / 4] : 0u);
      continue;
    }
    if (s_stub_top < tw + 0x2000)
      break; // (the stub area's own stubs end far below: never)
    s_stub_top -= tw;
    s_stub_top &= ~1u;
    uint32_t *stub = w + s_stub_top;
    memcpy(stub, ga_hook_tpl, tpl);
    const size_t di = (size_t)(ga_hook_displaced - ga_hook_tpl) / 4, bi = (size_t)(ga_hook_back - ga_hook_tpl) / 4;
    stub[di] = h->first;
    const int64_t back = (int64_t)(off / 4 + 1) - (int64_t)(s_stub_top + bi);
    stub[bi] = 0x14000000u | ((uint32_t)back & 0x3ffffffu);
    const uint64_t fn = (uintptr_t)h->fn;
    memcpy((uint8_t *)stub + (ga_hook_lit - ga_hook_tpl), &fn, 8);
    w[off / 4] = 0x14000000u | ((uint32_t)((int64_t)s_stub_top - (int64_t)(off / 4)) & 0x3ffffffu);
    done++;
  }
  log_printf("[rumble] %u of the game's moments hooked for HD rumble", done);
  return done;
}

#define LINUX_PROT_READ 1
#define LINUX_PROT_WRITE 2
#define LINUX_PROT_EXEC 4
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED 0x10

static void *pd_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
  const uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  // the game's own Darwin mmap, a file mapping, or a fixed one: ordinary memory
  if ((lr >= s_dmmap_lo && lr < s_dmmap_hi) || !(flags & LINUX_MAP_ANONYMOUS) || (flags & LINUX_MAP_FIXED) || !s_hooked)
    return mmap_fake(addr, len, prot, flags, fd, off);
  void *p = code_region_new(len);
  if (!p) {
    errno = ENOMEM;
    return (void *)-1;
  }
  return p;
}

static int pd_mprotect(void *addr, size_t len, int prot) {
  const uintptr_t a = ALIGN_DOWN((uintptr_t)addr, PAGE);
  const size_t n = ALIGN_UP((uintptr_t)addr + len, PAGE) - a;
  mutexLock(&s_cm_lock);
  Region *r = region_of(a, n);
  if (!r) {
    mutexUnlock(&s_cm_lock);
    return mprotect_fake(addr, len, prot);
  }
  // read-write-execute: writes come next (then a read-execute request)
  const u32 perm = (prot & LINUX_PROT_WRITE) ? Perm_Rw : (prot & LINUX_PROT_EXEC) ? Perm_Rx : Perm_R;
  Result rc = 0;
  // the image's atomics stubs made executable: a fast path for the write barrier's first (fast_atomics)
  if (perm == Perm_Rx && !s_stubs_done && a == r->addr && r->len > (16u << 20)) {
    s_stubs_done = 1;
    int rw = n >= STUB_AREA && *(const uint32_t *)r->addr == 0xd10283ffu;
    for (size_t i = 0; rw && i < STUB_AREA / PAGE; i++)
      rw = r->perm[i] == Perm_Rw;
    unsigned seen = 0, fast = 0;
    if (rw && config.fast_atomics)
      fast = stubs_fast_or((uint32_t *)r->addr, &seen);
    if (rw)
      ga_hooks_install(r);
    log_printf("[pd] atomics stubs at %p (made executable +0x%zx): %s%u of %u given a fast path (relaxed 64-bit OR)",
               (void *)r->addr, n, !rw ? "not the stubs' pages as expected; " : !config.fast_atomics ? "fast_atomics 0; " : "",
               fast, seen);
  }
  // runs of pages that change, each with one permission now
  for (uintptr_t p = a; p < a + n && R_SUCCEEDED(rc);) {
    const size_t i0 = (p - r->addr) / PAGE;
    const u8 was = r->perm[i0];
    if (was == perm) {
      p += PAGE;
      continue;
    }
    uintptr_t q = p + PAGE;
    while (q < a + n && r->perm[(q - r->addr) / PAGE] == was)
      q += PAGE;
    if (was == Perm_Rw) {
      // written pages: to the point of unification before they are code again, then AliasCode again
      armDCacheFlush((void *)p, q - p);
      rc = reopen_pages(r, p, q - p);
      if (R_FAILED(rc) || s_remaps <= 4 || config.log_diag > 1)
        log_page("code pages mapped again", p, q - p, rc);
    }
    if (R_SUCCEEDED(rc))
      rc = svcSetProcessMemoryPermission(so_own_process(), p, q - p, perm);
    if (R_FAILED(rc))
      log_page(perm == Perm_Rw ? "code pages made writable" : "code pages made read-only/executable", p, q - p, rc);
    if (R_SUCCEEDED(rc)) {
      memset(r->perm + i0, (int)perm, (q - p) / PAGE);
      if (perm == Perm_Rx)
        armICacheInvalidate((void *)p, q - p);
    }
    p = q;
  }
  mutexUnlock(&s_cm_lock);
  if (R_FAILED(rc)) {
    log_printf("[pd] mprotect %p +0x%zx %d: 0x%x", (void *)a, n, prot, rc);
    errno = EACCES;
    return -1;
  }
  return 0;
}

static int pd_munmap(void *addr, size_t len) {
  mutexLock(&s_cm_lock);
  const Region *r = region_of((uintptr_t)addr, len ? len : 1);
  mutexUnlock(&s_cm_lock);
  if (r)
    return 0; // (code memory stays: the loader unmaps only on failure)
  return munmap_fake(addr, len);
}

// the CPU features the loader is told (getauxval(AT_HWCAP)): it rewrites the
// Mac code's instructions the CPU lacks (LSE atomics 8, LRCPC 15, SHA3 17,
// dot product 20) into its own stubs. The Switch's A57 has none of them: 0.
// debug (config pd_hwcap): an emulator on an Apple CPU runs them as they are.
static unsigned long pd_getauxval(unsigned long type) {
  return type == 16 ? (unsigned long)config.pd_hwcap : 0;
}

// fputc for the loader: its key-value store (kv_put: the game's iCloud store, PCSaveData/icloud_kvs.txt) writes the
// whole file -- 93 KB, the save's JSON in it -- a character at a time, and does so each time the game sets a value
// (11 times in 10 s of play, round 14): newlib's fputc takes the FILE's lock per character, 1-1.3% of the main
// thread's time in profile windows and a hitch of its own each time. The loader's files are its own, each written
// by the function that opened it: the unlocked put. (stdio's fake streams: dropped, as fputc_fake does.)
extern uint8_t fake_sF[3][0x100]; // (libc_shim.c: bionic's stdin/stdout/stderr)
static int pd_fputc(int c, FILE *f) {
  if ((uint8_t *)f >= &fake_sF[0][0] && (uint8_t *)f < &fake_sF[0][0] + sizeof(fake_sF))
    return c;
  return putc_unlocked(c, f);
}

static DynLibFunction s_overrides[] = {
  { "fputc", (uintptr_t)&pd_fputc },
  { "putc", (uintptr_t)&pd_fputc },
  { "getauxval", (uintptr_t)&pd_getauxval },
  { "mmap", (uintptr_t)&pd_mmap },
  { "mmap64", (uintptr_t)&pd_mmap },
  { "mprotect", (uintptr_t)&pd_mprotect },
  { "munmap", (uintptr_t)&pd_munmap },
};

// trap_make(lib, sym): a stub for an import nothing implements, which reports
// itself (trap_hit) and returns 0 when called; data imports get zeroed memory.
// As the loader makes it, in so_util's code memory instead of its RWX pool.
static uintptr_t s_trap_hit;
static unsigned s_traps;

typedef struct {
  char *lib, *sym;
  int calls;
} TrapInfo;

static void *pd_trap_make(const char *lib, const char *sym) {
  static const struct { const char *p; size_t n; } data[] = {
    { "_OBJC_CLASS_$_", 14 }, { "_OBJC_METACLASS_$_", 18 }, { "__objc_empty_cache", 18 },
    { "___CFConstantString", 19 }, { "_kCF", 4 }, { "__ZTI", 5 }, { "__ZTS", 5 }, { "__ZTV", 5 },
    { "_NSLocale", 9 }, { "_GKPlayerDid", 12 }, { "_NSUbiquitous", 13 },
  };
  for (size_t i = 0; sym && i < sizeof data / sizeof *data; i++)
    if (!strncmp(sym, data[i].p, data[i].n))
      return calloc(1, 512);
  const size_t sl = sym ? strlen(sym) : 0;
  if (sl >= 5 && !strcmp(sym + sl - 4, "2idE"))
    return calloc(1, 512);
  TrapInfo *info = malloc(sizeof *info);
  info->lib = lib ? strdup(lib) : NULL;
  info->sym = sym ? strdup(sym) : NULL;
  info->calls = 0;
  void *stub = so_alloc_code(32);
  if (!stub)
    return NULL;
  struct {
    uint32_t code[4];
    uint64_t info, hit;
  } s = { { 0x58000080u /* ldr x0, #16 */, 0x580000b0u /* ldr x16, #20 */, 0xd61f0200u /* br x16 */,
            0xd503201fu /* nop */ },
          (uint64_t)(uintptr_t)info, s_trap_hit };
  so_write_code(stub, &s, sizeof s);
  if (++s_traps <= 40 || config.log_diag)
    log_printf("[pd] unimplemented import %s (%s): a reporting stub", sym ? sym : "?", lib ? lib : "?");
  return stub;
}

// --- stopping threads (the Mac game's GC) ------------------------------------------------------

// the loader's record of a thread (dthread_self: calloc'd, 0x1150 bytes)
typedef struct {
  void *pthread;     // pthread_self()
  int32_t tid;       // gettid()
  uint32_t port;     // its Mach port: 0x1000 + its index in g_threads
  uint64_t stack_lo, stack_hi;
  uint8_t pad32[8];
  int32_t alive;     // 40
  int32_t dead;      // 44: gone (the signal found no thread)
  int32_t count;     // 48: suspend count
  int32_t parked;    // 52: stopped, its registers below are its own
  int32_t resume;    // 56
  int32_t pad60;
  uint64_t x[31];    // 64: x0-x30 (x29 fp, x30 lr)
  uint64_t sp;       // 312
  uint64_t pc;       // 320
  uint64_t pstate;   // 328
} DThread;

_Static_assert(offsetof(DThread, alive) == 40, "dthread layout");
_Static_assert(offsetof(DThread, count) == 48, "dthread layout");
_Static_assert(offsetof(DThread, x) == 64, "dthread layout");
_Static_assert(offsetof(DThread, sp) == 312, "dthread layout");
_Static_assert(offsetof(DThread, pstate) == 328, "dthread layout");

static DThread **s_threads; // g_threads
static int32_t *s_nthreads; // g_nthreads

// each collection's thread handles, read once at its first suspend (when no
// thread is paused yet, so no paused thread can hold the list's lock)
#define MAX_SNAP 256
static pthread_t s_snap_t[MAX_SNAP];
static Handle s_snap_h[MAX_SNAP];
static int s_snap_n;
static int s_paused; // threads paused now
static u64 s_pause_t0, s_pause_ticks, s_pause_max; // the collector's stops of every thread ([gc] lines)
static unsigned s_pause_n;
static Mutex s_gc_lock;
static unsigned s_stops, s_stop_fail;

static DThread *dthread_of(uint32_t port) {
  if (!s_threads || port < 0x1000 || (int32_t)(port - 0x1000) >= *s_nthreads)
    return NULL;
  return s_threads[port - 0x1000];
}

static Handle handle_of(void *pt) {
  for (int i = 0; i < s_snap_n; i++)
    if ((void *)s_snap_t[i] == pt)
      return s_snap_h[i];
  return 0;
}

#define KERN_SUCCESS 0
#define KERN_INVALID_ARGUMENT 4

// debug (log_diag): each C++ exception the Mac game throws, through
// Project Dream's own unwinder (eh_throw: the object, its type_info, ...)
static void name_of(uintptr_t a, char *out, size_t n) {
  uintptr_t base = 0;
  const char *m = so_name_at_unlocked(a, &base);
  if (m)
    snprintf(out, n, "%s+%lx", m, (unsigned long)(a - base));
  else if (!pd_shim_name(a, out, n))
    snprintf(out, n, "%lx", (unsigned long)a);
}

static void on_throw(uint64_t *frame) {
  static unsigned n;
  if (n++ >= 40)
    return;
  (void)frame;
  const char *tname = "?"; // (the type_info is not always at x1: not read)
  char from[96];
  name_of(frame[1], from, sizeof from);
  log_printf("[pd] C++ throw %u: %s from %s (thread %p)", n, tname, from, (void *)pthread_self_soloader());
}

static void on_uncaught(uint64_t *frame) {
  char from[96];
  name_of(frame[1], from, sizeof from);
  log_printf("[pd] uncaught C++ exception (from %s)", from);
}

static void on_resume(uint64_t *frame) {
  static unsigned n;
  if (n++ >= 40)
    return;
  char from[96];
  name_of(frame[1], from, sizeof from);
  log_printf("[pd] unwinder resumes (%s), cursor %p", from, (void *)frame[2]);
}

static void pd_no_patches(void) {
  log_printf("[pd] gp_initialize skipped (config pd_patches 0): Project Dream's game patches are off");
}

static int pd_thread_suspend(uint32_t port) {
  DThread *t = dthread_of(port);
  if (!t || t->pthread == (void *)pthread_self_soloader())
    return KERN_INVALID_ARGUMENT;
  mutexLock(&s_gc_lock);
  if (t->count++ > 0 || t->dead) {
    mutexUnlock(&s_gc_lock);
    return KERN_SUCCESS;
  }
  if (s_paused == 0) {
    uintptr_t lo[MAX_SNAP], hi[MAX_SNAP];
    s_snap_n = pthr_snapshot(s_snap_t, s_snap_h, lo, hi, MAX_SNAP);
  }
  const Handle h = handle_of(t->pthread);
  if (!h) {
    // a thread the port did not start and that is not in the list: finished
    t->dead = 1;
    mutexUnlock(&s_gc_lock);
    return KERN_SUCCESS;
  }
  if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
    t->dead = 1;
    mutexUnlock(&s_gc_lock);
    return KERN_SUCCESS;
  }
  if (s_paused++ == 0)
    s_pause_t0 = armGetSystemTick();
  ThreadContext ctx;
  Result rc = 0;
  for (int tries = 0; tries < 50; tries++) {
    rc = svcGetThreadContext3(&ctx, h);
    if (R_SUCCEEDED(rc))
      break;
    svcSleepThread(100000);
  }
  if (R_FAILED(rc)) {
    // no registers: the GC scans its whole stack from the bottom up instead
    if (s_stop_fail++ < 8)
      log_printf("[pd] GC: the registers of thread %d: 0x%x", t->tid, rc);
    memset(t->x, 0, sizeof t->x);
    t->sp = t->stack_lo;
    t->pc = 0;
    t->pstate = 0;
  } else {
    for (int i = 0; i < 29; i++)
      t->x[i] = ctx.cpu_gprs[i].x;
    t->x[29] = ctx.fp;
    t->x[30] = ctx.lr;
    // Apple's arm64 code may keep values in the 128 bytes below sp (its red
    // zone): the GC scans from there
    uint64_t sp = ctx.sp;
    sp = sp - 128 > t->stack_lo ? sp - 128 : (t->stack_lo ? t->stack_lo : sp);
    t->sp = sp;
    t->pc = ctx.pc.x;
    t->pstate = ctx.psr;
  }
  __atomic_store_n(&t->parked, 1, __ATOMIC_RELEASE);
  if (s_stops++ < 12 || config.log_diag > 1) {
    char pcn[96] = "?";
    pd_shim_name(t->pc, pcn, sizeof pcn);
    uintptr_t base = 0;
    const char *mn = so_name_at_unlocked(t->pc, &base);
    log_printf("[pd] GC stop %u: thread %d (port %x) at %s%s%lx, sp %lx", s_stops, t->tid, t->port, mn ? mn : pcn,
               mn ? "+" : "", mn ? (unsigned long)(t->pc - base) : 0ul, (unsigned long)t->sp);
  }
  mutexUnlock(&s_gc_lock);
  return KERN_SUCCESS;
}

static int pd_thread_resume(uint32_t port) {
  DThread *t = dthread_of(port);
  if (!t)
    return KERN_INVALID_ARGUMENT;
  mutexLock(&s_gc_lock);
  if (t->count > 0 && --t->count > 0) {
    mutexUnlock(&s_gc_lock);
    return KERN_SUCCESS;
  }
  if (t->dead || !t->parked) {
    mutexUnlock(&s_gc_lock);
    return KERN_SUCCESS;
  }
  __atomic_store_n(&t->parked, 0, __ATOMIC_RELEASE);
  const Handle h = handle_of(t->pthread);
  if (h)
    svcSetThreadActivity(h, ThreadActivity_Runnable);
  if (s_paused > 0 && --s_paused == 0 && s_pause_t0) {
    // the world going again: how long the collector held it
    const u64 d = armGetSystemTick() - s_pause_t0;
    s_pause_n++;
    s_pause_ticks += d;
    if (d > s_pause_max)
      s_pause_max = d;
    s_pause_t0 = 0;
  }
  mutexUnlock(&s_gc_lock);
  return KERN_SUCCESS;
}

void pd_shim_gc_stats(unsigned *stops) {
  *stops = s_stops;
}

void pd_shim_gc_pauses(unsigned *n, double *total_ms, double *max_ms) {
  mutexLock(&s_gc_lock);
  *n = s_pause_n;
  *total_ms = armTicksToNs(s_pause_ticks) / 1e6;
  *max_ms = armTicksToNs(s_pause_max) / 1e6;
  s_pause_n = 0;
  s_pause_ticks = s_pause_max = 0;
  mutexUnlock(&s_gc_lock);
}

// a name for an address in the loader's code memory: the Mac game's image
// (the biggest region) as GameAssembly+offset
int pd_shim_name(uintptr_t a, char *out, size_t n) {
  const Region *best = NULL;
  for (int i = 0; i < s_nregions; i++)
    if (!best || s_regions[i].len > best->len)
      best = &s_regions[i];
  for (int i = 0; i < s_nregions; i++)
    if (a >= s_regions[i].addr && a < s_regions[i].addr + s_regions[i].len) {
      snprintf(out, n, "%s+%lx", &s_regions[i] == best ? "GameAssembly" : "pdcode", (unsigned long)(a - s_regions[i].addr));
      return 1;
    }
  return 0;
}

// debug: where the Mac game's il2cpp_class_get_methods is and what its page is
void pd_shim_debug(void) {
  static int done;
  uintptr_t *pp = (uintptr_t *)pd_sym("p_il2cpp_class_get_methods", NULL);
  if (done || !pp || !*pp)
    return;
  done = 1;
  const uintptr_t f = *pp;
  char nm[96] = "?";
  pd_shim_name(f, nm, sizeof nm);
  MemoryInfo mi = { 0 };
  u32 pi = 0;
  svcQueryMemory(&mi, &pi, f);
  log_printf("[pd] il2cpp_class_get_methods = %p (%s): page %llx +%llx type %x perm %x attr %x", (void *)f, nm,
             (unsigned long long)mi.addr, (unsigned long long)mi.size, mi.type, mi.perm, mi.attr);
  for (int i = 0; i < s_nregions && i < 6; i++)
    log_printf("[pd] region %d: %p +%zx (src %p), first page perm %d", i, (void *)s_regions[i].addr, s_regions[i].len,
               (void *)s_regions[i].src, s_regions[i].perm[0]);
  if (f) {
    const Region *r = region_of(f, 4);
    if (r)
      log_printf("[pd] its page's recorded perm %d", r->perm[(f - r->addr) / PAGE]);
    if (mi.perm & Perm_R)
      log_printf("[pd] code there: %08x %08x %08x %08x", ((uint32_t *)f)[0], ((uint32_t *)f)[1], ((uint32_t *)f)[2],
                 ((uint32_t *)f)[3]);
  }
}

// --- the il2cpp API, straight ------------------------------------------------------------------------------------
// Unity reaches the game's runtime through the il2cpp_* functions it looked up in Project Dream's loader. 238 of
// them are the same wrapper: pthread_once(&api_once, do_load) -- the game's image loaded on first use -- then the
// game's own function from a slot, jumped to with the arguments untouched. Unity calls some of them all the time
// (il2cpp_gc_wbarrier_set_field on every object field it stores: 1.4% of the main thread in the wrapper, 0.5% more
// in pthread_once on the console). Once do_load has run, Unity's pointers to those wrappers (its .data/.bss, set
// from dlsym) are made to point at the game's functions themselves.

static int decode_adrp(uintptr_t pc, uint32_t i, int *rd, uintptr_t *page) {
  if ((i & 0x9f000000u) != 0x90000000u)
    return 0;
  int64_t imm = (int64_t)((((i >> 5) & 0x7ffff) << 2) | ((i >> 29) & 3));
  imm = (int64_t)((uint64_t)imm << 43) >> 43; // the 21-bit field, signed: pages
  *rd = (int)(i & 31);
  *page = (pc & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12);
  return 1;
}

// the game's function a wrapper jumps to, or 0 when wrapper is not one of them (or the slot is empty)
static uintptr_t api_target(uintptr_t w, uintptr_t once, uintptr_t do_load) {
  const uint32_t *c = (const uint32_t *)w;
  uintptr_t reg[32] = { 0 };
  int k = 0;
  for (; k < 16; k++) { // up to the call of pthread_once with (once, do_load)
    const uint32_t i = c[k];
    int rd;
    uintptr_t page;
    if (decode_adrp(w + 4 * k, i, &rd, &page)) {
      reg[rd] = page;
    } else if ((i & 0xffc00000u) == 0x91000000u) { // add xd, xn, #imm
      const int d = (int)(i & 31), n = (int)((i >> 5) & 31);
      if (d != 31 && n != 31)
        reg[d] = reg[n] + ((i >> 10) & 0xfff);
    } else if ((i & 0xfc000000u) == 0x94000000u) { // bl
      break;
    }
  }
  if (k == 16 || reg[0] != once || reg[1] != do_load)
    return 0;
  // adrp xa, page; ldr xt, [xa, #off]; cbz xt
  int ra, rt;
  uintptr_t page;
  const uint32_t ld = c[k + 2], cb = c[k + 3];
  if (!decode_adrp(w + 4 * (k + 1), c[k + 1], &ra, &page) || (ld & 0xffc00000u) != 0xf9400000u ||
      (int)((ld >> 5) & 31) != ra || (cb & 0xff00001fu) != (0xb4000000u | (ld & 31)))
    return 0;
  rt = (int)(ld & 31);
  // and the jump: br xt, with nothing but the arguments and the frame restored before it. Some wrappers do more
  // there -- a null method answered without a call (il2cpp_method_get_*), a bool argument cut to its low byte
  // (il2cpp_gchandle_new[_weakref], il2cpp_bounded_array_class_get: Android leaves the upper bits undefined, the
  // Mac code expects them clear), a callback kept -- and must stay in the way. (Round 9: those were bypassed too.)
  int jumps = 0;
  for (int j = k + 4; j < k + 24; j++) {
    const uint32_t i = c[j];
    if (i == (0xd61f0000u | ((uint32_t)rt << 5))) {
      jumps = 1;
      break;
    }
    const int restore =
        (i & 0xffe0ffe0u) == 0xaa0003e0u ||                          // mov xd, xm
        (i & 0xffe0ffe0u) == 0x2a0003e0u ||                          // mov wd, wm
        (i & 0xffc00000u) == 0xa9400000u || (i & 0xffc00000u) == 0xa8c00000u || // ldp x (offset, post-index)
        (i & 0xffc00000u) == 0x6d400000u || (i & 0xffc00000u) == 0x6cc00000u || // ldp d
        (i & 0xffc003e0u) == 0xf94003e0u ||                          // ldr xt, [sp, #imm]
        (i & 0xffe00fe0u) == 0xf84007e0u ||                          // ldr xt, [sp], #imm
        (i & 0xff8003ffu) == 0x910003ffu ||                          // add sp, sp, #imm
        i == 0xd50323bfu;                                            // autiasp
    if (!restore)
      return 0;
  }
  if (!jumps)
    return 0;
  return *(volatile uintptr_t *)(page + ((ld >> 10) & 0xfff) * 8);
}

typedef struct {
  uintptr_t wrapper, target;
} ApiPair;

static int by_wrapper(const void *a, const void *b) {
  const uintptr_t x = ((const ApiPair *)a)->wrapper, y = ((const ApiPair *)b)->wrapper;
  return x < y ? -1 : x > y;
}

int pd_direct_api(uintptr_t lo, uintptr_t hi) {
  static int done;
  if (done || !s_mod)
    return 0;
  const uintptr_t do_load = pd_sym("do_load", NULL);
  if (!do_load)
    return 0;
  // the once control: from the first wrapper's code (adrp/add x0 before the call)
  static ApiPair pairs[512];
  int n = 0;
  uintptr_t once = 0;
  for (int i = 0; i < s_nsyms && n < 512; i++) {
    if (strncmp(s_syms[i].name, "il2cpp_", 7) || !s_syms[i].value)
      continue;
    const uintptr_t w = s_base + (uintptr_t)s_syms[i].value;
    if (!once) { // (found as the x0 of the first wrapper calling with do_load)
      const uint32_t *c = (const uint32_t *)w;
      uintptr_t reg[32] = { 0 };
      for (int k = 0; k < 16; k++) {
        int rd;
        uintptr_t page;
        if (decode_adrp(w + 4 * k, c[k], &rd, &page))
          reg[rd] = page;
        else if ((c[k] & 0xffc00000u) == 0x91000000u && (c[k] & 31) != 31 && ((c[k] >> 5) & 31) != 31)
          reg[c[k] & 31] = reg[(c[k] >> 5) & 31] + ((c[k] >> 10) & 0xfff);
        else if ((c[k] & 0xfc000000u) == 0x94000000u) {
          if (reg[1] == do_load)
            once = reg[0];
          break;
        }
      }
      if (!once)
        continue;
    }
    const uintptr_t t = api_target(w, once, do_load);
    if (t)
      pairs[n++] = (ApiPair){ w, t };
  }
  // do_load done (bionic pthread_once: 2), or not yet: then later
  if (!once || __atomic_load_n((volatile int *)once, __ATOMIC_ACQUIRE) != 2 || n == 0)
    return 0;
  done = 1;
  qsort(pairs, (size_t)n, sizeof *pairs, by_wrapper);
  int changed = 0;
  for (uintptr_t a = (lo + 7) & ~(uintptr_t)7; a + 8 <= hi; a += 8) {
    const uintptr_t v = *(volatile uintptr_t *)a;
    if (v < pairs[0].wrapper || v > pairs[n - 1].wrapper)
      continue;
    const ApiPair key = { v, 0 };
    const ApiPair *p = bsearch(&key, pairs, (size_t)n, sizeof *pairs, by_wrapper);
    if (p) {
      __atomic_store_n((volatile uintptr_t *)a, p->target, __ATOMIC_RELEASE);
      changed++;
    }
  }
  log_printf("[pd] il2cpp API: %d of Unity's pointers to %d of the loader's wrappers now go straight to the game's "
             "functions", changed, n);
  return changed;
}

// Project Dream's own list screens (Achievements; built in place of Game Center's view) are open: their rows move on
// Unity's arrow keys (Input.GetKeyDown UpArrow/DownArrow, W/S), which Android sends for a gamepad's D-pad -- here the
// controller reaches the game only through Project Dream's GameController bridge, so main.c sends them while open
static volatile uint8_t *s_list_flag; // gp_list_open

int pd_list_open(void) {
  if (!s_list_flag && s_mod)
    s_list_flag = (volatile uint8_t *)pd_sym("gp_list_open", NULL);
  return s_list_flag && (*s_list_flag & 1);
}

// one row up (-1) or down (+1) in the open list: Project Dream's own gp_list_select (it clamps to the rows and plays
// its sound), as its tick does for the keys. Driven directly: Android D-pad keys (round 9) reached the game's own
// keyboard navigation too and moved the menu behind (round 10).
void pd_list_move(int step, int sound) {
  static void (*select_row)(int);
  static const volatile int32_t *selected;
  if (!select_row && s_mod) {
    select_row = (void (*)(int))pd_sym("gp_list_select", NULL);
    selected = (const volatile int32_t *)pd_sym("gp_list_selected", NULL);
  }
  if (!select_row || !selected)
    return;
  // gp_list_select plays its tick when the row changes while gp_list_open is set (it reads the flag for that
  // alone): cleared for the call, no tick (main.c: held repeats tick 4 times a second at most -- round 14, holding
  // the stick down the list at 16 rows a second, the soundtrack went silent until it was let go)
  const uint8_t was = s_list_flag ? *s_list_flag : 0;
  if (!sound && s_list_flag)
    *s_list_flag = 0;
  select_row(*selected + step);
  if (!sound && s_list_flag)
    *s_list_flag = was;
}

// Dynamic resolution in Project Dream's Graphics menu: its options table is fixed (26 entries), so its VSync entry
// becomes it -- the Switch's swapchain is held to vsync whatever that option says (Unity's vSyncCount stays 1:
// unity_diag.c), so it never did anything here. Its own key in the options file (dynamic_resolution: Project Dream
// reads and writes the file by these keys), label and text; on by default; Off/On as before. drs.c reads the value.
static uint8_t *s_dyn_entry;
static const char k_dyn_key[] = "dynamic_resolution";
static const char k_dyn_label[] = "Dynamic Resolution";
static const char k_dyn_text[] = "Dynamic Resolution: Lowers the resolution only while the GPU cannot keep up, "
                                 "never above Resolution Scale.";

void unity_find_menu_opened(void); // unity_diag.c
static void menu_opened(uint64_t *frame) {
  (void)frame;
  unity_find_menu_opened();
}

static void dyn_res_option(void) {
  uint8_t *t = (uint8_t *)pd_sym("gp_options", NULL);
  for (int i = 0; t && i < 26; i++) {
    uint8_t *e = t + i * 0x60;
    const char *key = *(const char *const *)e;
    if (key && !strcmp(key, "vsync")) {
      *(const char **)(e + 0x00) = k_dyn_key;
      *(const char **)(e + 0x08) = k_dyn_label;
      *(const char **)(e + 0x10) = k_dyn_text;
      *(int32_t *)(e + 0x1c) = 1; // (its default)
      *(int32_t *)(e + 0x24) = 1; // (its value, until the options file says otherwise)
      s_dyn_entry = e;
      log_printf("[pd] Graphics menu: Dynamic Resolution in VSync's place (option %d)", i);
      return;
    }
  }
  log_printf("[pd] no VSync option in Project Dream's menu: Dynamic Resolution only in config.txt");
}

// Frame Rate in Project Dream's Graphics menu: a slider of 30-120 fps a frame at a time (value + 30), with nothing
// over 60 to be had on the console's 60 Hz screen and nothing between 30 and 60 it can show evenly (a frame one
// refresh, the next two: round 15's stutter at 36-43 fps). Now three choices: 30, 60 and Auto (60, and an even 30
// while 60 is out of reach: drs.c), Auto by default. Its key stays "fps" (the options file's; an old slider value
// over 2 becomes Auto). Two of Project Dream's instructions changed with it:
//  - gp_option_label prints "fps" as value + 30 (a key check, cbz to that): the check passed over, so the choice's
//    label shows as every other option's does;
//  - gp_update sets Application.targetFrameRate to value + 30 every few seconds (the method looked up, cbz past it
//    when not found): that cbz made a b -- the port sets the frame rate from the choice (drs.c), and holds it.
static uint8_t *s_fps_entry;
static const char k_fps_text[] = "Frame Rate: 30 or 60 frames a second, the two the screen shows evenly. Auto: 60, "
                                 "and an even 30 while 60 is out of reach.";
static const char *const k_fps_labels[3] = { "30", "60", "Auto" };
#define PD_FPS_LABEL_CHECK 0x8f270 // gp_option_label: cbz w0 (key "fps") -> the number format
#define PD_FPS_APPLY 0x7bd2c       // gp_update: cbz x0 (no set_targetFrameRate) -> past the call

static void fps_option(void) {
  uint8_t *t = (uint8_t *)pd_sym("gp_options", NULL);
  uint8_t *e = NULL;
  for (int i = 0; t && i < 26 && !e; i++)
    if (*(const char *const *)(t + i * 0x60) && !strcmp(*(const char *const *)(t + i * 0x60), "fps"))
      e = t + i * 0x60;
  uint32_t *label = (uint32_t *)(s_base + PD_FPS_LABEL_CHECK), *apply = (uint32_t *)(s_base + PD_FPS_APPLY);
  if (!e || *label != 0x34000220u || *apply != 0xb4000440u) {
    log_printf("[pd] Frame Rate left as Project Dream's slider (not the expected code: 0x%08x 0x%08x)", *label,
               *apply);
    return;
  }
  const uint32_t nop = 0xd503201fu, b = 0x14000022u; // (b +0x88: cbz's own target)
  if (so_write_code(label, &nop, 4) || so_write_code(apply, &b, 4))
    return;
  *(const char **)(e + 0x10) = k_fps_text;
  *(int32_t *)(e + 0x1c) = 2; // (its default: Auto)
  *(int32_t *)(e + 0x20) = 2; // (the most)
  if (*(int32_t *)(e + 0x24) > 2 || *(int32_t *)(e + 0x24) < 0)
    *(int32_t *)(e + 0x24) = 2;
  for (int c = 0; c < 3; c++)
    *(const char **)(e + 0x28 + 8 * c) = k_fps_labels[c];
  s_fps_entry = e;
  log_printf("[pd] Graphics menu: Frame Rate is 30 / 60 / Auto");
}

// the menu's Frame Rate: 0 = 30, 1 = 60, 2 = Auto; -1 when it is Project Dream's own (the patch not made). An old
// slider value (the options file's, over 2) becomes Auto.
int pd_fps_choice(void) {
  if (!s_fps_entry)
    return -1;
  volatile int32_t *v = (volatile int32_t *)(s_fps_entry + 0x24);
  if (*v < 0 || *v > 2)
    *v = 2;
  return *v;
}

// Settings' Vibration (Project Dream's option "vibration", Off/On): HD rumble on or off (sdt_rumble.c), its text
// saying what it is now
static uint8_t *s_vib_entry;
static const char k_vib_text[] = "Vibration: HD Rumble when you are hit, strike an enemy, boost, switch characters and "
                                 "reach the goal.";

static void vibration_option(void) {
  uint8_t *t = (uint8_t *)pd_sym("gp_options", NULL);
  for (int i = 0; t && i < 26; i++) {
    uint8_t *e = t + i * 0x60;
    if (*(const char *const *)e && !strcmp(*(const char *const *)e, "vibration")) {
      *(const char **)(e + 0x10) = k_vib_text;
      s_vib_entry = e;
      return;
    }
  }
  log_printf("[rumble] no Vibration option in Project Dream's menu: config rumble only");
}

// The first release's defaults for Project Dream's own options, as the port was tested on the console: Chromatic
// Aberration off (a post-processing pass of its own, GPU and CPU), Touch Controls off (the on-screen buttons, for a
// controller). The table's default and its value before the options file is read: a player's own file still wins.
static void release_defaults(void) {
  static const struct { const char *key; int32_t value; } k[] = {
    { "chromatic_aberration", 0 },
    { "touch_controls", 0 },
  };
  uint8_t *t = (uint8_t *)pd_sym("gp_options", NULL);
  for (int i = 0; t && i < 26; i++) {
    uint8_t *e = t + i * 0x60;
    const char *key = *(const char *const *)e;
    for (size_t j = 0; key && j < sizeof k / sizeof *k; j++)
      if (!strcmp(key, k[j].key)) {
        *(int32_t *)(e + 0x1c) = k[j].value;
        *(int32_t *)(e + 0x24) = k[j].value;
      }
  }
}

int pd_vibration_option(void) {
  return s_vib_entry ? *(const volatile int32_t *)(s_vib_entry + 0x24) : -1;
}

// the menu's Dynamic Resolution (0 off, 1 on), -1 when it is not there
int pd_dynamic_res_option(void) {
  return s_dyn_entry ? *(const volatile int32_t *)(s_dyn_entry + 0x24) : -1;
}

// Project Dream's Resolution Scale slider (its options table, gp_options: 26 entries of 0x60 bytes -- key, label,
// text, then page, default, the most and the value at +0x18/+0x1c/+0x20/+0x24; render_scale is the first) as the
// scale it means: value / the most (50 = 1.00, as the menu shows it); -1 when not there. Project Dream applies it to
// URP itself; drs.c keeps to it as the most.
float pd_render_scale_option(void) {
  static const uint8_t *e;
  if (!e && s_mod) {
    const uint8_t *t = (const uint8_t *)pd_sym("gp_options", NULL);
    if (t && *(const char *const *)t && !strcmp(*(const char *const *)t, "render_scale"))
      e = t;
  }
  if (!e)
    return -1.f;
  // (gp_update: (value + 50) / 100, the slider's 0..50 = 0.50..1.00; round 15 -- read as value / most before)
  const int32_t v = *(const volatile int32_t *)(e + 0x24);
  return v >= 0 && v <= 50 ? (float)(v + 50) / 100.f : -1.f;
}

// Project Dream's gp_update sets URP's renderScale to the slider's value whenever the asset holds another, every
// frame (gp_update+0x39cc: b.le past the setter when they are within 0.001). With dynamic resolution the two took
// turns (round 15: "keeps setting its own render scale", the scale switching between the slider's and the port's).
// That one check's b.le made a b: the setter is never reached from there; drs.c keeps the asset at the slider's value
// (times its factor). The slider still applies itself once when changed (its settings rebuild, elsewhere).
#define PD_RESCALE_EVERY_FRAME 0x7c40c
static void pd_patch_rescale(void) {
  uint32_t *at = (uint32_t *)(s_base + PD_RESCALE_EVERY_FRAME);
  if (*at != 0x5400404du) { // b.le +0x808
    log_printf("[pd] gp_update's render-scale check is not the expected code (0x%08x): left as it is", *at);
    return;
  }
  const uint32_t b = 0x14000202u; // b +0x808
  if (so_write_code(at, &b, 4) == 0)
    log_printf("[pd] gp_update's every-frame render scale left to the port (dynamic resolution)");
}

unsigned pd_shim_remaps(void) {
  return s_remaps;
}

// The loader's icall_set_srp_batching passes false to Unity whatever the game asks: the SRP Batcher off, every
// object drawn through the classic per-draw path (800 draws a frame of the first level's colour pass, the main
// thread's largest cost on the console). config srp_batcher 1: the game's own value goes through.
#define UNITY_SET_SRP_BATCHING 0x5c0664 // GraphicsSettings_Set_Custom_PropUseScriptableRenderPipelineBatching(bool)

static void pd_set_srp_batching(uint8_t on) {
  extern so_module *g_unity;
  if (g_unity) {
    log_printf("[pd] the game asks for the SRP Batcher %s", on ? "on" : "off");
    ((void (*)(uint8_t))(g_unity->base + UNITY_SET_SRP_BATCHING))(on);
  }
}

// The loader's bundle_path (icall_bundle_from_file[_async]): an AssetBundle path inside the APK ("<app>/base.apk!/
// assets/aa/...") was copied to files/bundles on first use, then that copy was opened -- on a phone's flash, cheap;
// on the console's SD card the first boot sat on a black screen copying (57 bundles, 12 MB in 45 s, hundreds of MB
// to go). sdt.apk stores every entry uncompressed, which Unity's APK file system reads in place given the path in
// its own form: "jar:file://" + the APK it knows (ApplicationInfo.sourceDir, sdt.apk) + "!/assets/...".
static void *(*s_string_new)(const char *);
static const uint16_t *(*s_string_chars)(void *);
static int32_t (*s_string_length)(void *);
static unsigned s_bundles_in_place;

static void *pd_bundle_path(void *path) {
  if (!path || !s_string_new || !s_string_chars || !s_string_length)
    return path;
  const int32_t n = s_string_length(path);
  const uint16_t *c = s_string_chars(path);
  char in[1024];
  if (n <= 0 || n >= (int32_t)sizeof in || !c)
    return path;
  for (int32_t i = 0; i < n; i++) {
    if (c[i] > 0x7f)
      return path;
    in[i] = (char)c[i];
  }
  in[n] = 0;
  const char *k = strstr(in, ".apk!/assets/");
  if (!k || !strncmp(in, "jar:", 4))
    return path;
  char out[1200];
  // the controller icons: the Switch's (L, R, +, -; built into the NRO, glyphs.c writes them next to it)
  if (strstr(k, "/controllericons/")) {
    const char *base = strrchr(k, '/') + 1;
    snprintf(out, sizeof out, "%s/glyphs/%s", path_base(), base);
    struct stat st;
    if (stat(out, &st) == 0) {
      static int s_logged;
      if (s_logged++ < 4)
        log_printf("[pd] the Switch's button icon: %s", out);
      return s_string_new(out);
    }
  }
  snprintf(out, sizeof out, "jar:file://%s!/assets/%s", path_apk(), k + 13);
  if (s_bundles_in_place++ < 3 || config.log_fs)
    log_printf("[pd] bundle read in place: %s", out);
  return s_string_new(out);
}


// --- set-up -------------------------------------------------------------------------------------

static void load_hook(so_module *m) {
  if (strcmp(m->soname, PD_LIB) != 0 || s_mod)
    return;
  s_mod = m;
  s_base = m->base;
  if (read_symtab(m->path) < 0 || s_nsyms == 0) {
    log_printf("[pd] %s has no symbol table: not Project Dream's loader?", m->path);
    return;
  }
  size_t dmmap_size = 0;
  const uintptr_t dmmap = pd_sym("d_mmap", &dmmap_size);
  const uintptr_t suspend = pd_sym("d_thread_suspend", NULL), resume = pd_sym("d_thread_resume", NULL);
  const uintptr_t trap_make = pd_sym("trap_make", NULL);
  s_trap_hit = pd_sym("trap_hit", NULL);
  s_threads = (DThread **)pd_sym("g_threads", NULL);
  s_nthreads = (int32_t *)pd_sym("g_nthreads", NULL);
  if (!dmmap || !dmmap_size || !suspend || !resume || !trap_make || !s_trap_hit || !s_threads || !s_nthreads) {
    log_printf("[pd] the loader is not the one this port knows (d_mmap %p, d_thread_suspend %p, trap_make %p, "
               "g_threads %p): its Mach-O and GC parts are left as they are",
               (void *)dmmap, (void *)suspend, (void *)trap_make, (void *)s_threads);
    s_threads = NULL;
    return;
  }
  s_dmmap_lo = dmmap;
  s_dmmap_hi = dmmap + dmmap_size;
  hook_arm64(suspend, (uintptr_t)&pd_thread_suspend);
  hook_arm64(resume, (uintptr_t)&pd_thread_resume);
  hook_arm64(trap_make, (uintptr_t)&pd_trap_make);
  if (config.log_diag > 1) {
    const uintptr_t t = pd_sym("eh_throw", NULL), u = pd_sym("uncaught", NULL), r = pd_sym("__unw_resume", NULL);
    if (t) hook_arm64_pre(t, on_throw);
    if (u) hook_arm64_pre(u, on_uncaught);
    if (r) hook_arm64_pre(r, on_resume);
  }
  const uintptr_t srp = pd_sym("icall_set_srp_batching", NULL);
  if (srp && config.srp_batcher)
    hook_arm64(srp, (uintptr_t)&pd_set_srp_batching);
  const uintptr_t bundle_path = pd_sym("bundle_path", NULL);
  s_string_new = (void *(*)(const char *))pd_sym("il2cpp_string_new", NULL);
  s_string_chars = (const uint16_t *(*)(void *))pd_sym("il2cpp_string_chars", NULL);
  s_string_length = (int32_t (*)(void *))pd_sym("il2cpp_string_length", NULL);
  if (bundle_path && s_string_new && s_string_chars && s_string_length)
    hook_arm64(bundle_path, (uintptr_t)&pd_bundle_path);
  else
    log_printf("[pd] no bundle_path: asset bundles are copied out of the APK as they are first loaded");
  dyn_res_option();
  pd_patch_rescale();
  fps_option();
  vibration_option();
  release_defaults();
  // a settings screen opening: Project Dream's button searches go every time again (unity_diag.c's backoff)
  const uintptr_t menu_open = pd_sym("gp_menu_open", NULL);
  if (menu_open && *(const uint32_t *)menu_open == 0xd10483ffu) // sub sp, sp, #0x120
    hook_arm64_pre(menu_open, menu_opened);
  const uintptr_t gp_init = pd_sym("gp_initialize", NULL);
  if (!config.pd_patches && gp_init)
    hook_arm64(gp_init, (uintptr_t)&pd_no_patches);
  s_hooked = 1;
  log_printf("[pd] Project Dream's loader at %p (%d symbols): code memory, GC thread stops, import traps hooked",
             (void *)s_base, s_nsyms);
}

void pd_shim_init(void) {
  mutexInit(&s_cm_lock);
  mutexInit(&s_gc_lock);
  so_set_module_overrides(PD_LIB, s_overrides, sizeof s_overrides / sizeof *s_overrides);
  so_set_patchable(PD_LIB);
  so_set_load_hook(load_hook);
}

// --- the Java side: controllers and haptics -------------------------------------------------------

// Haptics.play(kind): Android's HapticFeedbackConstants as GameActivity's
// Haptics.feedback maps them: 0 clock tick, 1 keyboard tap, 2 virtual key,
// 3 long press, 4 confirm, 5 long press, 6 reject
static jvalue haptics_play(JObj *thiz, JMethod *m, const jvalue *a) {
  (void)thiz;
  const int kind = m->nargs > 0 ? a[0].i : 2;
  static unsigned s_logged;
  if (s_logged++ < 8)
    log_printf("[pd] haptics %d", kind);
  switch (kind) {
    case 0: rumble_play(0, 22.f, 0.10f, 160.f, 0.22f, 320.f, SHAPE_CLICK); break;
    case 1: rumble_play(0, 28.f, 0.15f, 160.f, 0.28f, 320.f, SHAPE_CLICK); break;
    case 2: rumble_play(0, 35.f, 0.22f, 150.f, 0.32f, 300.f, SHAPE_CLICK); break;
    case 4: rumble_play(0, 70.f, 0.35f, 140.f, 0.40f, 280.f, SHAPE_THUD); break;
    case 6: rumble_play(0, 160.f, 0.45f, 120.f, 0.30f, 240.f, SHAPE_ROUGH); break;
    case 3:
    case 5:
    default: rumble_play(0, 120.f, 0.45f, 130.f, 0.35f, 260.f, SHAPE_FADE); break;
  }
  return (jvalue){ 0 };
}

static void (*s_publish)(void *env, void *cls, JObj *ids, JObj *names, JObj *types, JObj *values);

void pd_shim_java_init(void) {
  jni_method("com/sonicdreamteam/android/Haptics", "play", "(I)V", haptics_play);
  if (!s_mod)
    return;
  void (*reg)(void *env, void *cls) = (void *)so_sym(s_mod, "Java_com_sonicdreamteam_android_Haptics_register");
  s_publish = (void *)so_sym(s_mod, "Java_com_sonicdreamteam_android_ControllerInput_publish");
  if (reg) {
    reg(jni_env(), jni_class("com/sonicdreamteam/android/Haptics"));
    log_printf("[pd] Haptics.register: HD rumble for the game's haptics");
  }
  if (!s_publish)
    log_printf("[pd] the loader has no ControllerInput.publish: no controller");
}

// GCExtendedGamepad's slots as ControllerInput fills them
enum {
  SLOT_HOME = 0, SLOT_MENU = 1, SLOT_OPTIONS = 2, SLOT_A = 3, SLOT_B = 4, SLOT_Y = 5, SLOT_X = 6, SLOT_R1 = 7,
  SLOT_R2 = 8, SLOT_L1 = 9, SLOT_L2 = 10, SLOT_DPAD_X = 11, SLOT_DPAD_Y = 12, SLOT_LX = 13, SLOT_LY = 14,
  SLOT_L3 = 15, SLOT_RX = 16, SLOT_RY = 17, SLOT_R3 = 18, SLOTS = 25
};

static float stick(int v) {
  const float f = (float)v / 32767.f;
  const float dz = 0.12f, a = f < 0 ? -f : f;
  if (a <= dz)
    return 0.f;
  const float o = (a - dz) / (1.f - dz);
  return f < 0 ? -(o > 1.f ? 1.f : o) : (o > 1.f ? 1.f : o);
}

void pd_shim_pad(u64 held, int lx, int ly, int rx, int ry, int connected) {
  if (!s_publish)
    return;
  float v[SLOTS] = { 0 };
  if (connected) {
    // Nintendo's labels: A confirms, B goes back (the game's A and B)
    v[SLOT_A] = (held & HidNpadButton_A) ? 1.f : 0.f;
    v[SLOT_B] = (held & HidNpadButton_B) ? 1.f : 0.f;
    v[SLOT_X] = (held & HidNpadButton_X) ? 1.f : 0.f;
    v[SLOT_Y] = (held & HidNpadButton_Y) ? 1.f : 0.f;
    v[SLOT_L1] = (held & HidNpadButton_L) ? 1.f : 0.f;
    v[SLOT_R1] = (held & HidNpadButton_R) ? 1.f : 0.f;
    v[SLOT_L2] = (held & HidNpadButton_ZL) ? 1.f : 0.f;
    v[SLOT_R2] = (held & HidNpadButton_ZR) ? 1.f : 0.f;
    v[SLOT_MENU] = (held & HidNpadButton_Plus) ? 1.f : 0.f;
    v[SLOT_OPTIONS] = (held & HidNpadButton_Minus) ? 1.f : 0.f;
    v[SLOT_L3] = (held & HidNpadButton_StickL) ? 1.f : 0.f;
    v[SLOT_R3] = (held & HidNpadButton_StickR) ? 1.f : 0.f;
    v[SLOT_DPAD_X] = ((held & HidNpadButton_Right) ? 1.f : 0.f) - ((held & HidNpadButton_Left) ? 1.f : 0.f);
    v[SLOT_DPAD_Y] = ((held & HidNpadButton_Up) ? 1.f : 0.f) - ((held & HidNpadButton_Down) ? 1.f : 0.f);
    v[SLOT_LX] = stick(lx);
    v[SLOT_LY] = stick(ly); // (up is positive, as ControllerInput sends it)
    if (pd_list_open()) { // (its rows move by main.c's list_keys; nothing behind it moves)
      v[SLOT_DPAD_X] = v[SLOT_DPAD_Y] = v[SLOT_LX] = v[SLOT_LY] = 0.f;
    }
    v[SLOT_RX] = stick(rx);
    v[SLOT_RY] = stick(ry);
  }
  // only when something changed, and every half second (as ControllerInput's refresh)
  static float last[SLOTS];
  static int last_connected = -1;
  static u64 last_tick;
  const u64 now = armGetSystemTick();
  if (connected == last_connected && !memcmp(v, last, sizeof v) && armTicksToNs(now - last_tick) < 500000000ull)
    return;
  memcpy(last, v, sizeof v);
  last_tick = now;
  if (connected != last_connected)
    log_printf("[pd] controller %s", connected ? "connected" : "gone");
  last_connected = connected;
  const int n = connected ? 1 : 0;
  JObj *ids = jni_array('I', n, NULL), *types = jni_array('I', n, NULL), *values = jni_array('F', n * SLOTS, NULL);
  const char *name = "Nintendo Switch Controller";
  JObj *names = jni_string_array(&name, n);
  if (n) {
    ((int32_t *)jni_arr(ids)->data)[0] = 1;
    ((int32_t *)jni_arr(types)->data)[0] = 0; // (0: generic, 1: DualShock, 2: DualSense)
    memcpy(jni_arr(values)->data, v, sizeof v);
  }
  s_publish(jni_env(), jni_class("com/sonicdreamteam/android/ControllerInput"), ids, names, types, values);
}
