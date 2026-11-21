/* so_util.h -- a small dynamic linker for Android arm64 .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 * Rewritten for the Geode port: dependency loading, hash lookups, runtime
 * dlopen/dlsym/dladdr and dual-mapped code (see so_util.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#define SO_MAX_PHDRS 16

typedef struct {
  const char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct so_module {
  struct so_module *next;
  char path[512];  // file it was loaded from
  char soname[128]; // DT_SONAME, else the file name
  int refcount;
  int initialized; // constructors have run
  int mapped;      // so_finalize done: read through base, not src
  int jni_loaded;  // its JNI_OnLoad has run (unity.c, System.load)

  // runtime image: [base, base + size). The first text_size bytes (every
  // read-only and executable page) are code memory with a writable alias at
  // rw_alias; the rest is the data segments, mapped read-write.
  uintptr_t base;
  size_t size;
  void *src;       // heap backing store of the image
  size_t text_size;
  // read-only or executable segments after the data (a library patched
  // after linking, as Mario Kart Tour KV's libil2cpp.so: its hooks'
  // trampolines and new headers appended at the end): offsets and lengths
  // in pages, given their own permission once the data is mapped
  struct { size_t off, len; int exec; } late[4];
  int nlate;
  uintptr_t rw_alias;
  uint32_t codemem; // Handle
  void *base_rsv, *alias_rsv;

  // pristine program headers (link-time vaddrs) for dl_iterate_phdr
  Elf64_Phdr phdr[SO_MAX_PHDRS];
  int phnum;

  // dynamic section, as link-time vaddrs (see so_ptr)
  Elf64_Addr symtab, strtab, gnu_hash, sysv_hash;
  size_t nsyms;
  Elf64_Addr rela, jmprel, relr, aps2;
  size_t relasz, pltrelsz, relrsz, aps2sz;
  Elf64_Addr init_array, fini_array, init_fn;
  size_t init_count, fini_count;

  int num_needed;
  struct so_module **needed; // NULL entries: system libraries (import table)

  // exports other modules must not bind to (GD's static C++ runtime)
  const char *const *hidden;
  int num_hidden;

  // Linux system calls the code makes itself (svc #0): each one is a branch
  // to a stub of its own in [svc_base, base), the pages just below the image
  // (see so_util.c and linux_svc.c)
  int svc_count;
  Elf64_Addr *svc_sites; // link-time vaddrs of the svc instructions
  uintptr_t svc_base;
  size_t svc_size;
  void *svc_src;         // the stubs, staged until the module is mapped
  uint32_t *svc_hits;    // per site: calls so far (linux_svc.c logs some)
} so_module;

// bionic's Dl_info
typedef struct {
  const char *dli_fname;
  void *dli_fbase;
  const char *dli_sname;
  void *dli_saddr;
} so_dl_info;

// --- setup -------------------------------------------------------------------

// libc/libm/libGLESv2/... symbols (the "system libraries" every module may
// import) and overrides consulted before any module's own exports.
void so_set_system_symbols(DynLibFunction *funcs, size_t n);
void so_set_overrides(DynLibFunction *funcs, size_t n);
// imports of the module `name` (soname or file name) that resolve to these
// first: one module's own versions of system functions
void so_set_module_overrides(const char *name, DynLibFunction *funcs, size_t n);
// bionic's libstdc++.so (new/delete, __cxa_guard_*): only for modules that
// list it in DT_NEEDED (the game, libfmod)
void so_set_stdcxx_symbols(DynLibFunction *funcs, size_t n);
// last resort for imports nothing exports (GL extension entry points)
void so_set_fallback(uintptr_t (*resolver)(const char *name));
// directories searched for a DT_NEEDED / dlopen name without a '/'
void so_add_search_dir(const char *dir);
// a module whose code gets hooked (by soname or file name): its code is
// CodeMemory with a writable alias. Others are mapped read-execute for good
// (the kernel has only 10 CodeMemory objects for the whole system).
void so_set_patchable(const char *name);
// a module that writes its own read-only pages (mprotect, then writes: an
// obfuscated library decrypting its strings in place, MKT KV's libmktkv.so):
// its executable segments read-execute, everything else read-write. Its
// code cannot be hooked.
void so_set_writable_readonly(const char *name);
// called for each library as it is loaded (so_load, dlopen, and every
// dependency they pull in), mapped but before its constructors run
// (game_patch.c patches its code there)
void so_set_load_hook(void (*hook)(so_module *m));
// this process's real handle (for the process-memory calls)
unsigned int so_own_process(void);
// verify the kernel calls the loader needs; NULL when fine, else a message
const char *so_check_kernel(void);

// --- loading -------------------------------------------------------------------

// Load a library and everything it needs, relocated and mapped, constructors
// NOT run yet (patch first, then so_run_inits). Returns NULL and sets
// so_last_error() on failure.
so_module *so_load(const char *name);
void so_run_inits(so_module *mod);
const char *so_last_error(void);

// Hide exports of mod from every other module's symbol resolution.
void so_hide_exports(so_module *mod, const char *const *names, int n);

// --- lookups -------------------------------------------------------------------

// runtime address of an exported symbol of mod, or 0
uintptr_t so_sym(so_module *mod, const char *name);
// same, aborting with an error screen when it is missing
uintptr_t so_sym_req(so_module *mod, const char *name);
so_module *so_find_loaded(const char *name);
so_module *so_module_at(uintptr_t addr);
// the module whose svc stub starts at stub: the svc's runtime address and its
// index in svc_sites; NULL when stub is not one
so_module *so_svc_site(uintptr_t stub, uintptr_t *site, int *index);

// --- code memory -------------------------------------------------------------------

// Write to loaded code (or anything else) through the writable alias and
// flush the caches. Returns 0 on success.
int so_write_code(void *dst, const void *src, size_t size);
// Executable memory for generated code (hook trampolines); write it with
// so_write_code. Never freed.
void *so_alloc_code(size_t size);
// Redirect the function at addr to dst (16-byte absolute branch).
void hook_arm64(uintptr_t addr, uintptr_t dst);
// Send every call of the function at addr to dst instead; returns the
// original (its first instructions moved to a trampoline) for dst to call,
// or 0 when addr cannot be hooked. A C dst must not let a C++ exception
// (an IL2CPP managed one) pass through it: only for functions that never
// throw.
uintptr_t hook_arm64_replace(uintptr_t addr, uintptr_t dst);
// MKT KV's functions: the game's own code under KV's (0: not one of them)
uintptr_t hook_kv_original(uintptr_t addr);
// what MKT KV put in addr's table slot now (its handler; 0: not one of KV's)
uintptr_t kv_handler(uintptr_t addr);
// Run fn on every entry to the function at addr, then the function as if
// called directly (its first instructions moved to a trampoline; see
// hook_pre.s). fn gets the stub's frame: [0] the caller's x29, [1] the
// return address, [2..11] x0-x9 (writable). -1 when addr cannot be hooked.
int hook_arm64_pre(uintptr_t addr, void (*fn)(uint64_t *frame));

// --- bionic libdl ----------------------------------------------------------------

// the module containing addr (its soname, its base), NULL when none; takes no
// lock, so it never waits on a stuck thread (watchdog.c)
const char *so_name_at_unlocked(uintptr_t addr, uintptr_t *base);
void *so_dlopen(const char *name, int flags);
void *so_dlsym(void *handle, const char *name);
int so_dlclose(void *handle);
char *so_dlerror(void);
int so_dladdr(const void *addr, so_dl_info *info);
int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);

#endif
