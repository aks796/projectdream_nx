/* save_writer.c -- the game's saves written to the SD card behind its back, not on its main thread
 *
 * The console's profiles: the main thread stalled in its saves, up to 19% of a 3 s window (d_write, close of
 * ext/PrimaryHL_Default_Save_ID: the game rewrites its 92 KB save every 10-20 s in cutscenes and at its checkpoints;
 * Project Dream's iCloud key-value file, fopen + fwrite + fsync + fclose + rename on every put). On the SD card each
 * truncate, write, flush and close is a round trip with the card's slow small writes: a hitch at every save.
 *
 * Here a save file opened to be rewritten (truncated) is a file in memory: the game writes, seeks and closes it as
 * usual, then a thread of the port's writes it to the card. Nothing the game's threads do waits for the card:
 *  - a closed save waits 2 s on the queue (WRITE_DELAY), and a rewrite in that time replaces it: the game rewrote
 *    its save, its backup and Project Dream's key-value file many times a second at its checkpoints;
 *  - the card's file is rewritten in place (no new file, no rename): the fewest of the card's slow metadata writes.
 *    The game keeps a backup of its save itself, as it did when it truncated and rewrote the file;
 *  - a rename of a save not on the card yet (the key-value file's temp file into place) only changes where its
 *    pending write goes; the temp name is removed from the card by the writer afterwards.
 * Opening a pending save to read it gives the new contents from memory; stat gives its new size. Only an open to
 * change a pending save in place, an unlink of one, or a rename with nothing pending waits for the card.
 * Exit, and the game going to the background (HOME, sleep: it may be closed from there), write them at once.
 *
 * Hardware round 7: the card took up to 3 s a save (and 0.5 s a log line); the first version waited for a write
 * under way in rename() and removed the temp file on the game's thread: frames of 5-32 s.
 *
 * The files: the game's saves (ext/...Save_ID...) and Project Dream's key-value store (PCSaveData/).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE // fopencookie
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "log.h"

#define B_ACCMODE 3 // bionic open flags
#define B_RDONLY 0
#define B_TRUNC 0x200
#define B_APPEND 0x400

#define MEMFD_BASE 0x5f00
#define NMEM 16
#define WRITE_DELAY_NS 2000000000ull // a closed save's time on the queue (rewrites in it replace it)

typedef struct {
  int used;
  int writable;
  char path[600];
  unsigned char *buf;
  size_t size, cap, pos;
} MemFile;

typedef struct Job {
  char path[600];         // where it goes (a rename while it is being written changes it: the writer renames)
  char remove_after[600]; // a name to take off the card once it is written (a rename's old name)
  unsigned char *buf;
  size_t size;
  u64 queued;             // when its first unwritten version was closed
  struct Job *next;
} Job;

static Mutex s_lock;
static CondVar s_cv;       // a job queued / one finished
static Job *s_head, *s_tail;
static Job *s_active;      // the one being written (not in the list)
static MemFile s_mem[NMEM];
static Thread s_thread;
static int s_thread_up;
static int s_flush_now;    // Exit, the game in the background: no waiting on the queue
static u64 s_saves, s_bytes, s_card_ns, s_card_max_ns, s_written;

int sw_is_save_path(const char *p) {
  return p && ((strstr(p, "/ext/") && strstr(p, "Save_ID")) || strstr(p, "/PCSaveData/"));
}

int sw_is_memfd(int fd) {
  return fd >= MEMFD_BASE && fd < MEMFD_BASE + NMEM && s_mem[fd - MEMFD_BASE].used;
}

// the newest queued or active job for a path (lock held)
static Job *job_for(const char *p) {
  Job *found = NULL;
  if (s_active && !strcmp(s_active->path, p))
    found = s_active;
  for (Job *j = s_head; j; j = j->next)
    if (!strcmp(j->path, p))
      found = j;
  return found;
}

static int write_all(int fd, const unsigned char *buf, size_t size) {
  size_t done = 0;
  while (done < size) {
    const ssize_t n = write(fd, buf + done, size - done);
    if (n <= 0)
      return 0;
    done += (size_t)n;
  }
  return 1;
}

// j's contents onto the card at `at` (the path it had when the writer took it), rewritten in place
static void write_job(Job *j, const char *at) {
  const u64 t0 = armGetSystemTick();
  int ok = 0;
  int fd = open(at, O_WRONLY);
  if (fd < 0)
    fd = open(at, O_WRONLY | O_CREAT, 0666);
  if (fd >= 0) {
    struct stat st;
    const off_t old = fstat(fd, &st) == 0 ? st.st_size : -1;
    ok = write_all(fd, j->buf, j->size);
    if (ok && old != (off_t)j->size && ftruncate(fd, (off_t)j->size) != 0)
      ok = 0;
    if (close(fd) != 0)
      ok = 0;
  }
  if (!ok)
    log_printf("[save] could not write %s (errno %d): the game's save kept in memory only", at, errno);
  // renamed while it was being written: to the new name (Horizon's rename refuses an existing destination)
  char dest[600], gone[600];
  mutexLock(&s_lock);
  snprintf(dest, sizeof dest, "%s", j->path);
  snprintf(gone, sizeof gone, "%s", j->remove_after);
  mutexUnlock(&s_lock);
  if (ok && strcmp(dest, at)) {
    remove(dest);
    if (rename(at, dest) != 0)
      log_printf("[save] could not rename %s to %s (errno %d)", at, dest, errno);
  }
  if (gone[0] && strcmp(gone, dest))
    remove(gone);
  const u64 ns = armTicksToNs(armGetSystemTick() - t0);
  mutexLock(&s_lock);
  s_card_ns += ns;
  if (ns > s_card_max_ns)
    s_card_max_ns = ns;
  s_written++;
  mutexUnlock(&s_lock);
}

static void writer(void *arg) {
  (void)arg;
  mutexLock(&s_lock);
  for (;;) {
    while (!s_head)
      condvarWait(&s_cv, &s_lock);
    Job *j = s_head;
    const u64 age = armTicksToNs(armGetSystemTick() - j->queued);
    if (!s_flush_now && age < WRITE_DELAY_NS) {
      condvarWaitTimeout(&s_cv, &s_lock, WRITE_DELAY_NS - age);
      continue;
    }
    s_head = j->next;
    if (!s_head)
      s_tail = NULL;
    j->next = NULL;
    s_active = j;
    char at[600];
    snprintf(at, sizeof at, "%s", j->path);
    mutexUnlock(&s_lock);
    write_job(j, at);
    mutexLock(&s_lock);
    s_active = NULL;
    free(j->buf);
    free(j);
    condvarWakeAll(&s_cv);
  }
}

static void start_writer(void) {
  if (s_thread_up)
    return;
  // priority 59 like the game's threads, on the cores off the main thread's (pthr.c gives core 0 to it)
  if (R_SUCCEEDED(threadCreate(&s_thread, writer, NULL, NULL, 0x8000, 0x3B, 1))) {
    svcSetThreadCoreMask(s_thread.handle, 1, 0x6);
    threadStart(&s_thread);
    s_thread_up = 1;
  }
}

// a closed save's contents to the card (lock held): a queued, not started write of the same file is replaced
static void queue(const char *path, unsigned char *buf, size_t size) {
  for (Job *j = s_head; j; j = j->next)
    if (!strcmp(j->path, path)) {
      free(j->buf);
      j->buf = buf;
      j->size = size;
      s_saves++, s_bytes += size;
      return;
    }
  Job *j = calloc(1, sizeof *j);
  if (!j) { // no memory for the record: written here and now
    Job now = { .buf = buf, .size = size };
    snprintf(now.path, sizeof now.path, "%s", path);
    mutexUnlock(&s_lock);
    write_job(&now, now.path);
    mutexLock(&s_lock);
    free(buf);
    return;
  }
  snprintf(j->path, sizeof j->path, "%s", path);
  j->buf = buf;
  j->size = size;
  j->queued = armGetSystemTick();
  if (s_tail)
    s_tail->next = j;
  else
    s_head = j;
  s_tail = j;
  s_saves++, s_bytes += size;
  start_writer();
  condvarWakeAll(&s_cv);
}

// until no write of p is queued or under way (lock held)
static void wait_path_locked(const char *p) {
  while (job_for(p))
    condvarWait(&s_cv, &s_lock);
}

void sw_wait_path(const char *p) {
  if (!sw_is_save_path(p))
    return;
  mutexLock(&s_lock);
  wait_path_locked(p);
  mutexUnlock(&s_lock);
}

static int new_mem(const char *p, int writable) {
  for (int i = 0; i < NMEM; i++)
    if (!s_mem[i].used) {
      MemFile *m = &s_mem[i];
      memset(m, 0, sizeof *m);
      m->used = 1;
      m->writable = writable;
      snprintf(m->path, sizeof m->path, "%s", p);
      return MEMFD_BASE + i;
    }
  return -1;
}

// open(2) of a save: a memory file's fd, or -2 when the card's file is to be opened as usual
int sw_open(const char *p, int flags) {
  if (!sw_is_save_path(p))
    return -2;
  const int acc = flags & B_ACCMODE;
  mutexLock(&s_lock);
  int fd = -2;
  if (acc != B_RDONLY && (flags & B_TRUNC) && !(flags & B_APPEND)) {
    fd = new_mem(p, 1);
  } else if (acc == B_RDONLY) {
    Job *j = job_for(p);
    if (j && (fd = new_mem(p, 0)) >= 0) { // the newest contents, not on the card yet
      MemFile *m = &s_mem[fd - MEMFD_BASE];
      m->buf = malloc(j->size ? j->size : 1);
      if (m->buf) {
        memcpy(m->buf, j->buf, j->size);
        m->size = m->cap = j->size;
      } else {
        m->used = 0;
        fd = -2;
        wait_path_locked(p);
      }
    }
  } else {
    wait_path_locked(p); // changed in place: on the card first
  }
  if (fd == -1) { // no memory file free: as usual, after any write of it
    wait_path_locked(p);
    fd = -2;
  }
  mutexUnlock(&s_lock);
  return fd;
}

long sw_read(int fd, void *buf, size_t n) {
  MemFile *m = &s_mem[fd - MEMFD_BASE];
  if (m->pos >= m->size)
    return 0;
  if (n > m->size - m->pos)
    n = m->size - m->pos;
  memcpy(buf, m->buf + m->pos, n);
  m->pos += n;
  return (long)n;
}

long sw_write(int fd, const void *buf, size_t n) {
  MemFile *m = &s_mem[fd - MEMFD_BASE];
  if (!m->writable) {
    errno = EBADF;
    return -1;
  }
  const size_t end = m->pos + n;
  if (end > m->cap) {
    size_t cap = m->cap ? m->cap : 128 * 1024;
    while (cap < end)
      cap *= 2;
    unsigned char *b = realloc(m->buf, cap);
    if (!b) {
      errno = ENOSPC;
      return -1;
    }
    memset(b + m->cap, 0, cap - m->cap);
    m->buf = b;
    m->cap = cap;
  }
  memcpy(m->buf + m->pos, buf, n);
  m->pos = end;
  if (end > m->size)
    m->size = end;
  return (long)n;
}

long sw_lseek(int fd, long off, int whence) {
  MemFile *m = &s_mem[fd - MEMFD_BASE];
  const long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)m->pos : (long)m->size;
  const long to = base + off;
  if (to < 0) {
    errno = EINVAL;
    return -1;
  }
  m->pos = (size_t)to;
  return to;
}

int sw_ftruncate(int fd, long len) {
  MemFile *m = &s_mem[fd - MEMFD_BASE];
  if (!m->writable || len < 0) {
    errno = EINVAL;
    return -1;
  }
  if ((size_t)len > m->size) {
    const size_t pos = m->pos;
    m->pos = m->size;
    static const unsigned char zero[256];
    while (m->size < (size_t)len)
      if (sw_write(fd, zero, (size_t)len - m->size > sizeof zero ? sizeof zero : (size_t)len - m->size) < 0)
        return -1;
    m->pos = pos;
  }
  m->size = (size_t)len;
  return 0;
}

long sw_size(int fd) {
  return (long)s_mem[fd - MEMFD_BASE].size;
}

int sw_close(int fd) {
  MemFile *m = &s_mem[fd - MEMFD_BASE];
  mutexLock(&s_lock);
  if (m->writable) {
    queue(m->path, m->buf, m->size); // (the job owns the buffer now)
  } else {
    free(m->buf);
  }
  m->buf = NULL;
  m->used = 0;
  mutexUnlock(&s_lock);
  return 0;
}

// drop the queued jobs (other than keep) writing `path` (lock held)
static void drop_queued(const char *path, const Job *keep) {
  for (Job **pp = &s_head; *pp;) {
    Job *k = *pp;
    if (k != keep && !strcmp(k->path, path)) {
      *pp = k->next;
      free(k->buf);
      free(k);
    } else {
      pp = &k->next;
    }
  }
  s_tail = s_head;
  while (s_tail && s_tail->next)
    s_tail = s_tail->next;
}

// rename(2) of a save: a pending write of `from` goes to `to` instead (0, nothing waited for); -2 when nothing of
// it is pending (the card's file is renamed as usual)
int sw_rename(const char *from, const char *to) {
  if (!sw_is_save_path(from) || !sw_is_save_path(to))
    return -2;
  mutexLock(&s_lock);
  Job *j = NULL; // the newest queued write of `from`
  for (Job *k = s_head; k; k = k->next)
    if (!strcmp(k->path, from))
      j = k;
  int r = -2;
  if (j) {
    // what was queued for either name is older than this rename: `to` gets j's contents, `from` is no more
    drop_queued(to, j);
    drop_queued(from, j);
    snprintf(j->path, sizeof j->path, "%s", to);
    snprintf(j->remove_after, sizeof j->remove_after, "%s", from);
    r = 0;
  } else if (s_active && !strcmp(s_active->path, from)) {
    // under way: the writer renames it once written
    drop_queued(to, NULL);
    snprintf(s_active->path, sizeof s_active->path, "%s", to);
    r = 0;
  } else {
    wait_path_locked(to); // (a pending write of `to` must not land over the renamed file)
  }
  mutexUnlock(&s_lock);
  return r;
}

// stat of a save with a write pending: its newest size (1), or 0 when the card's file tells
int sw_stat(const char *p, size_t *size) {
  if (!sw_is_save_path(p))
    return 0;
  mutexLock(&s_lock);
  Job *j = job_for(p);
  if (j)
    *size = j->size;
  mutexUnlock(&s_lock);
  return j != NULL;
}

// ---- FILE* (Project Dream's key-value store writes with fopen/fwrite/fsync/fclose)

static ssize_t ck_read(void *c, char *buf, size_t n) { return sw_read((int)(intptr_t)c, buf, n); }
static ssize_t ck_write(void *c, const char *buf, size_t n) { return sw_write((int)(intptr_t)c, buf, n); }
static int ck_seek(void *c, off_t *off, int whence) {
  const long r = sw_lseek((int)(intptr_t)c, (long)*off, whence);
  if (r < 0)
    return -1;
  *off = r;
  return 0;
}
static int ck_close(void *c) { return sw_close((int)(intptr_t)c); }

static struct {
  FILE *f;
  int fd;
} s_files[NMEM];

// fopen of a save: a FILE over a memory file, or NULL with *handled 0 for the card's
FILE *sw_fopen(const char *p, const char *mode, int *handled) {
  *handled = 0;
  if (!sw_is_save_path(p) || !mode)
    return NULL;
  int flags;
  if (mode[0] == 'w')
    flags = (strchr(mode, '+') ? 2 : 1) | B_TRUNC;
  else if (mode[0] == 'r' && !strchr(mode, '+'))
    flags = B_RDONLY;
  else {
    sw_wait_path(p); // appends and in-place changes: the card's, after any pending write
    return NULL;
  }
  const int fd = sw_open(p, flags);
  if (fd < 0)
    return NULL;
  cookie_io_functions_t io = { ck_read, ck_write, ck_seek, ck_close };
  FILE *f = fopencookie((void *)(intptr_t)fd, mode, io);
  if (!f) {
    sw_close(fd);
    return NULL;
  }
  setvbuf(f, NULL, _IOFBF, 64 * 1024); // (whole saves at a time: 93 KB, 1 KB per cookie write before)
  mutexLock(&s_lock);
  for (int i = 0; i < NMEM; i++)
    if (!s_files[i].f) {
      s_files[i].f = f;
      s_files[i].fd = fd;
      break;
    }
  mutexUnlock(&s_lock);
  *handled = 1;
  return f;
}

// a FILE of ours: its memory fd (fileno), -1 otherwise
int sw_fileno(FILE *f) {
  int fd = -1;
  mutexLock(&s_lock);
  for (int i = 0; i < NMEM; i++)
    if (s_files[i].f == f)
      fd = s_files[i].fd;
  mutexUnlock(&s_lock);
  return fd;
}

void sw_file_closed(FILE *f) {
  mutexLock(&s_lock);
  for (int i = 0; i < NMEM; i++)
    if (s_files[i].f == f)
      s_files[i].f = NULL;
  mutexUnlock(&s_lock);
}

void sw_set_urgent(int on) {
  mutexLock(&s_lock);
  s_flush_now = on;
  condvarWakeAll(&s_cv);
  mutexUnlock(&s_lock);
}

// at exit: every pending write on the card (or the time out)
void sw_flush_all(u64 timeout_ns) {
  const u64 t0 = armGetSystemTick();
  mutexLock(&s_lock);
  s_flush_now = 1;
  condvarWakeAll(&s_cv);
  while ((s_head || s_active) && armTicksToNs(armGetSystemTick() - t0) < timeout_ns)
    condvarWaitTimeout(&s_cv, &s_lock, 50000000ull);
  const int left = s_head || s_active;
  mutexUnlock(&s_lock);
  if (left)
    log_printf("[save] leaving with a save not yet written");
}

void sw_stats(u64 *saves, u64 *bytes, u64 *written, u64 *card_ns, u64 *card_max_ns) {
  mutexLock(&s_lock);
  *saves = s_saves, *bytes = s_bytes, *written = s_written, *card_ns = s_card_ns, *card_max_ns = s_card_max_ns;
  s_saves = s_bytes = s_written = s_card_ns = s_card_max_ns = 0;
  mutexUnlock(&s_lock);
}
