/*
 * Ui.h - the state of the keyboard and what ends up on the displays.
 */

#pragma once
#include "Config.h"
#include "Input.h"

void uiBegin();

// A button event: this is where it is decided what pressing a key means right
// now (a macro, picking a page, picking a file).
void uiHandleEvent(const KeyEvent &event);

// Deadlines: pending second actions, the countdown bar, the confirmation flash,
// entering standby.
void uiTick();

// Redraws the displays that have changed and nothing else.
void uiRender();

// System actions of a key (standby, page menu, jump to a page).
void uiDeviceAction(uint8_t action, uint16_t page);

// Loads a profile from the SD card and opens it; on error it shows the error on
// the displays and returns false.
bool uiLoadFile(const char *fileName);

// Menu of the profiles on the card. It opens at startup when needed, and while
// the keyboard is running by holding the page key for HOLD_PRESS_MS.
void uiOpenFileMenu();

// True while that menu is on screen: the LED signals it even with a profile
// already open.
bool uiFileMenuOpen();

// Error screen. As long as it stays up, the page key reopens the profile menu,
// so a broken file can be left behind without pulling the plug.
void uiShowError(const char *title, const char *body);
