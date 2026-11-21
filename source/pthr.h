/* pthr.h -- bionic<->newlib pthread wrappers for the game's libraries
 *
 * Ported from gm666q/lswtcs-vita (reimpl/pthr.h) via lbbg_nx, adapted for
 * devkitA64/libnx. The Android pthread structs differ in layout from newlib's;
 * these wrappers reinterpret the game's storage so statically initialized
 * mutexes/conds get a real newlib object on first use.
 *
 * Unlike the Fusion ports there is no GL-ownership machinery here: the game
 * renders on exactly one thread (ours), all other game/FMOD threads never
 * touch GL.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __PTHR_H__
#define __PTHR_H__

#include <pthread.h>
#include <stdint.h>
#include <time.h>

// Android's pthread_attr_t/mutex_t/cond_t reinterpreted: the first machine
// word of the game's storage holds a pointer to the real newlib object.
typedef struct {
  pthread_attr_t *real_ptr; // overlays bionic `uint32_t flags`
  int32_t magic;            // overlays bionic `void *stack_base` low word
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
} pthread_attr_t_bionic;

// Bionic's LP64 mutex (40 bytes) / cond (48) storage carries {real_ptr,
// magic} in its first 16 bytes: the per-call "initialized yet?" check is one
// atomic load. Statically initialized storage is zero past the kind word, so
// magic == 0 means uninitialized.
typedef struct { pthread_mutex_t *real_ptr; uint32_t magic; } pthread_mutex_t_bionic;
typedef struct { pthread_cond_t  *real_ptr; uint32_t magic; } pthread_cond_t_bionic;

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param);
int pthread_join_soloader(pthread_t thread, void **value_ptr);
int pthread_detach_soloader(pthread_t thread);
int pthread_equal_soloader(pthread_t t1, pthread_t t2);
pthread_t pthread_self_soloader(void);
int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void));
int pthread_key_create_soloader(int *key, void (*dtor)(void *));
int pthread_key_delete_soloader(int key);
void *pthread_getspecific_soloader(int key);
int pthread_setspecific_soloader(int key, const void *value);
int pthread_getschedparam_soloader(pthread_t thread, int *policy, struct sched_param *param);

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr);

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond);
int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex);
int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime);
int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond);
int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond);

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex);
int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex);
int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex);
int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex);

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr);
int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr);
int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state);
int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize);

// thread records: the main thread's (call once, first thing), and any
// thread's kernel handle and stack bounds (0 when unknown or finished)
void pthr_register_main(void);
#include <switch.h>
int pthr_thread_info(pthread_t t, Handle *h, uintptr_t *lo, uintptr_t *hi);
void pthr_current_stack(uintptr_t *lo, uintptr_t *hi);
// a thread's name (the game's pthread_setname_np, prctl PR_SET_NAME)
void pthr_set_name(pthread_t t, const char *name);
// the busiest threads (up to top) since the last call, as
// "name/handle pct%" (of one core) into out
void pthr_cpu_watch(Handle h, const char *name); // a port thread in the [perf] cpu list
void pthr_cpu_report(char *out, size_t n, int top);
// the main thread and every live thread made through the wrapper: kernel
// handles and stack bounds, at most max; returns the count (watchdog.c)
int pthr_threads(Handle *h, uintptr_t *lo, uintptr_t *hi, int max);
// the same with each one's pthread_t, waiting for the list's lock: for the
// garbage collector, before it pauses anyone (gcstop.c)
int pthr_snapshot(pthread_t *t, Handle *h, uintptr_t *lo, uintptr_t *hi, int max);
// a timed wait's deadline (on bionic's real-time or monotonic clock) as a
// deadline on newlib's real-time clock
void pthr_deadline_to_realtime(const struct timespec *in, struct timespec *out);

// installs a fake RW TLS block (TPIDR_EL0) for the current thread so the
// game's stack-protector cookie reads land on valid memory; call once per
// thread that runs game code (the trampoline does this for spawned threads)
void pthr_install_fake_tls(void);

// idempotent variant for threads not created through our wrapper (SDL audio
// thread, OpenSL ThreadPool workers): installs only when TPIDR_EL0 is unset.
// Safe to call before every excursion into game code.
void pthr_ensure_fake_tls(void);

// pin the calling thread to a background core (off the render core); for
// threads spawned outside our pthread trampoline (SDL audio, OpenSL)
void pthr_pin_bg_core(void);

// the main thread joins the game's threads: priority 59 on every application
// core (pthr.c, "scheduling"); logs what it was and what it became
void pthr_sched_main(void);

// round-robin background pin used by the vendored OpenSL helper threads
void pthr_pin_worker_core(void);

// config main_core: the first application core for the main thread alone (call on it, after the config)
void pthr_dedicate_main_core(void);

int pthr_app_cores(void);

#endif
