/* log.h -- debug.log next to the .nro
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __LOG_H__
#define __LOG_H__

// Opens <dir>/debug.log (truncated). Before this, lines are dropped.
void log_init(const char *dir);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_flush(void);
// start-up is over: stop syncing the file on every line
void log_boot_done(void);

#endif
