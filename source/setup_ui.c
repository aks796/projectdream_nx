/* setup_ui.c -- the first-run setup screen (the PvZ Touch port's green bar):
 * the title in green, what is going on, a bar with the percentage and the
 * current step, drawn on the libnx console.
 *
 * The console owns the default window until setup_ui_close(), which happens
 * before EGL takes it -- for good: once Mesa has used the window the console
 * must never take it back (Mesa registers 3 buffer slots, the console 2; the
 * console's third frame then fails to dequeue and libnx aborts).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "setup_ui.h"
#include "log.h"

#define TITLE "Mario Kart Tour"

static int s_open, s_retired;

int setup_ui_retired(void) {
  return s_retired;
}

static void open_console(void) {
  if (s_open || s_retired || !nwindowIsValid(nwindowGetDefault()))
    return;
  consoleInit(NULL);
  s_open = 1;
}

static int centre(int cols, int n) {
  return n < cols ? (cols - n) / 2 + 1 : 1;
}

void setup_ui_progress(const char *note, const char *what, int permille) {
  static char last_what[96], last_note[96];
  static int last_pct = -1;
  open_console();
  if (!s_open)
    return;
  if (!what)
    what = "";
  if (!note)
    note = "";
  if (permille < 0)
    permille = 0;
  if (permille > 1000)
    permille = 1000;
  const int pct = permille / 10;
  if (pct == last_pct && !strncmp(what, last_what, sizeof(last_what) - 1) &&
      !strncmp(note, last_note, sizeof(last_note) - 1))
    return; // a console frame waits for the display: draw only on change
  last_pct = pct;
  snprintf(last_what, sizeof(last_what), "%s", what);
  snprintf(last_note, sizeof(last_note), "%s", note);

  enum { COLS = 80, BAR = 56 };
  const int fill = permille * BAR / 1000;
  char bar[BAR + 1];
  for (int i = 0; i < BAR; i++)
    bar[i] = i < fill ? '#' : '-';
  bar[BAR] = 0;
  printf("\x1b[2J");
  printf("\x1b[18;%dH\x1b[32;1m%s\x1b[0m", centre(COLS, (int)strlen(TITLE)), TITLE);
  printf("\x1b[20;%dH%s", centre(COLS, (int)strlen(last_note)), last_note);
  printf("\x1b[23;%dH[\x1b[32m%s\x1b[0m] %3d%%", (COLS - BAR - 7) / 2 + 1, bar, pct);
  printf("\x1b[25;%dH%s", centre(COLS, (int)strlen(last_what)), last_what);
  fflush(stdout);
  consoleUpdate(NULL);
}

// A message the player has to read before setup can go on (no game files,
// unsupported APK): shown until A is pressed. Returns 1 if A, 0 if the app
// is being closed.
int setup_ui_message(const char *title, const char *body) {
  open_console();
  if (!s_open)
    return 1;
  printf("\x1b[2J\x1b[3;3H\x1b[33;1m%s\x1b[0m\n\n", title);
  // indent the body two columns
  for (const char *p = body; *p;) {
    const char *nl = strchr(p, '\n');
    const int len = nl ? (int)(nl - p) : (int)strlen(p);
    printf("  %.*s\n", len, p);
    p = nl ? nl + 1 : p + len;
  }
  printf("\n  Press A to continue.");
  consoleUpdate(NULL);

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_A)
      return 1;
    consoleUpdate(NULL);
  }
  return 0;
}

void setup_ui_close(void) {
  if (s_open) {
    consoleExit(NULL);
    s_open = 0;
  }
  s_retired = 1;
}
