/*
 * Display.h - the five OLEDs behind the multiplexer.
 *
 * This module knows how to draw, not what to draw: it receives the three rows
 * of a column ready to go (text + inverted flag) and sends them to the right
 * display. Deciding the contents is Ui.cpp's job.
 */

#pragma once
#include "Config.h"

// The three rows of a column display, from top to bottom.
// "bar" is the countdown bar: how many pixels of the row it still covers,
// counted from the left (SCREEN_W = the whole row, 0 = no bar). It runs behind
// the text and flips the row for that stretch, so the row carrying it has to be
// sent plain: that way the bar comes out white with black letters.
struct ScreenRows {
  const char *text[ROWS_PER_COL];
  bool        inverted[ROWS_PER_COL];
  uint8_t     bar[ROWS_PER_COL];
};

// The two pickers the page key can open: only the heading prompt changes, the
// rest of the menu is identical.
enum MenuKind : uint8_t {
  MENU_PAGES = 0,
  MENU_PROFILES
};

// Powers up the bus, looks for the multiplexer and initialises the displays
// that are there. Returns false only if the PCA9548A does not answer (without
// it nothing can be seen); missing displays are counted in "missing" and simply
// skipped by every drawing function.
bool displaysBegin(uint8_t &missing);

// Standby: switches the matrix of every display off or back on. The buffer
// contents stay, so the same page reappears on wake-up.
void displaysPower(bool on);

// Redraws a column. With onlyRow >= 0 just the band of that row is sent to the
// display (a third of the screen, hence a third of the time): that is what the
// confirmation flash and the countdown bar use.
void renderColumn(uint8_t col, const ScreenRows &rows, int8_t onlyRow = -1);

// ---- page key display, one drawing per mode ----
// The list of pages: one to three rows, splitting the screen into equal parts.
// When there are more pages than rows the names get slightly narrower and the
// bar cursor runs in the black strip left over on the right.
void renderPageList(const char *const names[], const bool inverted[],
                    uint8_t rowCount, uint16_t firstShown, uint16_t total);

// A single page, centred on the screen.
void renderPageSingle(const char *name, bool inverted);

// Menu heading: icon and prompt to pick something (see MenuKind), plus the
// "2/3" screen counter when there is more than one screen.
void renderPageMenu(uint8_t kind, uint8_t index, uint8_t total);

// The page key acting as "back", while a subpage is on screen.
void renderPageBack();

// A message across the whole keyboard: the title on the page key display, the
// body wrapped over the twelve rows of the four columns, without the lines that
// separate the keys (here the rows are one single sentence, not twelve labels).
// It is the only way the firmware has to tell about an error when there is no
// serial port.
void renderMessage(const char *title, const char *body);
