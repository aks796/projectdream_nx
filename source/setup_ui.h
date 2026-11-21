/* setup_ui.h -- first-run setup screen on the libnx console (setup_ui.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SETUP_UI_H__
#define __SETUP_UI_H__

// note: why this is happening; what: the current step; permille: 0..1000
void setup_ui_progress(const char *note, const char *what, int permille);
int setup_ui_message(const char *title, const char *body);
// hand the window to EGL, permanently
void setup_ui_close(void);
// the console can no longer be used (EGL has the window)
int setup_ui_retired(void);

#endif
