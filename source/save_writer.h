/* save_writer.h -- the game's saves written to the SD card on a thread of the port's (save_writer.c) */
#ifndef SAVE_WRITER_H
#define SAVE_WRITER_H

#include <stddef.h>
#include <stdio.h>
#include <switch.h>

int sw_is_save_path(const char *p);
int sw_is_memfd(int fd);
int sw_open(const char *p, int flags); // a memory file's fd, or -2: the card's as usual
long sw_read(int fd, void *buf, size_t n);
long sw_write(int fd, const void *buf, size_t n);
long sw_lseek(int fd, long off, int whence);
int sw_ftruncate(int fd, long len);
long sw_size(int fd);
int sw_close(int fd);
int sw_rename(const char *from, const char *to); // 0 done, -2: the card's as usual
int sw_stat(const char *p, size_t *size);        // 1: pending, *size its newest size
void sw_wait_path(const char *p);
FILE *sw_fopen(const char *p, const char *mode, int *handled);
int sw_fileno(FILE *f);
void sw_file_closed(FILE *f);
void sw_flush_all(u64 timeout_ns);
void sw_set_urgent(int on); // 1: no waiting on the queue (the game in the background: it may be closed)
void sw_stats(u64 *saves, u64 *bytes, u64 *written, u64 *card_ns, u64 *card_max_ns);

#endif
