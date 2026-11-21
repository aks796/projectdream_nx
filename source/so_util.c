/* so_util.c -- a small dynamic linker for Android arm64 .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * Rewritten for the Geode port. Geode and its mods are ordinary Android
 * shared libraries that need each other, the game, libc++_shared and the
 * "system" libraries, and Geode loads mods at runtime with dlopen. So this is
 * a real (if small) dynamic linker:
 *
 *  - DT_NEEDED dependencies are loaded recursively; a name without a '/'
 *    matches an already loaded module by soname/file name, then the search
 *    directories, then the system libraries (libc, libm, libGLESv2, ...),
 *    which are the import table the port provides.
 *  - Symbols bind like bionic: the module itself, then its dependencies
 *    breadth-first in DT_NEEDED order. Overrides (port hooks) come first.
 *    Lookups use the GNU or SysV hash tables.
 *  - RELA, JMPREL, RELR and Android packed (APS2) relocations.
 *  - dlopen/dlsym/dladdr/dl_iterate_phdr over everything loaded.
 *
 * Code memory. Horizon never gives a page write and execute at once, and
 * once code memory has been written it cannot become executable again. Geode
 * hooks functions at any time, so the read-only + executable pages of the
 * modules that get hooked are a CodeMemory object mapped twice: executable at
 * the module's base, and read-write at rw_alias. so_write_code() patches
 * through the alias. Trampolines come from a CodeMemory pool (so_alloc_code)
 * mapped the same way.
 *
 * The kernel keeps only 10 CodeMemory objects for the whole system
 * (Atmosphere's slab), so only the modules named by so_set_patchable() (the
 * game, its FMOD and Geode) get one, plus the pool, in 16 MB chunks. Every other module
 * -- mods, libc++, fmod -- is ordinary process code memory: code set
 * read-execute, data read-write, no CodeMemory at all (one per module ran out
 * at the 11th module on hardware: svcCreateCodeMemory -> 0xce01). A write
 * into such a module's code is refused (so_write_code returns -1: TulipHook
 * reports the hook as failed) instead of faulting. Data segments are always
 * ordinary process code memory set read-write.
 *
 * Linux system calls. Code that calls the kernel itself (svc #0, the call
 * number in x8) would raise a bad-SVC exception on Horizon. Every svc #0 in a
 * module's executable sections becomes a branch to a stub of its own in the
 * pages just below the image (a branch reaches +-128 MB), which calls
 * linux_svc_entry (linux_svc_entry.s, linux_svc.c) and comes back after the
 * svc with only x0 changed, as from the kernel.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <elf.h>

#include "so_util.h"
#include "error.h"
#include "log.h"

// not in devkitA64's elf.h
#ifndef DT_RELR
#define DT_RELRSZ  35
#define DT_RELR    36
#define DT_RELRENT 37
#endif
#define DT_ANDROID_REL     0x6000000f
#define DT_ANDROID_RELSZ   0x60000010
#define DT_ANDROID_RELA    0x60000011
#define DT_ANDROID_RELASZ  0x60000012
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032
#endif

// bionic <dlfcn.h>
#define BIONIC_RTLD_NOLOAD 4
#define HANDLE_DEFAULT ((void *)0)
#define HANDLE_NEXT    ((void *)-1L)

#define PAGE 0x1000

static so_module *so_list;      // load order
static RMutex so_lock;          // module list, code pool (recursive: init
                                // functions may dlopen)
static char so_error[640];
static __thread char so_dlerror_buf[640];
static __thread int so_dlerror_set;

// a pseudo-module standing for every system library (handles from dlopen
// of libc.so/libGLESv2.so/... and the NULL entries of so_module.needed)
static so_module system_mod = { .soname = "<system>", .refcount = 1, .initialized = 1 };
// bionic's own tiny libstdc++.so (operator new/delete, static-init guards):
// only modules that link it see these, never Geode or libc++_shared users
static so_module stdcxx_mod = { .soname = "libstdc++.so", .refcount = 1, .initialized = 1 };

// --- import tables --------------------------------------------------------------

typedef struct {
  DynLibFunction *funcs;
  size_t n;
} SymTable;

static SymTable sys_table, ovr_table, stdcxx_table;
static uintptr_t (*fallback_resolver)(const char *name);

void so_set_fallback(uintptr_t (*resolver)(const char *name)) {
  fallback_resolver = resolver;
}

static int dynlib_cmp(const void *a, const void *b) {
  return strcmp(((const DynLibFunction *)a)->symbol, ((const DynLibFunction *)b)->symbol);
}

static void table_set(SymTable *t, DynLibFunction *funcs, size_t n) {
  // sorted copy for binary search; on duplicates the last entry wins, like the
  // linear table scan this replaces would have found the first -- warn instead
  t->funcs = malloc(n * sizeof(*funcs));
  memcpy(t->funcs, funcs, n * sizeof(*funcs));
  qsort(t->funcs, n, sizeof(*funcs), dynlib_cmp);
  t->n = n;
  for (size_t i = 1; i < n; i++)
    if (!strcmp(t->funcs[i - 1].symbol, t->funcs[i].symbol))
      log_printf("[linker] duplicate import table entry %s", t->funcs[i].symbol);
}

static uintptr_t table_find(const SymTable *t, const char *name) {
  size_t lo = 0, hi = t->n;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = strcmp(name, t->funcs[mid].symbol);
    if (c == 0)
      return t->funcs[mid].func;
    if (c < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return 0;
}

void so_set_system_symbols(DynLibFunction *funcs, size_t n) { table_set(&sys_table, funcs, n); }

// imports of one module that resolve to the port's functions before anything
// else (sdt_nx: Project Dream's loader gets mmap/mprotect that make code
// memory, pd_shim.c)
#define MAX_MODULE_OVR 4
static struct {
  char name[64];
  SymTable table;
} s_module_ovr[MAX_MODULE_OVR];
static int s_n_module_ovr;

void so_set_module_overrides(const char *name, DynLibFunction *funcs, size_t n) {
  if (s_n_module_ovr >= MAX_MODULE_OVR)
    return;
  snprintf(s_module_ovr[s_n_module_ovr].name, sizeof s_module_ovr[0].name, "%s", name);
  table_set(&s_module_ovr[s_n_module_ovr].table, funcs, n);
  s_n_module_ovr++;
}

static uintptr_t module_override(const so_module *from, const char *name) {
  for (int i = 0; i < s_n_module_ovr; i++) {
    const char *base = strrchr(from->path, '/');
    if (!strcmp(from->soname, s_module_ovr[i].name) || (base && !strcmp(base + 1, s_module_ovr[i].name))) {
      const uintptr_t a = table_find(&s_module_ovr[i].table, name);
      if (a)
        return a;
    }
  }
  return 0;
}
void so_set_overrides(DynLibFunction *funcs, size_t n) { table_set(&ovr_table, funcs, n); }
void so_set_stdcxx_symbols(DynLibFunction *funcs, size_t n) { table_set(&stdcxx_table, funcs, n); }

// names that resolve to the import table instead of a file. libvulkan is
// vkdk (sdt_nx: Unity's Android player draws with Vulkan); libaaudio is
// absent, so Unity's audio falls back to OpenSL ES (opensles.c).
static const char *const system_libs[] = {
  "libc.so", "libm.so", "libdl.so", "liblog.so", "libstdc++.so", "libz.so",
  "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",
  "libandroid.so", "libjnigraphics.so", "libmediandk.so", "libOpenSLES.so",
  "libnativewindow.so", "libsync.so", "libvulkan.so", "libvulkan.so.1",
};

static const char *base_name(const char *path) {
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}

static int is_system_lib(const char *name) {
  const char *b = base_name(name);
  for (size_t i = 0; i < sizeof(system_libs) / sizeof(*system_libs); i++)
    if (!strcmp(b, system_libs[i]))
      return 1;
  return 0;
}

// --- search dirs ---------------------------------------------------------------

#define MAX_SEARCH_DIRS 8
static char *search_dirs[MAX_SEARCH_DIRS];
static int num_search_dirs;

void so_add_search_dir(const char *dir) {
  if (num_search_dirs < MAX_SEARCH_DIRS)
    search_dirs[num_search_dirs++] = strdup(dir);
}

// hbloader passes the real process handle. Started any other way (an
// emulator running the .nro directly) there is none, and the process-code
// calls refuse the pseudo-handle; get a real one the way hbloader does:
// send the pseudo-handle to ourselves over IPC as a copied handle.
static Handle s_own_process = INVALID_HANDLE;

static void own_handle_receiver(void *arg) {
  Handle session = (Handle)(uintptr_t)arg;
  void *tls = armGetTls();
  hipcMakeRequestInline(tls);
  s32 idx = 0;
  if (R_SUCCEEDED(svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX))) {
    HipcParsedRequest r = hipcParseRequest(tls);
    if (r.meta.num_copy_handles == 1)
      s_own_process = r.data.copy_handles[0];
  }
  svcCloseHandle(session);
}

static Handle own_process(void);
Handle so_own_process(void) { return own_process(); }

static Handle own_process(void) {
  if (s_own_process != INVALID_HANDLE)
    return s_own_process;
  s_own_process = envGetOwnProcessHandle();
  if (s_own_process != INVALID_HANDLE)
    return s_own_process;

  Handle server, client;
  Thread t;
  if (R_SUCCEEDED(svcCreateSession(&server, &client, 0, 0))) {
    if (R_SUCCEEDED(threadCreate(&t, own_handle_receiver, (void *)(uintptr_t)server, NULL,
                                 0x1000, 0x20, -2))) {
      threadStart(&t);
      hipcMakeRequestInline(armGetTls(), .num_copy_handles = 1).copy_handles[0] =
          CUR_PROCESS_HANDLE;
      svcSendSyncRequest(client);
      threadWaitForExit(&t);
      threadClose(&t);
    }
    svcCloseHandle(client);
  }
  if (s_own_process == INVALID_HANDLE)
    s_own_process = CUR_PROCESS_HANDLE;
  log_printf("[linker] own process handle 0x%x (from IPC)", s_own_process);
  return s_own_process;
}

const char *so_check_kernel(void) {
  if (!envIsSyscallHinted(0x4B) || !envIsSyscallHinted(0x4C))
    return "svcCreateCodeMemory / svcControlCodeMemory are unavailable.";
  if (!envIsSyscallHinted(0x77) || !envIsSyscallHinted(0x78))
    return "svcMapProcessCodeMemory is unavailable.";
  if (!envIsSyscallHinted(0x73))
    return "svcSetProcessMemoryPermission is unavailable.";
  return NULL;
}

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_error(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  vsnprintf(so_error, sizeof(so_error), fmt, va);
  va_end(va);
  log_printf("[linker] %s", so_error);
}

const char *so_last_error(void) {
  return so_error;
}

// --- image access ----------------------------------------------------------------

// pointer to a link-time vaddr of mod: the heap copy until it is mapped
static inline void *so_ptr(const so_module *m, Elf64_Addr vaddr) {
  return (void *)((m->mapped ? m->base : (uintptr_t)m->src) + vaddr);
}

static inline const Elf64_Sym *sym_at(const so_module *m, size_t i) {
  return (const Elf64_Sym *)so_ptr(m, m->symtab) + i;
}

static inline const char *str_at(const so_module *m, Elf64_Word off) {
  return (const char *)so_ptr(m, m->strtab) + off;
}

// --- symbol lookup --------------------------------------------------------------------

static uint32_t gnu_hash_of(const char *s) {
  uint32_t h = 5381;
  for (const uint8_t *p = (const uint8_t *)s; *p; p++)
    h = (h << 5) + h + *p;
  return h;
}

static uint32_t sysv_hash_of(const char *s) {
  uint32_t h = 0, g;
  for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
    h = (h << 4) + *p;
    g = h & 0xf0000000u;
    h ^= g;
    h ^= g >> 24;
  }
  return h;
}

static int sym_is_export(const Elf64_Sym *s) {
  if (s->st_shndx == SHN_UNDEF)
    return 0;
  const int bind = ELF64_ST_BIND(s->st_info);
  return bind == STB_GLOBAL || bind == STB_WEAK || bind == STB_GNU_UNIQUE;
}

static int is_hidden(const so_module *m, const char *name) {
  for (int i = 0; i < m->num_hidden; i++)
    if (!strcmp(m->hidden[i], name))
      return 1;
  return 0;
}

// exported symbol of one module; for_other: the lookup comes from a
// different module (hidden exports are skipped)
static const Elf64_Sym *module_lookup(const so_module *m, const char *name,
                                      uint32_t gh, uint32_t sh, int for_other) {
  if (m == &system_mod || m == &stdcxx_mod || !m->symtab)
    return NULL;
  const Elf64_Sym *found = NULL;
  if (m->gnu_hash) {
    const uint32_t *ht = so_ptr(m, m->gnu_hash);
    const uint32_t nbuckets = ht[0], symoffset = ht[1];
    const uint32_t bloom_size = ht[2], bloom_shift = ht[3];
    const uint64_t *bloom = (const uint64_t *)(ht + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    const uint64_t word = bloom[(gh / 64) % bloom_size];
    const uint64_t mask = (1ull << (gh % 64)) | (1ull << ((gh >> bloom_shift) % 64));
    if ((word & mask) != mask)
      return NULL;
    uint32_t idx = buckets[gh % nbuckets];
    if (idx < symoffset)
      return NULL;
    for (;; idx++) {
      const Elf64_Sym *s = sym_at(m, idx);
      const uint32_t h2 = chain[idx - symoffset];
      if ((gh | 1) == (h2 | 1) && sym_is_export(s) && !strcmp(name, str_at(m, s->st_name))) {
        found = s;
        break;
      }
      if (h2 & 1)
        break;
    }
  } else if (m->sysv_hash) {
    const uint32_t *ht = so_ptr(m, m->sysv_hash);
    const uint32_t nbucket = ht[0];
    const uint32_t *bucket = ht + 2, *chain = bucket + nbucket;
    for (uint32_t i = bucket[sh % nbucket]; i; i = chain[i]) {
      const Elf64_Sym *s = sym_at(m, i);
      if (sym_is_export(s) && !strcmp(name, str_at(m, s->st_name))) {
        found = s;
        break;
      }
    }
  } else {
    for (size_t i = 1; i < m->nsyms; i++) {
      const Elf64_Sym *s = sym_at(m, i);
      if (sym_is_export(s) && !strcmp(name, str_at(m, s->st_name))) {
        found = s;
        break;
      }
    }
  }
  if (found && for_other && m->num_hidden && is_hidden(m, name))
    return NULL;
  return found;
}

static uintptr_t sym_addr(const so_module *m, const Elf64_Sym *s) {
  if (ELF64_ST_TYPE(s->st_info) == STT_GNU_IFUNC) {
    uintptr_t (*resolver)(void) = (void *)(m->base + s->st_value);
    return resolver();
  }
  return m->base + s->st_value;
}

uintptr_t so_sym(so_module *mod, const char *name) {
  const Elf64_Sym *s = module_lookup(mod, name, gnu_hash_of(name), sysv_hash_of(name), 0);
  return s ? sym_addr(mod, s) : 0;
}

uintptr_t so_sym_req(so_module *mod, const char *name) {
  const uintptr_t a = so_sym(mod, name);
  if (!a)
    fatal_error("Error: could not find symbol\n%s\nin %s", name, mod->soname);
  return a;
}

// Bionic order for an import of `from`: the module itself, then the local
// group breadth-first in DT_NEEDED order (each module once).
#define MAX_BFS 256

static uintptr_t resolve_import(so_module *from, const char *name, so_module **where) {
  uintptr_t a = from && s_n_module_ovr ? module_override(from, name) : 0;
  if (a) {
    if (where) *where = &system_mod;
    return a;
  }
  a = table_find(&ovr_table, name);
  if (a) {
    if (where) *where = &system_mod;
    return a;
  }

  const uint32_t gh = gnu_hash_of(name), sh = sysv_hash_of(name);
  so_module *queue[MAX_BFS];
  int head = 0, tail = 0, system_done = 0;
  queue[tail++] = from;
  while (head < tail) {
    so_module *m = queue[head++];
    if (m == &stdcxx_mod) {
      a = table_find(&stdcxx_table, name);
      if (a) {
        if (where) *where = m;
        return a;
      }
      continue;
    }
    if (m == &system_mod) {
      if (system_done)
        continue;
      system_done = 1;
      a = table_find(&sys_table, name);
      if (a) {
        if (where) *where = &system_mod;
        return a;
      }
      continue;
    }
    const Elf64_Sym *s = module_lookup(m, name, gh, sh, m != from);
    if (s) {
      if (where) *where = m;
      return sym_addr(m, s);
    }
    for (int i = 0; i < m->num_needed && tail < MAX_BFS; i++) {
      so_module *d = m->needed[i] ? m->needed[i] : &system_mod;
      int seen = 0;
      for (int j = 0; j < tail; j++)
        if (queue[j] == d) { seen = 1; break; }
      if (!seen)
        queue[tail++] = d;
    }
  }
  if (fallback_resolver && (a = fallback_resolver(name))) {
    if (where) *where = &system_mod;
    return a;
  }
  return 0;
}

// --- code memory ----------------------------------------------------------------------

#define POOL_CHUNK (16 * 1024 * 1024) // each chunk is a CodeMemory: few, large

typedef struct CodeChunk {
  struct CodeChunk *next;
  uintptr_t rx, rw;
  size_t size, used;
} CodeChunk;

static CodeChunk *code_chunks;
static int codemem_count; // CodeMemory objects this process holds

// the modules whose code gets hooked (CodeMemory); by soname or file name
#define MAX_PATCHABLE 8
static char patchable[MAX_PATCHABLE][128];
static int num_patchable;

static void (*load_hook)(so_module *m);

void so_set_load_hook(void (*hook)(so_module *m)) {
  load_hook = hook;
}

void so_set_patchable(const char *name) {
  if (num_patchable < MAX_PATCHABLE)
    snprintf(patchable[num_patchable++], sizeof(patchable[0]), "%s", name);
}

static char writable_ro[4][64];
static int num_writable_ro;

void so_set_writable_readonly(const char *name) {
  if (num_writable_ro < 4)
    snprintf(writable_ro[num_writable_ro++], sizeof(writable_ro[0]), "%s", name);
}

static int is_writable_ro(const so_module *m) {
  for (int i = 0; i < num_writable_ro; i++)
    if (!strcmp(m->soname, writable_ro[i]))
      return 1;
  return 0;
}

static int is_patchable(const so_module *m) {
  for (int i = 0; i < num_patchable; i++)
    if (!strcmp(m->soname, patchable[i]))
      return 1;
  return 0;
}

// Map size bytes of heap at src as code memory: executable at a fresh
// code-region address, writable at a fresh ASLR-region alias.
static int map_codemem(void *src, size_t size, uintptr_t rx_fixed, uintptr_t *rx_out,
                       uintptr_t *rw_out, Handle *h_out, void **rx_rsv, void **rw_rsv) {
  Handle h;
  Result rc = svcCreateCodeMemory(&h, src, size);
  if (R_FAILED(rc)) {
    set_error("svcCreateCodeMemory(%p, 0x%zx) failed: 0x%x (%d held)", src, size, rc,
              codemem_count);
    return -1;
  }
  codemem_count++;

  virtmemLock();
  void *rw = virtmemFindAslr(size, PAGE);
  void *rwr = rw ? virtmemAddReservation(rw, size) : NULL;
  void *rx = (void *)rx_fixed, *rxr = NULL;
  if (!rx_fixed) {
    rx = virtmemFindCodeMemory(size, PAGE);
    rxr = rx ? virtmemAddReservation(rx, size) : NULL;
  }
  virtmemUnlock();
  if (!rw || !rx) {
    set_error("no address space for 0x%zx bytes of code", size);
    svcCloseHandle(h);
    return -1;
  }

  rc = svcControlCodeMemory(h, CodeMapOperation_MapOwner, rw, size, Perm_Rw);
  if (R_SUCCEEDED(rc))
    rc = svcControlCodeMemory(h, CodeMapOperation_MapSlave, rx, size, Perm_Rx);
  if (R_FAILED(rc)) {
    set_error("svcControlCodeMemory(0x%zx) failed: 0x%x", size, rc);
    return -1;
  }

  log_printf("[linker] code memory %d: 0x%zx bytes at %p", codemem_count, size, rx);
  *rx_out = (uintptr_t)rx;
  *rw_out = (uintptr_t)rw;
  *h_out = h;
  if (rx_rsv) *rx_rsv = rxr;
  if (rw_rsv) *rw_rsv = rwr;
  return 0;
}

void *so_alloc_code(size_t size) {
  size = ALIGN_MEM(size, 16);
  rmutexLock(&so_lock);
  CodeChunk *c = code_chunks;
  if (!c || c->size - c->used < size) {
    const size_t want = size > POOL_CHUNK ? ALIGN_MEM(size, PAGE) : POOL_CHUNK;
    void *src = memalign(PAGE, want);
    c = calloc(1, sizeof(*c));
    Handle h;
    if (!src || !c || map_codemem(src, want, 0, &c->rx, &c->rw, &h, NULL, NULL) < 0) {
      rmutexUnlock(&so_lock);
      log_printf("[linker] code pool exhausted (%zu bytes): %s", size, so_error);
      return NULL;
    }
    c->size = want;
    c->next = code_chunks;
    code_chunks = c;
  }
  void *ret = (void *)(c->rx + c->used);
  c->used += size;
  rmutexUnlock(&so_lock);
  return ret;
}

// writable alias of [addr, addr + size), or NULL when it is not code;
// *fixed: the code of a module mapped without an alias (not patchable)
static void *code_alias(uintptr_t addr, size_t size, so_module **fixed) {
  *fixed = NULL;
  for (so_module *m = so_list; m; m = m->next) {
    if (!m->mapped || addr + size <= m->base || addr >= m->base + m->text_size)
      continue;
    if (m->rw_alias && addr >= m->base && addr + size <= m->base + m->text_size)
      return (void *)(m->rw_alias + (addr - m->base));
    *fixed = m;
    return NULL;
  }
  for (CodeChunk *c = code_chunks; c; c = c->next)
    if (addr >= c->rx && addr + size <= c->rx + c->size)
      return (void *)(c->rw + (addr - c->rx));
  return NULL;
}

int so_write_code(void *dst, const void *src, size_t size) {
  rmutexLock(&so_lock);
  so_module *fixed;
  void *alias = code_alias((uintptr_t)dst, size, &fixed);
  rmutexUnlock(&so_lock);
  if (fixed) {
    // its code is read-execute for good: refuse rather than fault
    log_printf("[linker] a patch of %s's code at +0x%lx refused (not patchable)", fixed->soname,
               (unsigned long)((uintptr_t)dst - fixed->base));
    return -1;
  }
  if (!alias) {
    // not code: data (vtables, GOT) is mapped writable already
    memcpy(dst, src, size);
    return 0;
  }
  memcpy(alias, src, size);
  armDCacheFlush(alias, size);
  armICacheInvalidate(dst, size);
  return 0;
}

// A function that ends (RET, BR, B) within its first 3 instructions is
// shorter than the 16-byte branch: patching it would overwrite the next one
// (IL2CPP folds identical tiny methods, so that one is shared widely).
static int too_short(uintptr_t addr) {
  const uint32_t *in = (const uint32_t *)addr;
  for (int i = 0; i < 3; i++) {
    const uint32_t w = in[i];
    if ((w & 0xfffffc1fu) == 0xd65f0000u || (w & 0xfffffc1fu) == 0xd61f0000u || (w & 0xfc000000u) == 0x14000000u) {
      log_printf("[linker] cannot hook 0x%lx: it ends at instruction %d (0x%08x)", (unsigned long)addr, i, w);
      return 1;
    }
  }
  return 0;
}

static uint64_t *kv_slot(uintptr_t addr, uintptr_t *now);

uintptr_t kv_handler(uintptr_t addr) {
  uintptr_t now = 0;
  return addr && kv_slot(addr, &now) ? now : 0;
}

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  uintptr_t now;
  uint64_t *slot = addr ? kv_slot(addr, &now) : NULL;
  if (slot) { // (a function MKT KV took over: its table slot)
    *slot = dst;
    return;
  }
  if (!addr || too_short(addr))
    return;
  uint32_t hook[4];
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  memcpy(&hook[2], &dst, sizeof(dst));
  so_write_code((void *)addr, hook, sizeof(hook));
}

// The first 4 instructions of the function at addr (the ones hook_arm64
// overwrites), moved into a trampoline that then branches to addr + 16.
// ADRP and ADR become loads of the address they made. Branches become
// absolute ones through X17 (IP1, which nothing keeps across a branch):
// B and BL a load and BR / BLR; B.cond, CBZ / CBNZ and TBZ / TBNZ the
// opposite test skipping over a load and BR. A function whose first
// instructions use X17 themselves, or load a literal, cannot move: 0.
static int mentions_x17(uint32_t w) {
  return (w & 0x1f) == 17 || ((w >> 5) & 0x1f) == 17 || ((w >> 10) & 0x1f) == 17 || ((w >> 16) & 0x1f) == 17;
}

static int64_t sext(uint64_t v, int bits) {
  return (int64_t)(v << (64 - bits)) >> (64 - bits);
}

static uintptr_t relocate_entry(uintptr_t addr) {
  if (too_short(addr))
    return 0;
  uint32_t code[40];
  uint64_t lits[12];
  int fix_at[12], n = 0, nlit = 0;
  const uint32_t *in = (const uint32_t *)addr;
#define LDR_LIT(rd, v) (lits[nlit] = (v), fix_at[nlit++] = n, code[n++] = 0x58000000u | (rd))
#define BR_X17 0xd61f0220u
  int branches = 0, x17 = 0;
  for (int i = 0; i < 4; i++) {
    const uint32_t w = in[i];
    x17 |= mentions_x17(w);
    if ((w & 0x7c000000) == 0x14000000 || (w & 0xff000010) == 0x54000000 || (w & 0x7e000000) == 0x34000000 ||
        (w & 0x7e000000) == 0x36000000)
      branches++;
    if ((w & 0x3b000000) == 0x18000000) { // LDR (literal), PRFM (literal)
      log_printf("[linker] cannot hook 0x%lx: instruction %d (0x%08x) loads a literal", (unsigned long)addr, i, w);
      return 0;
    }
  }
  if (branches && x17) {
    log_printf("[linker] cannot hook 0x%lx: a branch to move and X17 in use", (unsigned long)addr);
    return 0;
  }
  for (int i = 0; i < 4; i++) {
    const uint32_t w = in[i];
    const uintptr_t pc = addr + 4 * (uintptr_t)i;
    if ((w & 0x1f000000) == 0x10000000) { // ADR / ADRP
      const int64_t imm = sext(((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2), 21);
      LDR_LIT(w & 0x1f,
              (w & 0x80000000) ? (pc & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12) : pc + (uintptr_t)imm);
    } else if ((w & 0x7c000000) == 0x14000000) { // B, BL
      LDR_LIT(17, pc + (uintptr_t)(sext(w & 0x3ffffff, 26) << 2));
      code[n++] = (w & 0x80000000) ? 0xd63f0220u : BR_X17; // BLR / BR X17
    } else if ((w & 0xff000010) == 0x54000000) { // B.cond
      const uintptr_t to = pc + (uintptr_t)(sext((w >> 5) & 0x7ffff, 19) << 2);
      if ((w & 0xf) < 0xe)
        code[n++] = 0x54000000u | (3u << 5) | ((w & 0xf) ^ 1); // the opposite: over the next two
      LDR_LIT(17, to);
      code[n++] = BR_X17;
    } else if ((w & 0x7e000000) == 0x34000000) { // CBZ, CBNZ
      const uintptr_t to = pc + (uintptr_t)(sext((w >> 5) & 0x7ffff, 19) << 2);
      code[n++] = ((w ^ 0x01000000u) & ~(0x7ffffu << 5)) | (3u << 5);
      LDR_LIT(17, to);
      code[n++] = BR_X17;
    } else if ((w & 0x7e000000) == 0x36000000) { // TBZ, TBNZ
      const uintptr_t to = pc + (uintptr_t)(sext((w >> 5) & 0x3fff, 14) << 2);
      code[n++] = ((w ^ 0x01000000u) & ~(0x3fffu << 5)) | (3u << 5);
      LDR_LIT(17, to);
      code[n++] = BR_X17;
    } else {
      code[n++] = w;
    }
  }
  LDR_LIT(17, addr + 16); // the way back
  code[n++] = BR_X17;
  // the literals after the code, 8-byte aligned
  if (n & 1)
    code[n++] = 0xd503201fu; // NOP
  const int lit0 = n;
  for (int k = 0; k < nlit; k++) {
    code[fix_at[k]] |= (uint32_t)(lit0 + 2 * k - fix_at[k]) << 5;
    memcpy(&code[n], &lits[k], 8);
    n += 2;
  }
#undef LDR_LIT
#undef BR_X17
  const size_t size = 4 * (size_t)n;
  void *tramp = so_alloc_code(size);
  if (!tramp || so_write_code(tramp, code, size))
    return 0;
  return (uintptr_t)tramp;
}

// MKT KV's libil2cpp: the game functions its libmktkv takes over start with
// "b T", T a trampoline of the mod's: "adrp x16, table; ldr x16, [x16, #off];
// cbz x16, orig; br x16", orig (T + 16) the function's own first instruction
// and a branch back. Such a function is hooked in its table slot (what runs
// now: the mod's, or the function's own through T + 16 when the slot is
// empty), never in its code. The slot, and in *now what it runs; NULL when
// addr is not such a function.
static uint64_t *kv_slot(uintptr_t addr, uintptr_t *now) {
  const uint32_t w = *(const uint32_t *)addr;
  if ((w & 0xfc000000u) != 0x14000000u)
    return NULL;
  const uintptr_t t = addr + (uintptr_t)(sext(w & 0x3ffffff, 26) << 2);
  const uint32_t *tc = (const uint32_t *)t;
  if ((tc[0] & 0x9f00001fu) != 0x90000010u || // ADRP X16
      (tc[1] & 0xffc003ffu) != 0xf9400210u || // LDR X16, [X16, #imm]
      (tc[2] & 0xff00001fu) != 0xb4000010u || // CBZ X16
      tc[3] != 0xd61f0200u)                   // BR X16
    return NULL;
  const int64_t imm = sext(((tc[0] >> 29) & 3) | (((tc[0] >> 5) & 0x7ffff) << 2), 21);
  uint64_t *slot = (uint64_t *)((t & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12) + ((tc[1] >> 10) & 0xfff) * 8);
  *now = *slot ? (uintptr_t)*slot : t + 16;
  log_printf("[linker] 0x%lx is MKT KV's (%s): hooked in its table slot", (unsigned long)addr,
             *slot ? "its own" : "the game's");
  return slot;
}

// A function MKT KV took over: the game's own code for it (its first
// instruction moved into KV's trampoline, then on), whatever KV's table
// slot runs now; 0 when addr is not such a function.
uintptr_t hook_kv_original(uintptr_t addr) {
  const uint32_t w = addr ? *(const uint32_t *)addr : 0;
  if ((w & 0xfc000000u) != 0x14000000u)
    return 0;
  uintptr_t now;
  return kv_slot(addr, &now) ? addr + (uintptr_t)(sext(w & 0x3ffffff, 26) << 2) + 16 : 0;
}

uintptr_t hook_arm64_replace(uintptr_t addr, uintptr_t dst) {
  uintptr_t now = 0;
  uint64_t *slot = addr ? kv_slot(addr, &now) : NULL;
  if (slot) {
    *slot = dst;
    return now;
  }
  const uintptr_t tramp = addr ? relocate_entry(addr) : 0;
  if (tramp)
    hook_arm64(addr, dst);
  return tramp;
}

int hook_arm64_pre(uintptr_t addr, void (*fn)(uint64_t *frame)) {
  extern const uint32_t hook_pre_template[], hook_pre_template_end[];
  const size_t n = (size_t)(hook_pre_template_end - hook_pre_template) * 4;
  uintptr_t now = 0;
  uint64_t *slot = addr ? kv_slot(addr, &now) : NULL;
  const uintptr_t tramp = slot ? now : addr ? relocate_entry(addr) : 0;
  uint8_t *stub = tramp ? so_alloc_code(ALIGN_MEM(n, 16)) : NULL;
  if (!stub)
    return -1;
  uint8_t buf[256];
  memcpy(buf, hook_pre_template, n);
  const uint64_t lit[2] = { (uintptr_t)fn, tramp };
  memcpy(buf + n - 16, lit, 16);
  if (so_write_code(stub, buf, n))
    return -1;
  if (slot)
    *slot = (uintptr_t)stub;
  else
    hook_arm64(addr, (uintptr_t)stub);
  return 0;
}

// --- missing imports ----------------------------------------------------------------

// A call through an import nothing provides lands in a stub that names it,
// and the module that called it ("name\0module" in one string).
static void missing_import_called(const char *name) {
  const char *module = name + strlen(name) + 1;
  log_printf("[linker] %s called missing import %s", module, name);
  fatal_error("A function nothing provides was called:\n%s\n\nfrom %s", name, module);
}

static uintptr_t missing_stub(const char *name, const char *module, int is_object) {
  if (is_object) {
    // data: a zeroed block is the least surprising thing to read
    return (uintptr_t)calloc(1, 256);
  }
  uint32_t code[8];
  code[0] = 0x58000080u; // LDR X0, #0x10  (name)
  code[1] = 0x580000b1u; // LDR X17, #0x14 (handler)
  code[2] = 0xd61f0220u; // BR X17
  code[3] = 0xd503201fu; // NOP (align)
  const size_t nl = strlen(name), ml = strlen(module);
  char *both = malloc(nl + ml + 2);
  if (both) {
    memcpy(both, name, nl + 1);
    memcpy(both + nl + 1, module, ml + 1);
  }
  const uint64_t n = (uint64_t)(both ? both : "?\0?"), h = (uint64_t)&missing_import_called;
  memcpy(&code[4], &n, 8);
  memcpy(&code[6], &h, 8);
  void *stub = so_alloc_code(sizeof(code));
  if (!stub)
    return (uintptr_t)&missing_import_called; // still names nothing, but fails loudly
  so_write_code(stub, code, sizeof(code));
  return (uintptr_t)stub;
}

// --- relocation -------------------------------------------------------------------------

typedef struct {
  int missing;
  int unsupported;
} RelocStats;

static void apply_rela(so_module *m, Elf64_Addr r_offset, Elf64_Xword r_info,
                       Elf64_Sxword r_addend, RelocStats *st) {
  const uint32_t type = ELF64_R_TYPE(r_info);
  const uint32_t symidx = ELF64_R_SYM(r_info);
  uint64_t *where = so_ptr(m, r_offset);

  switch (type) {
    case R_AARCH64_NONE:
      return;
    case R_AARCH64_RELATIVE:
      *where = m->base + r_addend;
      return;
    case R_AARCH64_IRELATIVE: {
      uintptr_t (*resolver)(void) = (void *)(m->base + r_addend);
      *where = resolver();
      return;
    }
    case R_AARCH64_ABS64:
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
      break;
    default:
      if (st->unsupported++ < 8)
        log_printf("[linker] %s: unsupported relocation type %u at 0x%llx", m->soname, type,
                   (unsigned long long)r_offset);
      return;
  }

  uintptr_t value = 0;
  if (symidx) {
    const Elf64_Sym *s = sym_at(m, symidx);
    const char *name = str_at(m, s->st_name);
    if (s->st_shndx != SHN_UNDEF && ELF64_ST_BIND(s->st_info) == STB_LOCAL) {
      value = sym_addr(m, s);
    } else {
      value = resolve_import(m, name, NULL);
      if (!value && s->st_shndx != SHN_UNDEF)
        value = sym_addr(m, s); // defined but hidden from lookup: itself
      if (!value) {
        if (ELF64_ST_BIND(s->st_info) == STB_WEAK) {
          value = 0;
        } else {
          if (st->missing++ < 64)
            log_printf("[linker] %s: missing import %s", m->soname, name);
          value = missing_stub(name, m->soname, ELF64_ST_TYPE(s->st_info) == STT_OBJECT);
        }
      }
    }
  }
  *where = value ? value + r_addend : 0;
}

static int64_t read_sleb(const uint8_t **p, const uint8_t *end) {
  int64_t v = 0;
  int shift = 0;
  uint8_t b;
  do {
    if (*p >= end)
      return 0;
    b = *(*p)++;
    v |= (int64_t)(b & 0x7f) << shift;
    shift += 7;
  } while (b & 0x80);
  if (shift < 64 && (b & 0x40))
    v |= -((int64_t)1 << shift);
  return v;
}

// Android packed relocations ("APS2"), see bionic's linker_reloc_iterators.h
static void apply_aps2(so_module *m, RelocStats *st) {
  const uint8_t *p = so_ptr(m, m->aps2), *end = p + m->aps2sz;
  if (m->aps2sz < 4 || memcmp(p, "APS2", 4) != 0) {
    log_printf("[linker] %s: bad packed relocations", m->soname);
    return;
  }
  p += 4;
  int64_t count = read_sleb(&p, end);
  Elf64_Addr offset = (Elf64_Addr)read_sleb(&p, end);
  Elf64_Xword info = 0;
  Elf64_Sxword addend = 0;
  while (count > 0 && p < end) {
    const int64_t group_size = read_sleb(&p, end);
    const int64_t flags = read_sleb(&p, end);
    const int by_offset = flags & 1, by_info = flags & 2, by_addend = flags & 4,
              has_addend = flags & 8;
    int64_t group_delta = 0;
    if (by_offset)
      group_delta = read_sleb(&p, end);
    if (by_info)
      info = (Elf64_Xword)read_sleb(&p, end);
    if (has_addend && by_addend)
      addend += read_sleb(&p, end);
    else if (!has_addend)
      addend = 0;
    for (int64_t i = 0; i < group_size; i++) {
      offset += by_offset ? group_delta : read_sleb(&p, end);
      if (!by_info)
        info = (Elf64_Xword)read_sleb(&p, end);
      if (has_addend && !by_addend)
        addend += read_sleb(&p, end);
      apply_rela(m, offset, info, addend, st);
    }
    count -= group_size;
  }
}

static void apply_relr(so_module *m) {
  const uint64_t *relr = so_ptr(m, m->relr);
  const size_t count = m->relrsz / sizeof(uint64_t);
  Elf64_Addr where = 0;
  for (size_t i = 0; i < count; i++) {
    const uint64_t e = relr[i];
    if ((e & 1) == 0) {
      where = e;
      *(uint64_t *)so_ptr(m, where) += m->base;
      where += 8;
    } else {
      for (int bit = 1; bit < 64; bit++)
        if (e & (1ull << bit))
          *(uint64_t *)so_ptr(m, where + (bit - 1) * 8) += m->base;
      where += 63 * 8;
    }
  }
}

static void relocate(so_module *m) {
  RelocStats st = { 0 };
  if (m->relr)
    apply_relr(m);
  if (m->aps2)
    apply_aps2(m, &st);
  const Elf64_Rela *r = so_ptr(m, m->rela);
  for (size_t i = 0; m->rela && i < m->relasz / sizeof(*r); i++)
    apply_rela(m, r[i].r_offset, r[i].r_info, r[i].r_addend, &st);
  r = so_ptr(m, m->jmprel);
  for (size_t i = 0; m->jmprel && i < m->pltrelsz / sizeof(*r); i++)
    apply_rela(m, r[i].r_offset, r[i].r_info, r[i].r_addend, &st);
  if (st.missing)
    log_printf("[linker] %s: %d missing imports (stubbed)", m->soname, st.missing);
}

// --- Linux system calls in the code ------------------------------------------------------

#define SVC0 0xd4000001u     // svc #0
#define SVC_STUB_SIZE 32

static void count_svc_in_segments(const so_module *m) {
  size_t n = 0;
  for (int i = 0; i < m->phnum; i++) {
    const Elf64_Phdr *p = &m->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    for (Elf64_Addr a = ALIGN_MEM(p->p_vaddr, 4); a + 4 <= p->p_vaddr + p->p_filesz; a += 4)
      n += *(const uint32_t *)so_ptr(m, a) == SVC0;
  }
  if (n)
    log_printf("[linker] %s has no section headers: %zu svc #0 words in its code segments are "
               "left as they are", m->soname, n);
}

// The svc #0 instructions in the executable sections, as link-time vaddrs in
// m->svc_sites; their count. Sections, not segments: read-only data shares
// the executable segment and holds the same four bytes now and then (in three
// of this game's libraries), and a patched word there would corrupt data.
// Without section headers nothing is patched, only counted.
static int find_svc_sites(const uint8_t *file, size_t fsize, so_module *m) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
  if (!eh->e_shoff || !eh->e_shnum || eh->e_shentsize != sizeof(Elf64_Shdr) ||
      eh->e_shoff > fsize || (size_t)eh->e_shnum * sizeof(Elf64_Shdr) > fsize - eh->e_shoff) {
    count_svc_in_segments(m);
    return 0;
  }
  const Elf64_Shdr *sh = (const Elf64_Shdr *)(file + eh->e_shoff);
  int n = 0, cap = 0;
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_PROGBITS || (sh[i].sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) !=
                                             (SHF_ALLOC | SHF_EXECINSTR))
      continue;
    Elf64_Addr end = sh[i].sh_addr + sh[i].sh_size;
    if (end > m->text_size)
      end = m->text_size; // only what gets mapped as code
    for (Elf64_Addr a = ALIGN_MEM(sh[i].sh_addr, 4); a + 4 <= end; a += 4) {
      if (*(const uint32_t *)so_ptr(m, a) != SVC0)
        continue;
      if (n == cap) {
        cap = cap ? cap * 2 : 64;
        Elf64_Addr *grown = realloc(m->svc_sites, cap * sizeof(*grown));
        if (!grown) {
          log_printf("[linker] %s: out of memory listing its svc #0 sites", m->soname);
          return n;
        }
        m->svc_sites = grown;
      }
      m->svc_sites[n++] = a;
    }
  }
  return n;
}

// B from `from` to `to`; -1 when it is out of reach (+-128 MB)
static int branch_insn(uintptr_t from, uintptr_t to, uint32_t *insn) {
  const int64_t d = (int64_t)(to - from);
  if ((d & 3) || d < -(1ll << 27) || d >= (1ll << 27))
    return -1;
  *insn = 0x14000000u | ((uint32_t)(d >> 2) & 0x03ffffffu);
  return 0;
}

// Write the stubs (staged in svc_src) and turn each svc into a branch to its
// stub. A stub needs x16 and x30 and puts them back; linux_svc_entry keeps
// every other register:
//   stp x16, x30, [sp, #-16]! ; ldr x16, =linux_svc_entry ; blr x16
//   ldp x16, x30, [sp], #16   ; b <the svc> + 4
static int patch_svc_sites(so_module *m) {
  extern void linux_svc_entry(void);
  const uint64_t entry = (uint64_t)(uintptr_t)&linux_svc_entry;
  m->svc_src = calloc(1, m->svc_size);
  m->svc_hits = calloc(m->svc_count, sizeof(*m->svc_hits));
  if (!m->svc_src || !m->svc_hits) {
    set_error("out of memory for %s's svc stubs", m->soname);
    return -1;
  }
  for (int k = 0; k < m->svc_count; k++) {
    const uintptr_t stub = m->svc_base + (uintptr_t)k * SVC_STUB_SIZE;
    const uintptr_t site = m->base + m->svc_sites[k];
    uint32_t to_stub, back;
    if (branch_insn(site, stub, &to_stub) < 0 || branch_insn(stub + 16, site + 4, &back) < 0) {
      set_error("%s: the svc #0 at +0x%llx is out of branch range of its stub", m->soname,
                (unsigned long long)m->svc_sites[k]);
      return -1;
    }
    uint32_t *s = (uint32_t *)((uint8_t *)m->svc_src + (size_t)k * SVC_STUB_SIZE);
    s[0] = 0xa9bf7bf0u; // STP X16, X30, [SP, #-16]!
    s[1] = 0x580000b0u; // LDR X16, #20 (s[6])
    s[2] = 0xd63f0200u; // BLR X16
    s[3] = 0xa8c17bf0u; // LDP X16, X30, [SP], #16
    s[4] = back;        // B <the svc> + 4
    s[5] = 0xd503201fu; // NOP
    memcpy(&s[6], &entry, sizeof(entry));
    *(uint32_t *)so_ptr(m, m->svc_sites[k]) = to_stub;
  }
  log_printf("[linker] %s: %d Linux system calls (svc #0) routed to the port", m->soname,
             m->svc_count);
  return 0;
}

// The stubs: process code memory set read-execute, checked against what was
// written (as finalize_fixed checks a module).
static int map_svc_stubs(so_module *m) {
  if (!m->svc_count)
    return 0;
  void *blk = memalign(PAGE, m->svc_size);
  if (!blk) {
    set_error("out of memory mapping %s's svc stubs", m->soname);
    return -1;
  }
  memcpy(blk, m->svc_src, m->svc_size);
  armDCacheFlush(blk, m->svc_size);
  Result rc = svcMapProcessCodeMemory(own_process(), m->svc_base, (u64)blk, m->svc_size);
  if (R_SUCCEEDED(rc))
    rc = svcSetProcessMemoryPermission(own_process(), m->svc_base, m->svc_size, Perm_Rx);
  if (R_FAILED(rc)) {
    set_error("%s: mapping its svc stubs failed: 0x%x", m->soname, rc);
    return -1;
  }
  armICacheInvalidate((void *)m->svc_base, m->svc_size);
  if (memcmp((const void *)m->svc_base, m->svc_src, m->svc_size) != 0) {
    set_error("%s: its svc stubs differ from what was written once mapped", m->soname);
    return -1;
  }
  free(m->svc_src);
  m->svc_src = NULL;
  return 0;
}

// --- loading ---------------------------------------------------------------------------

so_module *so_find_loaded(const char *name) {
  const char *b = base_name(name);
  for (so_module *m = so_list; m; m = m->next) {
    if (name[0] == '/' && !strcmp(m->path, name))
      return m;
    if (!strcmp(m->soname, b) || !strcmp(base_name(m->path), b))
      return m;
  }
  return NULL;
}

so_module *so_module_at(uintptr_t addr) {
  for (so_module *m = so_list; m; m = m->next)
    if (addr >= m->base && addr < m->base + m->size)
      return m;
  return NULL;
}

so_module *so_svc_site(uintptr_t stub, uintptr_t *site, int *index) {
  for (so_module *m = so_list; m; m = m->next) {
    if (!m->svc_count || stub < m->svc_base ||
        stub >= m->svc_base + (uintptr_t)m->svc_count * SVC_STUB_SIZE ||
        (stub - m->svc_base) % SVC_STUB_SIZE)
      continue;
    *index = (int)((stub - m->svc_base) / SVC_STUB_SIZE);
    *site = m->base + m->svc_sites[*index];
    return m;
  }
  return NULL;
}

static int file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  fclose(f);
  return 1;
}

static int find_file(const char *name, char *out, size_t outlen) {
  if (strchr(name, '/')) {
    snprintf(out, outlen, "%s", name);
    return file_exists(out);
  }
  for (int i = 0; i < num_search_dirs; i++) {
    snprintf(out, outlen, "%s/%s", search_dirs[i], name);
    if (file_exists(out))
      return 1;
  }
  return 0;
}

static void *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  *size = ftell(f);
  fseek(f, 0, SEEK_SET);
  void *buf = malloc(*size ? *size : 1);
  if (buf && fread(buf, 1, *size, f) != *size) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  return buf;
}

static void free_image(so_module *m);

// Read and lay out the image, parse its dynamic section. Not relocated.
static so_module *load_image(const char *path) {
  size_t fsize = 0;
  uint8_t *file = read_file(path, &fsize);
  if (!file) {
    set_error("cannot read %s", path);
    return NULL;
  }

  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
  if (fsize < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
      eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_AARCH64) {
    set_error("%s is not an arm64 ELF library", path);
    free(file);
    return NULL;
  }
  if (eh->e_phnum > SO_MAX_PHDRS) {
    set_error("%s: too many program headers (%d)", path, eh->e_phnum);
    free(file);
    return NULL;
  }

  so_module *m = calloc(1, sizeof(*m));
  snprintf(m->path, sizeof(m->path), "%s", path);
  m->phnum = eh->e_phnum;
  memcpy(m->phdr, file + eh->e_phoff, m->phnum * sizeof(Elf64_Phdr));

  // extent of the image, and where the writable part begins
  size_t end = 0, first_w = (size_t)-1, last_ro_end = 0;
  const Elf64_Phdr *dyn_ph = NULL;
  for (int i = 0; i < m->phnum; i++) {
    const Elf64_Phdr *p = &m->phdr[i];
    if (p->p_type == PT_DYNAMIC)
      dyn_ph = p;
    if (p->p_type == PT_TLS)
      log_printf("[linker] %s has ELF TLS (PT_TLS), which is not supported", path);
    if (p->p_type != PT_LOAD)
      continue;
    const size_t seg_end = p->p_vaddr + p->p_memsz;
    if (seg_end > end)
      end = seg_end;
    if ((p->p_flags & PF_W) && p->p_vaddr < first_w)
      first_w = p->p_vaddr;
  }
  // the code pages: the read-only and executable segments before the first
  // writable one; any after it are late ones (their own pages, permissions)
  m->nlate = 0;
  for (int i = 0; i < m->phnum; i++) {
    const Elf64_Phdr *p = &m->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_W))
      continue;
    const size_t seg_end = p->p_vaddr + p->p_memsz;
    if (p->p_vaddr < first_w) {
      if (seg_end > last_ro_end)
        last_ro_end = seg_end;
    } else if (m->nlate < (int)(sizeof m->late / sizeof *m->late)) {
      m->late[m->nlate].off = p->p_vaddr & ~(size_t)(PAGE - 1);
      m->late[m->nlate].len = ALIGN_MEM(seg_end, PAGE) - m->late[m->nlate].off;
      m->late[m->nlate].exec = (p->p_flags & PF_X) != 0;
      m->nlate++;
    } else {
      set_error("%s: too many segments after its data", path);
      free(file);
      free(m);
      return NULL;
    }
  }
  m->size = ALIGN_MEM(end, PAGE);
  m->text_size = ALIGN_MEM(last_ro_end, PAGE);
  if (first_w != (size_t)-1 && (first_w & ~(size_t)(PAGE - 1)) < m->text_size) {
    set_error("%s: writable segment inside the code pages", path);
    free(file);
    free(m);
    return NULL;
  }
  if (!dyn_ph) {
    set_error("%s has no dynamic section", path);
    free(file);
    free(m);
    return NULL;
  }

  m->src = memalign(PAGE, m->size);
  if (!m->src) {
    set_error("out of memory loading %s (%zu KB)", path, m->size / 1024);
    free(file);
    free(m);
    return NULL;
  }
  memset(m->src, 0, m->size);
  for (int i = 0; i < m->phnum; i++) {
    const Elf64_Phdr *p = &m->phdr[i];
    if (p->p_type == PT_LOAD && p->p_filesz)
      memcpy((uint8_t *)m->src + p->p_vaddr, file + p->p_offset, p->p_filesz);
  }
  // Linux system calls in the code, found while the section headers are at hand
  m->svc_count = find_svc_sites(file, fsize, m);
  m->svc_size = ALIGN_MEM((size_t)m->svc_count * SVC_STUB_SIZE, PAGE);
  free(file);

  // their stubs go in the pages just below the image
  virtmemLock();
  m->svc_base = (uintptr_t)virtmemFindCodeMemory(m->svc_size + m->size, PAGE);
  m->base_rsv = m->svc_base ? virtmemAddReservation((void *)m->svc_base, m->svc_size + m->size)
                            : NULL;
  virtmemUnlock();
  m->base = m->svc_base ? m->svc_base + m->svc_size : 0;
  if (!m->base) {
    set_error("no address space for %s", path);
    free_image(m);
    return NULL;
  }

  // dynamic section
  const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uint8_t *)m->src + dyn_ph->p_vaddr);
  Elf64_Xword soname = 0;
  int needed_count = 0;
  for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    const Elf64_Xword v = d->d_un.d_val;
    switch (d->d_tag) {
      case DT_NEEDED: needed_count++; break;
      case DT_SONAME: soname = v; break;
      case DT_SYMTAB: m->symtab = v; break;
      case DT_STRTAB: m->strtab = v; break;
      case DT_HASH: m->sysv_hash = v; break;
      case DT_GNU_HASH: m->gnu_hash = v; break;
      case DT_RELA: m->rela = v; break;
      case DT_RELASZ: m->relasz = v; break;
      case DT_JMPREL: m->jmprel = v; break;
      case DT_PLTRELSZ: m->pltrelsz = v; break;
      case DT_RELR: case DT_ANDROID_RELR: m->relr = v; break;
      case DT_RELRSZ: case DT_ANDROID_RELRSZ: m->relrsz = v; break;
      case DT_ANDROID_RELA: m->aps2 = v; break;
      case DT_ANDROID_RELASZ: m->aps2sz = v; break;
      case DT_INIT: m->init_fn = v; break;
      case DT_INIT_ARRAY: m->init_array = v; break;
      case DT_INIT_ARRAYSZ: m->init_count = v / 8; break;
      case DT_FINI_ARRAY: m->fini_array = v; break;
      case DT_FINI_ARRAYSZ: m->fini_count = v / 8; break;
      case DT_REL: case DT_ANDROID_REL:
        log_printf("[linker] %s: REL relocations are not supported", path);
        break;
      default: break;
    }
  }

  // number of symbols (for dladdr / tables without hashes)
  if (m->sysv_hash) {
    m->nsyms = ((const uint32_t *)so_ptr(m, m->sysv_hash))[1];
  } else if (m->gnu_hash) {
    const uint32_t *ht = so_ptr(m, m->gnu_hash);
    const uint32_t nbuckets = ht[0], symoffset = ht[1], bloom_size = ht[2];
    const uint32_t *buckets = (const uint32_t *)((const uint64_t *)(ht + 4) + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t maxidx = 0;
    for (uint32_t b = 0; b < nbuckets; b++)
      if (buckets[b] > maxidx)
        maxidx = buckets[b];
    if (maxidx >= symoffset)
      while (!(chain[maxidx - symoffset] & 1))
        maxidx++;
    m->nsyms = maxidx + 1;
  }

  snprintf(m->soname, sizeof(m->soname), "%s",
           soname ? str_at(m, soname) : base_name(path));

  // remember the needed names; resolved by the caller
  m->num_needed = needed_count;
  m->needed = calloc(needed_count ? needed_count : 1, sizeof(so_module *));

  // now that the base and the name are known
  if (m->svc_count && patch_svc_sites(m) < 0) {
    free_image(m);
    return NULL;
  }
  return m;
}

static void free_image(so_module *m) {
  virtmemLock();
  if (m->base_rsv)
    virtmemRemoveReservation(m->base_rsv);
  virtmemUnlock();
  free(m->src);
  free(m->needed);
  free(m->svc_sites);
  free(m->svc_src);
  free(m->svc_hits);
  free(m);
}

static const char *needed_name(so_module *m, int index) {
  const Elf64_Dyn *dyn = NULL;
  for (int i = 0; i < m->phnum; i++)
    if (m->phdr[i].p_type == PT_DYNAMIC)
      dyn = so_ptr(m, m->phdr[i].p_vaddr);
  int k = 0;
  for (; dyn && dyn->d_tag != DT_NULL; dyn++)
    if (dyn->d_tag == DT_NEEDED && k++ == index)
      return str_at(m, dyn->d_un.d_val);
  return "";
}

// Map the image: code memory (RX + RW alias) for the read-only pages, process
// code memory set RW for the data pages. The relocated image in m->src is a
// staging copy: the code pages are written through the alias after mapping
// (an emulator's code memory need not show what its source pages held), the
// data pages get a block of their own, and the staging copy is freed.
// Whether plain process code memory keeps what was written through its
// permission change, decided once with a probe block: on hardware it does
// (gdash_nx maps the game this way). Ryujinx running code natively (NCE)
// rewrites instructions in pages made executable (exclusive loads/stores
// become their acquire/release forms) -- and any constant data sharing the
// segment that looks like them (an Eclipse format string came out changed:
// fmt::format_error at load) -- and a mapping that went wrong there cannot be
// undone (it refuses to unmap it). The probe stays mapped: 256 KB.
static int plain_mapping_holds(void) {
  static int known = -1;
  if (known >= 0)
    return known;
  const size_t n = 64 * PAGE;
  // heap that held something else first, like a module's block
  void *junk = malloc(n * 2);
  if (junk) {
    memset(junk, 0x5a, n * 2);
    free(junk);
  }
  uint8_t *blk = memalign(PAGE, n);
  uint8_t *want = malloc(n);
  virtmemLock();
  void *addr = virtmemFindCodeMemory(n, PAGE);
  void *rsv = addr ? virtmemAddReservation(addr, n) : NULL;
  virtmemUnlock();
  known = 0;
  if (blk && want && rsv) {
    // the code half: what Android code is full of and an emulator running
    // code natively rewrites (Ryujinx's NCE patches TPIDR_EL0 reads and
    // svc) -- strings in the same segment then come out changed too
    for (size_t i = 0; i < n / 2; i += 32) {
      const uint32_t w[8] = { 0x885f7c20u /* ldxr w0, [x1] */, 0x88027c20u /* stxr w2, w0, [x1] */,
                              0xc85f7c20u /* ldxr x0, [x1] */, 0xc8027c20u /* stxr w2, x0, [x1] */,
                              0xd53bd040u /* mrs x0, tpidr_el0 */, 0xd4000021u /* svc #1 */,
                              0xd53be040u /* mrs x0, cntpct_el0 */, 0xd65f03c0u /* ret */ };
      memcpy(blk + i, w, sizeof w);
      memcpy(want + i, w, sizeof w);
    }
    for (size_t i = n / 2; i < n; i++)
      blk[i] = want[i] = (uint8_t)(i * 131 + (i >> 12) * 7 + 1);
    armDCacheFlush(blk, n);
    Result rc = svcMapProcessCodeMemory(own_process(), (u64)addr, (u64)blk, n);
    if (R_SUCCEEDED(rc))
      rc = svcSetProcessMemoryPermission(own_process(), (u64)addr, n / 2, Perm_Rx);
    if (R_SUCCEEDED(rc))
      rc = svcSetProcessMemoryPermission(own_process(), (u64)addr + n / 2, n / 2, Perm_Rw);
    size_t bad = 0;
    for (size_t off = 0; R_SUCCEEDED(rc) && off < n; off += PAGE)
      bad += memcmp((const uint8_t *)addr + off, want + off, PAGE) != 0;
    known = R_SUCCEEDED(rc) && bad == 0;
    log_printf("[linker] plain code mapping %s (probe: rc 0x%x, %zu of %zu pages changed)",
               known ? "holds: modules nothing hooks use no CodeMemory"
                     : "does not hold here (an emulator?): CodeMemory for every module",
               rc, bad, n / PAGE);
  }
  free(want);
  return known;
}

// A module nothing hooks: one block of process code memory, the code pages
// set read-execute and the data pages read-write (gdash_nx's way). Checked
// page by page against what was written: a module must never run from pages
// that differ.
// The data part, just mapped as process code memory: read-write, but the
// executable segments after the data (the late ones) read-execute, set
// first (code memory once made writable cannot become executable again).
// A late segment sharing a page with a writable one stays read-write.
static Result data_permissions(so_module *m, const char **step) {
  size_t at = m->text_size;
  *step = "read-write";
  for (;;) {
    // the next executable late segment from at, in address order
    int next = -1;
    for (int i = 0; i < m->nlate; i++)
      if (m->late[i].exec && m->late[i].off >= at && (next < 0 || m->late[i].off < m->late[next].off))
        next = i;
    size_t off = next >= 0 ? m->late[next].off : m->size, len = next >= 0 ? m->late[next].len : 0;
    if (next >= 0)
      for (int j = 0; j < m->phnum; j++) {
        const Elf64_Phdr *p = &m->phdr[j];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_W) && p->p_vaddr < off + len &&
            ALIGN_MEM(p->p_vaddr + p->p_memsz, PAGE) > off) {
          log_printf("[linker] %s: segment at 0x%zx shares pages with data: left read-write", m->soname, off);
          len = 0;
        }
      }
    if (len) {
      // (no cache work through m->base before this: the console maps fresh
      // code memory inaccessible until its permission is set, so the
      // caller cleans the source pages before mapping them)
      const Result rc = svcSetProcessMemoryPermission(own_process(), m->base + off, len, Perm_Rx);
      if (R_FAILED(rc)) {
        *step = "segments after the data";
        return rc;
      }
      armICacheInvalidate((void *)(m->base + off), len);
      log_printf("[linker] %s: executable segment after the data at 0x%zx (%zu KB)", m->soname, off, len / 1024);
    }
    const size_t gap_end = len ? off : (next >= 0 ? off + m->late[next].len : m->size);
    if (gap_end > at) {
      const Result rc = svcSetProcessMemoryPermission(own_process(), m->base + at, gap_end - at, Perm_Rw);
      if (R_FAILED(rc))
        return rc;
    }
    if (next < 0)
      return 0;
    at = off + m->late[next].len;
  }
}

static int finalize_fixed(so_module *m) {
  void *src = memalign(PAGE, m->size);
  if (!src) {
    set_error("out of memory mapping %s", m->soname);
    return -1;
  }
  memcpy(src, m->src, m->size);
  armDCacheFlush(src, m->size);
  Result rc = svcMapProcessCodeMemory(own_process(), m->base, (u64)src, m->size);
  if (R_SUCCEEDED(rc) && m->text_size)
    rc = svcSetProcessMemoryPermission(own_process(), m->base, m->text_size, Perm_Rx);
  const char *step = "";
  if (R_SUCCEEDED(rc) && m->size > m->text_size)
    rc = data_permissions(m, &step);
  if (R_FAILED(rc)) {
    set_error("%s: mapping %zu KB failed: 0x%x", m->soname, m->size / 1024, rc);
    return -1;
  }
  armICacheInvalidate((void *)m->base, m->text_size);
  size_t bad = 0;
  for (size_t off = 0; off < m->size; off += PAGE)
    bad += memcmp((const uint8_t *)m->base + off, (const uint8_t *)m->src + off, PAGE) != 0;
  if (bad) {
    set_error("%s: %zu of %zu pages differ from the file once mapped", m->soname, bad,
              m->size / PAGE);
    return -1;
  }
  free(m->src);
  m->src = NULL;
  m->mapped = 1;
  return 0;
}

// A module that writes its own read-only pages (so_set_writable_readonly):
// one block of process code memory, its executable segments read-execute
// (set first: code memory once writable cannot become executable again)
// and every other page read-write. The other pages are checked against the
// file (an emulator running code natively may rewrite executable ones).
static int finalize_writable_ro(so_module *m) {
  void *src = memalign(PAGE, m->size);
  if (!src) {
    set_error("out of memory mapping %s", m->soname);
    return -1;
  }
  memcpy(src, m->src, m->size);
  armDCacheFlush(src, m->size);
  Result rc = svcMapProcessCodeMemory(own_process(), m->base, (u64)src, m->size);
  for (int i = 0; i < m->phnum && R_SUCCEEDED(rc); i++) {
    const Elf64_Phdr *p = &m->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    const size_t off = p->p_vaddr & ~(size_t)(PAGE - 1), end = ALIGN_MEM(p->p_vaddr + p->p_memsz, PAGE);
    rc = svcSetProcessMemoryPermission(own_process(), m->base + off, end - off, Perm_Rx);
    if (R_SUCCEEDED(rc))
      armICacheInvalidate((void *)(m->base + off), end - off);
  }
  // the rest, in runs between the executable pages
  size_t at = 0, bad = 0;
  while (R_SUCCEEDED(rc) && at < m->size) {
    int exec = 0;
    size_t run_end = m->size;
    for (int i = 0; i < m->phnum; i++) {
      const Elf64_Phdr *p = &m->phdr[i];
      if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
        continue;
      const size_t off = p->p_vaddr & ~(size_t)(PAGE - 1), end = ALIGN_MEM(p->p_vaddr + p->p_memsz, PAGE);
      if (at >= off && at < end)
        exec = 1, run_end = end;
      else if (off > at && off < run_end)
        run_end = off;
    }
    if (!exec) {
      rc = svcSetProcessMemoryPermission(own_process(), m->base + at, run_end - at, Perm_Rw);
      for (size_t o = at; R_SUCCEEDED(rc) && o < run_end; o += PAGE)
        bad += memcmp((const uint8_t *)m->base + o, (const uint8_t *)m->src + o, PAGE) != 0;
    }
    at = run_end;
  }
  if (R_FAILED(rc) || bad) {
    set_error("%s: mapping %zu KB failed: 0x%x (%zu pages differ)", m->soname, m->size / 1024, rc, bad);
    return -1;
  }
  log_printf("[linker] %s: its read-only pages writable (it writes them itself)", m->soname);
  free(m->src);
  m->src = NULL;
  m->mapped = 1;
  return 0;
}

static int finalize(so_module *m) {
  void *dummy = NULL;
  Handle h = INVALID_HANDLE;
  if (map_svc_stubs(m) < 0)
    return -1;
  if (is_writable_ro(m))
    return finalize_writable_ro(m);
  if (!is_patchable(m) && plain_mapping_holds())
    return finalize_fixed(m);
  if (m->text_size) {
    void *code_src = memalign(PAGE, m->text_size);
    if (!code_src) {
      set_error("out of memory mapping %s", m->soname);
      return -1;
    }
    if (map_codemem(code_src, m->text_size, m->base, &m->base, &m->rw_alias, &h, &dummy,
                    &m->alias_rsv) < 0)
      return -1;
    m->codemem = h;
    memcpy((void *)m->rw_alias, m->src, m->text_size);
    armDCacheFlush((void *)m->rw_alias, m->text_size);
    armICacheInvalidate((void *)m->base, m->text_size);
    if (memcmp((const void *)m->base, m->src, 64) != 0)
      log_printf("[linker] %s: executable view differs from what was written", m->soname);
  }
  if (m->size > m->text_size) {
    const u64 len = m->size - m->text_size;
    void *data_src = memalign(PAGE, len);
    if (!data_src) {
      set_error("out of memory mapping %s", m->soname);
      return -1;
    }
    memcpy(data_src, (uint8_t *)m->src + m->text_size, len);
    armDCacheFlush(data_src, len); // (its executable segments after the data: data_permissions)
    const u64 dst = m->base + m->text_size;
    Result rc = svcMapProcessCodeMemory(own_process(), dst, (u64)data_src, len);
    const char *step = "map";
    if (R_SUCCEEDED(rc))
      rc = data_permissions(m, &step);
    if (R_FAILED(rc)) {
      set_error("%s: mapping %llu KB of data failed (%s): 0x%x", m->soname,
                (unsigned long long)(len / 1024), step, rc);
      return -1;
    }
    if (memcmp((const void *)dst, (uint8_t *)m->src + m->text_size, 64) != 0)
      log_printf("[linker] %s: data view differs from what was written", m->soname);
  }
  free(m->src);
  m->src = NULL;
  m->mapped = 1;
  return 0;
}

static so_module *load_recursive(const char *name, int depth);

static so_module *load_deps_and_map(so_module *m, int depth) {
  for (int i = 0; i < m->num_needed; i++) {
    const char *dep = needed_name(m, i);
    if (!strcmp(base_name(dep), "libstdc++.so")) {
      m->needed[i] = &stdcxx_mod;
      continue;
    }
    if (is_system_lib(dep)) {
      m->needed[i] = NULL;
      continue;
    }
    so_module *d = load_recursive(dep, depth + 1);
    if (!d) {
      // keep the error from the dependency, and say who needed it
      char inner[sizeof(so_error)];
      snprintf(inner, sizeof(inner), "%s", so_error);
      set_error("%s needs %s: %s", m->soname, dep, inner);
      return NULL;
    }
    m->needed[i] = d;
  }
  relocate(m);
  if (finalize(m) < 0)
    return NULL;
  log_printf("[linker] loaded %s at %p (%zu KB, code %zu KB)", m->soname, (void *)m->base,
             m->size / 1024, m->text_size / 1024);
  if (load_hook)
    load_hook(m);
  return m;
}

static so_module *load_recursive(const char *name, int depth) {
  so_module *m = so_find_loaded(name);
  if (m) {
    m->refcount++;
    return m;
  }
  if (depth > 32) {
    set_error("dependency chain too deep at %s", name);
    return NULL;
  }
  char path[512];
  if (!find_file(name, path, sizeof(path))) {
    set_error("%s not found", name);
    return NULL;
  }
  m = load_image(path);
  if (!m)
    return NULL;
  m->refcount = 1;

  // list it before its dependencies are loaded so cycles terminate
  so_module **tail = &so_list;
  while (*tail)
    tail = &(*tail)->next;
  *tail = m;

  if (!load_deps_and_map(m, depth)) {
    // unlink; a failed image is never mapped, so it can be freed
    for (so_module **pp = &so_list; *pp; pp = &(*pp)->next)
      if (*pp == m) { *pp = m->next; break; }
    if (!m->mapped)
      free_image(m);
    return NULL;
  }
  return m;
}

so_module *so_load(const char *name) {
  rmutexLock(&so_lock);
  so_module *m = load_recursive(name, 0);
  rmutexUnlock(&so_lock);
  return m;
}

void so_run_inits(so_module *m) {
  if (!m || m == &system_mod || m == &stdcxx_mod || m->initialized)
    return;
  m->initialized = 1; // set first: dependency cycles
  for (int i = 0; i < m->num_needed; i++)
    so_run_inits(m->needed[i]);
  log_printf("[linker] init %s", m->soname);
  if (m->init_fn)
    ((void (*)(void))(m->base + m->init_fn))();
  if (m->init_array) {
    void (**arr)(void) = so_ptr(m, m->init_array);
    for (size_t i = 0; i < m->init_count; i++)
      if (arr[i] && (uintptr_t)arr[i] != (uintptr_t)-1)
        arr[i]();
  }
}

void so_hide_exports(so_module *mod, const char *const *names, int n) {
  mod->hidden = names;
  mod->num_hidden = n;
}

// --- libdl ---------------------------------------------------------------------------------

static void dl_set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dl_set_error(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  vsnprintf(so_dlerror_buf, sizeof(so_dlerror_buf), fmt, va);
  va_end(va);
  so_dlerror_set = 1;
}

void *so_dlopen(const char *name, int flags) {
  if (!name)
    return &system_mod; // "the program": searched like RTLD_DEFAULT
  if (is_system_lib(name))
    return &system_mod;
  rmutexLock(&so_lock);
  so_module *m = so_find_loaded(name);
  if (m) {
    m->refcount++;
    rmutexUnlock(&so_lock);
    return m;
  }
  if (flags & BIONIC_RTLD_NOLOAD) {
    rmutexUnlock(&so_lock);
    dl_set_error("library \"%s\" wasn't loaded and RTLD_NOLOAD prevented it", name);
    return NULL;
  }
  log_printf("[linker] dlopen %s", name);
  m = load_recursive(name, 0);
  if (!m) {
    rmutexUnlock(&so_lock);
    dl_set_error("dlopen failed: %s", so_error);
    return NULL;
  }
  so_run_inits(m);
  rmutexUnlock(&so_lock);
  return m;
}

// RTLD_DEFAULT: overrides, the system libraries, then every module in load
// order (bionic walks the namespace in load order, system libraries first)
static uintptr_t lookup_default(const char *name, so_module *after) {
  uintptr_t a = 0;
  if (!after) {
    a = table_find(&ovr_table, name);
    if (!a)
      a = table_find(&sys_table, name);
    if (a)
      return a;
  }
  const uint32_t gh = gnu_hash_of(name), sh = sysv_hash_of(name);
  so_module *m = after ? after->next : so_list;
  for (; m; m = m->next) {
    const Elf64_Sym *s = module_lookup(m, name, gh, sh, 1);
    if (s)
      return sym_addr(m, s);
  }
  return fallback_resolver ? fallback_resolver(name) : 0;
}

void *so_dlsym(void *handle, const char *name) {
  if (!name) {
    dl_set_error("dlsym: NULL symbol name");
    return NULL;
  }
  uintptr_t a = 0;
  rmutexLock(&so_lock);
  if (handle == HANDLE_DEFAULT || handle == &system_mod) {
    a = lookup_default(name, NULL);
  } else if (handle == HANDLE_NEXT) {
    so_module *caller = so_module_at((uintptr_t)__builtin_return_address(0));
    a = lookup_default(name, caller ? caller : NULL);
  } else {
    // the handle's own exports, then its dependencies (bionic: local group)
    so_module *m = handle;
    const Elf64_Sym *s = module_lookup(m, name, gnu_hash_of(name), sysv_hash_of(name), 0);
    a = s ? sym_addr(m, s) : resolve_import(m, name, NULL);
  }
  rmutexUnlock(&so_lock);
  if (!a)
    dl_set_error("undefined symbol: %s", name);
  return (void *)a;
}

int so_dlclose(void *handle) {
  // modules stay mapped: nothing Geode loads is ever unloaded on Android either
  (void)handle;
  return 0;
}

char *so_dlerror(void) {
  if (!so_dlerror_set)
    return NULL;
  so_dlerror_set = 0;
  return so_dlerror_buf;
}

// the module containing addr and its base, without the linker's lock (for
// the watchdog and crash reports, which must not wait on a stuck thread)
const char *so_name_at_unlocked(uintptr_t addr, uintptr_t *base) {
  for (so_module *m = so_list; m; m = m->next)
    if (addr >= m->base && addr < m->base + m->size) {
      if (base)
        *base = m->base;
      return m->soname;
    }
  return NULL;
}

int so_dladdr(const void *addr, so_dl_info *info) {
  const uintptr_t a = (uintptr_t)addr;
  rmutexLock(&so_lock);
  so_module *m = so_module_at(a);
  if (!m) {
    rmutexUnlock(&so_lock);
    return 0;
  }
  info->dli_fname = m->path;
  info->dli_fbase = (void *)m->base;
  info->dli_sname = NULL;
  info->dli_saddr = NULL;
  // the symbol containing addr, else the closest one below it
  uintptr_t best = 0;
  for (size_t i = 1; i < m->nsyms; i++) {
    const Elf64_Sym *s = sym_at(m, i);
    if (s->st_shndx == SHN_UNDEF || !s->st_value)
      continue;
    const int t = ELF64_ST_TYPE(s->st_info);
    if (t != STT_FUNC && t != STT_OBJECT)
      continue;
    const uintptr_t sa = m->base + s->st_value;
    if (sa > a)
      continue;
    if (a < sa + s->st_size || sa > best) {
      best = sa;
      info->dli_sname = str_at(m, s->st_name);
      info->dli_saddr = (void *)sa;
      if (a < sa + s->st_size)
        break;
    }
  }
  rmutexUnlock(&so_lock);
  return 1;
}

// matches the layout bionic/libunwind expect
struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  // no lock held across the callback: unwinders call this from anywhere
  for (so_module *m = so_list; m; m = m->next) {
    if (!m->mapped)
      continue;
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)m->base;
    info.dlpi_name = m->path;
    info.dlpi_phdr = m->phdr; // link-time vaddrs + dlpi_addr = runtime
    info.dlpi_phnum = m->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}
