/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef RECOVERY_COMMON_H
#define RECOVERY_COMMON_H

#include <stdio.h>

// Initialize the graphics system.
void ui_init();

// Use KEY_* codes from <linux/input.h> or KEY_DREAM_* from "minui/minui.h".
int ui_wait_key();            // waits for a key/button press, returns the code
int ui_key_pressed(int key);  // returns >0 if the code is currently pressed
int ui_text_visible();        // returns >0 if text log is currently visible
void ui_clear_key_queue();

// Write a message to the on-screen log shown with Alt-L (also to stderr).
// The screen is small, and users may need to report these messages to support,
// so keep the output short and not too cryptic.
void ui_print(const char *fmt, ...);

void ui_reset_text_col();
void ui_set_show_text(int value);

// Display some header text followed by a menu of items, which appears
// at the top of the screen (in place of any scrolling ui_print()
// output, if necessary).
int ui_start_menu(char** headers, char** items);
// Set the menu highlight to the given index, and return it (capped to
// the range [0..numitems).
int ui_menu_select(int sel);
// End menu mode, resetting the text overlay so that ui_print()
// statements will be displayed.
void ui_end_menu();

int ui_get_showing_back_button();
void ui_set_showing_back_button(int showBackButton);

// Set the icon (normally the only thing visible besides the progress bar).
enum {
  BACKGROUND_ICON_NONE,
  BACKGROUND_ICON_INSTALLING,
  BACKGROUND_ICON_ERROR,
  BACKGROUND_ICON_FIRMWARE_INSTALLING,
  BACKGROUND_ICON_FIRMWARE_ERROR,
  NUM_BACKGROUND_ICONS
};
void ui_set_background(int icon);

// Get a malloc'd copy of the screen image showing (only) the specified icon.
// Also returns the width, height, and bits per pixel of the returned image.
// TODO: Use some sort of "struct Bitmap" here instead of all these variables?
char *ui_copy_image(int icon, int *width, int *height, int *bpp);

// Show a progress bar and define the scope of the next operation:
//   portion - fraction of the progress bar the next operation will use
//   seconds - expected time interval (progress bar moves at this minimum rate)
void ui_show_progress(float portion, int seconds);
void ui_set_progress(float fraction);  // 0.0 - 1.0 within the defined scope

// Default allocation of progress bar segments to operations
static const int VERIFICATION_PROGRESS_TIME = 60;
static const float VERIFICATION_PROGRESS_FRACTION = 0.25;
static const float DEFAULT_FILES_PROGRESS_FRACTION = 0.4;
static const float DEFAULT_IMAGE_PROGRESS_FRACTION = 0.1;

// Show a rotating "barberpole" for ongoing operations.  Updates automatically.
void ui_show_indeterminate_progress();

// Hide and reset the progress bar.
void ui_reset_progress();

#define LOGE(...) ui_print("E:" __VA_ARGS__)
#define LOGW(...) fprintf(stderr, "W:" __VA_ARGS__)
#define LOGI(...) fprintf(stderr, "I:" __VA_ARGS__)

#if 0
#define LOGV(...) fprintf(stderr, "V:" __VA_ARGS__)
#define LOGD(...) fprintf(stderr, "D:" __VA_ARGS__)
#else
#define LOGV(...) do {} while (0)
#define LOGD(...) do {} while (0)
#endif

// Dream-specific key codes by LeshaK
// Modify for Samsung Spica i5700

#define KEY_DREAM_HOME        227  // = KEY_HOME
#define KEY_DREAM_RED         249  // = KEY_END
#define KEY_DREAM_VOLUMEDOWN  209  // = KEY_VOLUMEDOWN
#define KEY_DREAM_VOLUMEUP    201  // = KEY_VOLUMEUP
#define KEY_DREAM_SYM         127  // = KEY_COMPOSE
#define KEY_DREAM_MENU        211  // = KEY_MENU
#define KEY_DREAM_BACK        212  // = KEY_BACK
#define KEY_DREAM_FOCUS       211  // = KEY_HP (light touch on camera)
#define KEY_DREAM_CAMERA      250  // = KEY_CAMERA
#define KEY_DREAM_AT          215  // = KEY_EMAIL
#define KEY_DREAM_GREEN       231
#define KEY_DREAM_FATTOUCH    258  // = BTN_2 ???
#define KEY_DREAM_BALL        272  // = BTN_MOUSE
#define KEY_DREAM_TOUCH       330  // = BTN_TOUCH

// For Samsung by LeshaK
#define KEY_I5700_CENTER      204
#define KEY_I5700_DOWN	      210
#define KEY_I5700_UP          202

//Redefine defaults
#undef KEY_HOME
#define KEY_HOME KEY_DREAM_HOME
#undef KEY_END
#define KEY_END KEY_DREAM_RED
#undef KEY_VOLUMEDOWN
#define KEY_VOLUMEDOWN KEY_DREAM_VOLUMEDOWN
#undef KEY_VOLUMEUP
#define KEY_VOLUMEUP KEY_DREAM_VOLUMEUP
#undef KEY_COMPOSE
#define KEY_COMPOSE KEY_DREAM_SYM
#undef KEY_MENU
#define KEY_MENU KEY_DREAM_MENU
#undef KEY_BACK
#define KEY_BACK KEY_DREAM_BACK
#undef KEY_HP
#define KEY_HP KEY_DREAM_FOCUS
#undef KEY_CAMERA
#define KEY_CAMERA KEY_DREAM_CAMERA
#undef KEY_EMAIL
#define KEY_EMAIL KEY_DREAM_AT
#undef BTN_2
#define BTN_2 KEY_DREAM_FATTOUCH
#undef BTN_MOUSE
#define BTN_MOUSE KEY_DREAM_BALL
#undef BTN_TOUCH
#define BTN_TOUCH KEY_DREAM_TOUCH
#undef KEY_DOWN
#define KEY_DOWN KEY_I5700_DOWN
#undef KEY_UP
#define KEY_UP KEY_I5700_UP
#undef KEY_SEND
#define KEY_SEND KEY_I5700_CENTER

#define STRINGIFY(x) #x
#define EXPAND(x) STRINGIFY(x)
//#define DEBUG 1

#endif  // RECOVERY_COMMON_H
