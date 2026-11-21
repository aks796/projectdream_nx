/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"
#include "log.h"
#include "setup_ui.h"

void fatal_error(const char *fmt, ...) {
  {
    char msg[1024];
    va_list va;
    va_start(va, fmt);
    vsnprintf(msg, sizeof(msg), fmt, va);
    va_end(va);
    log_printf("[fatal] %s", msg);
    log_flush();

    // once EGL has the window the console must not take it back: use the
    // system's error dialog instead
    if (setup_ui_retired()) {
      ErrorApplicationConfig c;
      if (R_SUCCEEDED(errorApplicationCreate(&c, msg, NULL)))
        errorApplicationShow(&c);
      exit(1);
    }
  }

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  va_list list;
  va_start(list, fmt);
  vprintf(fmt, list);
  va_end(list);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}
