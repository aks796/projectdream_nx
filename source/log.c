/* log.c -- debug.log next to the .nro
 *
 * Every line also goes to svcOutputDebugString (an attached debugger, an
 * emulator's log) -- from the log's thread once it runs (round 13: the main
 * thread spent 1-2% of a profile window in the call, printing the reports).
 * Lines go into a buffer in memory and a thread of the
 * log's own writes them to the card (a moment later, synced each second at
 * most), from the start: hardware round 7's card took 0.5 s a line when each
 * start-up line was written and synced by the thread that logged it (50 s
 * more start-up), and its writes took the game's thread tens of ms now and
 * then before that. A crash's handler, Exit and fatal errors call log_flush,
 * which writes what is left at once. The previous launch's log is kept as
 * debug.prev.log.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#include "log.h"

static FILE *s_log;
static Mutex s_file;   // the file
static Mutex s_lock;   // the buffer (held a moment)
static int s_boot_done, s_async;
static unsigned s_unsynced;

#define RING (256 * 1024)
static char s_ring[RING];
static size_t s_head, s_tail;  // bytes put in / written out, ever (s_head - s_tail waiting)
static unsigned s_dropped;     // lines that found the buffer full
static UEvent s_more;
static Thread s_writer;

static void start_thread(void);

void log_init(const char *dir) {
  char path[600], prev[600];
  snprintf(path, sizeof(path), "%s/debug.log", dir);
  snprintf(prev, sizeof(prev), "%s/debug.prev.log", dir);
  remove(prev);
  rename(path, prev);
  s_log = fopen(path, "w");
  start_thread();
}

// the buffer's lines into the file (s_file held, or not to be had: a crash)
static void drain(void) {
  for (;;) {
    mutexLock(&s_lock);
    const size_t head = s_head, tail = s_tail;
    const unsigned dropped = s_dropped;
    s_dropped = 0;
    mutexUnlock(&s_lock);
    if (dropped)
      fprintf(s_log, "[log] %u lines lost (the log's buffer full)\n", dropped);
    if (head == tail)
      return;
    // (the bytes up to the buffer's end at most: the rest next time round;
    // no one writes over them until s_tail passes them)
    const size_t at = tail % RING, n = head - tail < RING - at ? head - tail : RING - at;
    fwrite(s_ring + at, 1, n, s_log);
    svcOutputDebugString(s_ring + at, n);
    mutexLock(&s_lock);
    s_tail += n;
    mutexUnlock(&s_lock);
  }
}

static void writer(void *arg) {
  (void)arg;
  u64 synced = armGetSystemTick();
  for (;;) {
    waitSingle(waiterForUEvent(&s_more), 1000000000ll);
    svcSleepThread(30000000ll); // (a moment's lines together)
    mutexLock(&s_file);
    drain();
    fflush(s_log);
    const u64 now = armGetSystemTick();
    if (s_unsynced && armTicksToNs(now - synced) >= 1000000000ull) {
      fsync(fileno(s_log));
      s_unsynced = 0;
      synced = now;
    }
    mutexUnlock(&s_file);
  }
}

static void start_thread(void) {
  if (!s_log || s_async)
    return;
  ueventCreate(&s_more, true);
  if (R_SUCCEEDED(threadCreate(&s_writer, writer, NULL, NULL, 0x4000, 0x3b, 2)) && R_SUCCEEDED(threadStart(&s_writer)))
    s_async = 1;
}

void log_boot_done(void) {
  s_boot_done = 1;
  start_thread();
  if (!s_async)
    log_printf("[log] its thread NOT started: written as it comes");
}

void log_printf(const char *fmt, ...) {
  char line[1200];
  const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ull;
  int n = snprintf(line, sizeof(line), "[%6llu.%03llu] ", (unsigned long long)(ms / 1000),
                   (unsigned long long)(ms % 1000));
  va_list va;
  va_start(va, fmt);
  vsnprintf(line + n, sizeof(line) - n - 1, fmt, va);
  va_end(va);

  size_t len = strlen(line);
  if (!s_log || !s_async)
    svcOutputDebugString(line, len);
  if (!s_log)
    return;
  line[len++] = '\n';
  if (s_async) {
    mutexLock(&s_lock);
    if (s_head - s_tail + len > RING) {
      s_dropped++;
    } else {
      const size_t at = s_head % RING, first = len < RING - at ? len : RING - at;
      memcpy(s_ring + at, line, first);
      memcpy(s_ring, line + first, len - first);
      s_head += len;
      s_unsynced++;
    }
    mutexUnlock(&s_lock);
    ueventSignal(&s_more);
    return;
  }
  mutexLock(&s_file);
  fwrite(line, 1, len, s_log);
  fflush(s_log);
  if (!s_boot_done || ++s_unsynced >= 16) {
    fsync(fileno(s_log));
    s_unsynced = 0;
  }
  mutexUnlock(&s_file);
}

void log_flush(void) {
  if (!s_log)
    return;
  // (the writer may be in the file: waited for a moment, then written over
  // it, as a crash may have stopped it there)
  int held = 0;
  for (int i = 0; i < 300 && !(held = mutexTryLock(&s_file)); i++)
    svcSleepThread(1000000ll);
  drain();
  fflush(s_log);
  fsync(fileno(s_log));
  s_unsynced = 0;
  if (held)
    mutexUnlock(&s_file);
}
