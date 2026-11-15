/*
 * Ui.cpp - how the keyboard behaves.
 *
 * Five states: the normal page, the page menu, the menu of the profiles on the
 * SD card, standby and the error screen. The drawing is not redone on every
 * pass: every change marks which display (and which of its three bands) is to
 * be sent again, so the confirmation flash on a key costs a third of a screen
 * instead of five whole screens.
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "Ui.h"
#include "Display.h"
#include "Profile.h"
#include "Actions.h"

enum UiMode : uint8_t {
  UI_NORMAL = 0,   // a page of macros
  UI_MENU_PAGES,   // the pages in place of the twelve keys
  UI_MENU_FILES,   // the SD card profiles in place of the twelve keys
  UI_STANDBY,      // displays off
  UI_ERROR         // fixed message, the way out is the profile menu
};

// How many subpages can be chained before losing the way home.
static const uint8_t BACK_DEPTH = 4;

static uint8_t  mode = UI_ERROR;
static uint16_t currentPage = 0;
static uint8_t  menuScreen = 0;

static uint16_t backStack[BACK_DEPTH];
static uint8_t  backDepth = 0;

static uint32_t lastActivity = 0;

// Bits 0-2: the band to send again. Bit 7: the whole display.
static uint8_t colDirty[COL_COUNT];
static bool    pageDirty = true;

// Press confirmation: the label inverts for a moment.
struct KeyFlash { bool on; uint32_t until; };
static KeyFlash flash[KEY_COUNT];

// Pending second action. The pointer to the key stays valid because the profile
// does not move from RAM again until another one is loaded.
struct Pending {
  const KeyCfg *key;
  uint16_t      page;      // where the key was: the countdown bar is only
                           // visible if that page is still on screen
  uint32_t      dueMs;
  uint32_t      spanMs;    // the whole wait: from this and from how much is
                           // left comes how much of the row the bar still takes
  bool          countdown;
  uint8_t       lastBar;   // last width sent to the display, 0xFF = never
};
static Pending pending[KEY_COUNT];

// A key pressed to wake the keyboard up - or the one that has just sent it to
// standby - must not also do what it would normally do: the events of its id
// are thrown away until the key comes back up. The bit is cleared in uiTick()
// and not on an event: a long press produces no event at all on the release and
// its bit would stay set forever.
static uint16_t swallow = 0;

// Texts built on the fly (file names without their extension).
static char scratch[KEY_COUNT][MAX_FILENAME];

// ============================================================
// VISIBLE PAGES
// ============================================================
static uint16_t visibleCount() {
  uint16_t n = 0;
  for (uint16_t i = 0; i < profile.pageCount; i++) {
    if (profile.pages[i].visible) n++;
  }
  return n;
}

static uint16_t visibleAt(uint16_t position) {
  uint16_t n = 0;
  for (uint16_t i = 0; i < profile.pageCount; i++) {
    if (!profile.pages[i].visible) continue;
    if (n == position) return i;
    n++;
  }
  return 0;
}

// Position of a page in the list of the visible ones, -1 if it is hidden.
static int32_t visiblePos(uint16_t page) {
  int32_t n = 0;
  for (uint16_t i = 0; i < profile.pageCount; i++) {
    if (!profile.pages[i].visible) continue;
    if (i == page) return n;
    n++;
  }
  return -1;
}

// ============================================================
// DISPLAYS TO SEND AGAIN
// ============================================================
static void markAll() {
  for (uint8_t c = 0; c < COL_COUNT; c++) colDirty[c] = 0x80;
  pageDirty = true;
}

static void markKey(uint8_t id) {
  if (id >= KEY_COUNT) return;
  colDirty[id / ROWS_PER_COL] |= (1 << (id % ROWS_PER_COL));
}

// ============================================================
// MENUS
// ============================================================
static uint16_t menuItemCount() {
  return (mode == UI_MENU_FILES) ? profileFileCount() : visibleCount();
}

static uint8_t menuScreenCount() {
  const uint16_t items = menuItemCount();
  if (items == 0) return 1;
  return (items + KEY_COUNT - 1) / KEY_COUNT;
}

static void openPageMenu() {
  if (!profile.loaded) return;
  mode = UI_MENU_PAGES;

  // It opens on the screen where the current page is, so where you start from
  // is immediately visible. A hidden subpage is not in the list: in that case
  // it starts from the first screen.
  const int32_t position = visiblePos(currentPage);
  menuScreen = position >= 0 ? (uint8_t)(position / KEY_COUNT) : 0;
  markAll();
}

void uiOpenFileMenu() {
  mode = UI_MENU_FILES;
  menuScreen = 0;
  markAll();
}

bool uiFileMenuOpen() {
  return mode == UI_MENU_FILES;
}

static void closeMenu() {
  mode = UI_NORMAL;
  markAll();
}

static void advanceMenu() {
  const uint8_t screens = menuScreenCount();
  menuScreen = (menuScreen + 1) % screens;
  markAll();
}

// ============================================================
// CHANGING PAGE
// ============================================================
static void cycleNextPage() {
  const uint16_t total = visibleCount();
  if (total == 0) return;

  const int32_t position = visiblePos(currentPage);
  const uint16_t next = (position < 0) ? 0 : (uint16_t)((position + 1) % total);
  currentPage = visibleAt(next);
  markAll();
}

static void pushBack(uint16_t page) {
  if (backDepth < BACK_DEPTH) {
    backStack[backDepth++] = page;
  } else {
    // No deeper than this: the oldest step is lost.
    memmove(backStack, backStack + 1, sizeof(uint16_t) * (BACK_DEPTH - 1));
    backStack[BACK_DEPTH - 1] = page;
  }
}

static void goBack() {
  if (backDepth == 0) return;
  currentPage = backStack[--backDepth];
  markAll();
}

static void goToPage(uint16_t page) {
  if (!profile.loaded || page >= profile.pageCount) return;

  if (page != currentPage) {
    // A hidden page is a subpage: the page key becomes the way back. A visible
    // one, on the other hand, is a destination like any other and closes the
    // chain of subpages.
    if (!profile.pages[page].visible) pushBack(currentPage);
    else                              backDepth = 0;
    currentPage = page;
  }
  mode = UI_NORMAL;
  markAll();
}

// ============================================================
// STANDBY
// ============================================================
static void enterStandby() {
  if (mode == UI_STANDBY || mode == UI_ERROR) return;
  mode = UI_STANDBY;
  displaysPower(false);

  // When standby comes from a key, that key is still under the finger: its
  // release - or the long press that fires if it is held down - would arrive
  // right afterwards and be read as the wake-up. Everything the already pressed
  // keys have left to do is ignored, and the queue is emptied because the
  // waiting events belong to the keyboard that was still awake. Entering
  // through inactivity no key is down and nothing changes.
  for (uint8_t id = 0; id < BUTTON_COUNT; id++) {
    if (inputIsHeld(id)) swallow |= (uint16_t)1 << id;
  }
  inputFlush();
}

static void wakeUp() {
  if (mode != UI_STANDBY) return;
  mode = UI_NORMAL;
  displaysPower(true);
  markAll();
}

// ============================================================
// PRESSING A MACRO KEY
// ============================================================
static void clearPending() {
  for (uint8_t i = 0; i < KEY_COUNT; i++) {
    pending[i].key = nullptr;
    flash[i].on = false;
  }
}

// The press confirmation goes on screen BEFORE the action: runAction() is
// blocking and the drawing done by loop() would come afterwards. A shortcut
// keeps the USB bus busy for twenty milliseconds or so and the flash could be
// seen anyway, but a text waits on the endpoint for every character - a few
// milliseconds each - and on a long sentence the FLASH_MS were already over
// before the first drawing: the confirmation did not show up at all. Only the
// band of the pressed key is sent, so it costs a third of a screen and does not
// delay the macro noticeably.
static void buildColumnRows(uint8_t col, ScreenRows &rows);

static void drawKeyNow(uint8_t id) {
  const uint8_t col = id / ROWS_PER_COL;
  const uint8_t row = id % ROWS_PER_COL;

  ScreenRows rows;
  buildColumnRows(col, rows);

  // If the column was already due for a full redraw, all of it is sent: the
  // band alone would leave the old rest on screen.
  const bool whole = (colDirty[col] & 0x80) != 0;
  renderColumn(col, rows, whole ? (int8_t)-1 : (int8_t)row);
  colDirty[col] = whole ? 0 : (colDirty[col] & ~(1 << row));
}

static void pressKey(uint8_t id) {
  if (!profile.loaded || currentPage >= profile.pageCount) return;

  const KeyCfg &key = profile.pages[currentPage].keys[id];
  const uint16_t page = currentPage;   // the action can lead elsewhere

  const bool     second  = key.secondEnabled && key.second.type != ACT_NONE;
  const uint32_t delayMs = second ? (uint32_t)key.secondDelayDs * 100UL : 0;
  const bool     bar     = delayMs > COUNTDOWN_MIN_MS;

  // Where the bar is shown, it is the press confirmation itself: it starts as
  // the whole row, which is as visible as it gets, and on top of that the flash
  // would not look like a flash but like a hesitation, because it inverts a row
  // that the bar flips again a moment later. To have it ready for the drawing
  // below, the second action has to be queued now, before the action: the real
  // deadline is set again on the way back, so the wait stays counted from when
  // the first action finished and not from here.
  if (bar) {
    pending[id].key       = &key;
    pending[id].page      = page;
    pending[id].dueMs     = millis() + delayMs;
    pending[id].spanMs    = delayMs;
    pending[id].countdown = true;
    pending[id].lastBar   = 0xFF;
  } else {
    // The flash lasts at least FLASH_MS, but stays on until the action is over:
    // on a long text the inverted label says it is still typing, and it goes out
    // when the last character has been struck. The bar does the same job by
    // itself: it stays across the whole row for as long as the action keeps this
    // line of code busy.
    flash[id].on = true;
    flash[id].until = millis() + FLASH_MS;
  }
  drawKeyNow(id);

  runAction(key.first);

  if (!second) return;

  if (delayMs == 0) {
    // Immediately does not mean back to back: without this pause the two actions
    // overlap for whoever reads the keyboard at intervals and the second one is
    // lost. It only applies to the zero wait, because the smallest value the
    // configurator can write - a tenth of a second - is already enough by
    // itself.
    delay(ACTION_GAP_MS);
    runAction(key.second);
    return;
  }

  pending[id].key       = &key;
  pending[id].page      = page;
  pending[id].dueMs     = millis() + delayMs;
  pending[id].spanMs    = delayMs;
  pending[id].countdown = bar;
  pending[id].lastBar   = 0xFF;
  if (bar) markKey(id);
}

// ============================================================
// EVENTS
// ============================================================
static void handlePageKey(uint8_t kind) {
  // Ten seconds of page key lead back to the profile menu without a power
  // cycle. It comes after the long press, so whoever holds the key sees the page
  // menu open first: that is the sign the key is being read.
  if (kind == EV_HOLD && mode != UI_MENU_FILES) {
    if (profileFileCount() > 0) uiOpenFileMenu();
    return;
  }

  switch (mode) {
    case UI_NORMAL:
      // The long press leads to the page menu from any mode, even when the key
      // is acting as "back".
      if (kind == EV_LONG) {
        openPageMenu();
      } else if (kind == EV_CLICK) {
        if (backDepth > 0)                             goBack();
        else if (profile.pageKeyMode == MODE_NEXT_PAGE) cycleNextPage();
        else                                            openPageMenu();
      }
      break;

    case UI_MENU_PAGES:
      // Held down it exits without changing page; pressed and released it moves
      // to the next screen of the menu.
      if (kind == EV_LONG)       closeMenu();
      else if (kind == EV_CLICK) advanceMenu();
      break;

    case UI_MENU_FILES:
      if (kind == EV_LONG) {
        // At startup there is nothing to exit to yet: it only exits if a profile
        // is already open.
        if (profile.loaded) closeMenu();
      } else if (kind == EV_CLICK) {
        advanceMenu();
      }
      break;

    case UI_ERROR:
      // Any press will do: whoever reads the message does not know whether to
      // press or to hold.
      if (kind != EV_PRESS && profileFileCount() > 0) uiOpenFileMenu();
      break;

    default:
      break;
  }
}

static void handleMacroKey(uint8_t id) {
  const uint32_t slot = (uint32_t)menuScreen * KEY_COUNT + id;

  switch (mode) {
    case UI_NORMAL:
      pressKey(id);
      break;

    case UI_MENU_PAGES:
      if (slot < visibleCount()) {
        currentPage = visibleAt(slot);
        backDepth = 0;          // an explicit choice: the chain is reset
        closeMenu();
      }
      break;

    case UI_MENU_FILES:
      if (slot < profileFileCount()) {
        // The name is copied: profileLoad() rewrites the list of files.
        char chosen[MAX_FILENAME];
        snprintf(chosen, sizeof(chosen), "%s", profileFileName(slot));
        uiLoadFile(chosen);
      }
      break;

    default:
      break;
  }
}

void uiHandleEvent(const KeyEvent &event) {
  lastActivity = millis();

  // This check comes before everything else, wake-up included: a swallowed key
  // has already spent its press - it switched the displays on, or off - and what
  // is left must not wake anything. The bit stays set for the whole press,
  // uiTick() clears it when the key comes back up.
  if (swallow & ((uint16_t)1 << event.id)) return;

  if (mode == UI_STANDBY) {
    // The key that wakes up does nothing else.
    if (event.kind == EV_PRESS) swallow |= (uint16_t)1 << event.id;
    wakeUp();
    return;
  }

  if (event.id == PAGE_KEY_ID) {
    handlePageKey(event.kind);
  } else if (event.kind == EV_PRESS) {
    handleMacroKey(event.id);
  }
}

void uiDeviceAction(uint8_t action, uint16_t page) {
  switch (action) {
    case DEV_STANDBY:    enterStandby(); break;
    case DEV_PAGE_MENU:  openPageMenu(); break;
    case DEV_GO_TO_PAGE: goToPage(page); break;
    default: break;
  }
}

// ============================================================
// DEADLINES
// ============================================================
// How much of the row the countdown bar still takes, in pixels: all of it when
// the second action has just been queued, none when it is about to fire. It is
// rounded up, so as long as there is still some wait left a piece of bar is
// there to say so, and then to the nearest step.
static uint8_t countdownBar(const Pending &p, uint32_t now) {
  const int32_t left = (int32_t)(p.dueMs - now);
  if (left <= 0 || p.spanMs == 0) return 0;

  // The step is the slice of row the bar covers in COUNTDOWN_BAR_MS: the longer
  // the wait the narrower it gets, so the refreshes stay always the same
  // distance apart. Rounding up it never goes below one pixel, which is the
  // least the bar can move.
  const uint32_t step =
      ((uint32_t)SCREEN_W * COUNTDOWN_BAR_MS + p.spanMs - 1) / p.spanMs;

  uint32_t px = ((uint32_t)left * SCREEN_W + p.spanMs - 1) / p.spanMs;
  px = ((px + step - 1) / step) * step;
  if (px > SCREEN_W) px = SCREEN_W;   // a step that does not divide the row overshoots
  return (uint8_t)px;
}

void uiTick() {
  const uint32_t now = millis();

  for (uint8_t id = 0; id < KEY_COUNT; id++) {
    // ---- second action ----
    if (pending[id].key != nullptr) {
      if ((int32_t)(now - pending[id].dueMs) >= 0) {
        const Action *second = &pending[id].key->second;
        const bool onScreen = (pending[id].page == currentPage);
        pending[id].key = nullptr;
        if (onScreen) markKey(id);
        runAction(*second);
      } else if (pending[id].countdown) {
        const uint8_t bar = countdownBar(pending[id], now);
        if (bar != pending[id].lastBar) {
          pending[id].lastBar = bar;
          if (pending[id].page == currentPage) markKey(id);
        }
      }
    }

    // ---- confirmation flash ----
    if (flash[id].on && (int32_t)(now - flash[id].until) >= 0) {
      flash[id].on = false;
      markKey(id);
    }
  }

  // ---- swallowed keys ----
  // The events of this pass have already been consumed: a key that is no longer
  // down has finished its press and counts again.
  for (uint8_t id = 0; id < BUTTON_COUNT && swallow != 0; id++) {
    if ((swallow & ((uint16_t)1 << id)) && !inputIsHeld(id)) {
      swallow &= ~((uint16_t)1 << id);
    }
  }

  // ---- standby ----
  if (profile.standbyEnabled && profile.loaded &&
      (mode == UI_NORMAL || mode == UI_MENU_PAGES)) {
    bool busy = inputAnyDown();   // ten seconds of page key are not inactivity
    for (uint8_t id = 0; id < KEY_COUNT && !busy; id++) busy = (pending[id].key != nullptr);

    if (!busy && (now - lastActivity) >= profile.standbySeconds * 1000UL) {
      enterStandby();
    }
  }
}

// ============================================================
// DRAWING
// ============================================================
// "profile.json" -> "PROFILE": the extension is the same for all of them and
// would steal half the row.
static const char *fileLabel(uint8_t index, char *buffer, size_t size) {
  snprintf(buffer, size, "%s", profileFileName(index));
  char *dot = strrchr(buffer, '.');
  if (dot) *dot = '\0';
  return buffer;
}

static void buildColumnRows(uint8_t col, ScreenRows &rows) {
  const uint32_t base = (uint32_t)menuScreen * KEY_COUNT;

  for (uint8_t r = 0; r < ROWS_PER_COL; r++) {
    const uint8_t id = col * ROWS_PER_COL + r;
    rows.text[r] = "";
    rows.inverted[r] = false;
    rows.bar[r] = 0;

    if (mode == UI_MENU_PAGES) {
      const uint32_t slot = base + id;
      if (slot < visibleCount()) {
        const uint16_t page = visibleAt(slot);
        rows.text[r] = arenaStr(profile.pages[page].name);
        rows.inverted[r] = (page == currentPage);
      }

    } else if (mode == UI_MENU_FILES) {
      const uint32_t slot = base + id;
      if (slot < profileFileCount()) {
        rows.text[r] = fileLabel(slot, scratch[id], sizeof(scratch[id]));
        rows.inverted[r] = profile.loaded &&
                           strcasecmp(profileFileName(slot), profile.file) == 0;
      }

    } else if (profile.loaded && currentPage < profile.pageCount) {
      const KeyCfg &key = profile.pages[currentPage].keys[id];

      // During a long wait the bar runs behind the label and flips the row for
      // the stretch it covers: white background and black letters inside, the
      // usual black with white letters outside.
      const bool waiting = pending[id].key != nullptr && pending[id].countdown &&
                           pending[id].page == currentPage;

      // For the whole wait the row goes plain, whatever the profile says: the
      // bar is white because it flips a black background, and over an already
      // inverted label it would flip the white that is there - a black bar, and
      // an invisible one at that as soon as the background surrounds it. The
      // confirmation flash disappears too, since it inverts as well: the bar
      // starts as the whole row and flips it entirely already, so the flash
      // would only cancel it for an instant and the key would blink the other
      // way round. On the press it is seen anyway, because the flash goes on
      // screen before the second action is queued.
      rows.text[r] = arenaStr(key.label);
      rows.inverted[r] = waiting ? false
                                 : (key.inverted != flash[id].on);  // the flash inverts
      rows.bar[r] = waiting ? countdownBar(pending[id], millis()) : 0;
    }
  }
}

static void renderPageScreen() {
  if (mode == UI_MENU_PAGES) {
    renderPageMenu(MENU_PAGES, menuScreen, menuScreenCount());
    return;
  }
  if (mode == UI_MENU_FILES) {
    renderPageMenu(MENU_PROFILES, menuScreen, menuScreenCount());
    return;
  }
  if (!profile.loaded) return;

  // Subpage: the page key is the way back.
  if (backDepth > 0) {
    renderPageBack();
    return;
  }

  const uint16_t total = visibleCount();
  const char *name = arenaStr(profile.pages[currentPage].name);

  // With the menu as the page key behaviour, the display shows only the open
  // page. Same thing when there is a single page: there is nothing to tell it
  // apart from, so it stays white on black instead of inverted.
  if (profile.pageKeyMode == MODE_PAGE_MENU || total <= 1) {
    renderPageSingle(name, false);
    return;
  }

  // A three-row window over the list of visible pages, with the open one in the
  // middle where possible.
  const int32_t position = max<int32_t>(0, visiblePos(currentPage));
  const uint8_t rowCount = (uint8_t)min<uint16_t>(total, ROWS_PER_COL);

  int32_t first = 0;
  if (total > ROWS_PER_COL) {
    first = constrain(position - 1, (int32_t)0, (int32_t)(total - ROWS_PER_COL));
  }

  const char *names[ROWS_PER_COL];
  bool inverted[ROWS_PER_COL];
  for (uint8_t i = 0; i < rowCount; i++) {
    const uint16_t page = visibleAt((uint16_t)first + i);
    names[i] = arenaStr(profile.pages[page].name);
    inverted[i] = (page == currentPage);
  }
  renderPageList(names, inverted, rowCount, (uint16_t)first, total);
}

void uiRender() {
  if (mode == UI_STANDBY || mode == UI_ERROR) {
    // Nothing to send again: the screen is off or holds a fixed message.
    for (uint8_t c = 0; c < COL_COUNT; c++) colDirty[c] = 0;
    pageDirty = false;
    return;
  }

  if (pageDirty) {
    renderPageScreen();
    pageDirty = false;
  }

  for (uint8_t col = 0; col < COL_COUNT; col++) {
    if (colDirty[col] == 0) continue;

    // If a single band changed, only that one goes out: a third of the bytes on
    // the bus, which tells on the confirmation flash.
    int8_t onlyRow = -1;
    if (!(colDirty[col] & 0x80)) {
      const uint8_t bits = colDirty[col] & 0x07;
      if      (bits == 0x01) onlyRow = 0;
      else if (bits == 0x02) onlyRow = 1;
      else if (bits == 0x04) onlyRow = 2;
    }

    ScreenRows rows;
    buildColumnRows(col, rows);
    renderColumn(col, rows, onlyRow);
    colDirty[col] = 0;
  }
}

// ============================================================
// PROFILES
// ============================================================
void uiShowError(const char *title, const char *body) {
  mode = UI_ERROR;
  displaysPower(true);
  renderMessage(title, body);
  LOG_PRINTF("%s: %s\n", title, body);
}

bool uiLoadFile(const char *fileName) {
  char error[96];

  // The pending second actions point inside the profile that profileLoad() is
  // about to free: they have to be thrown away before, not after.
  clearPending();

  if (!profileLoad(fileName, error, sizeof(error))) {
    char body[192];
    snprintf(body, sizeof(body), "%s Press the page key to pick another file.", error);
    uiShowError("ERROR", body);
    return false;
  }

  profileWritePreference(profile.file);

  swallow = 0;
  backDepth = 0;
  menuScreen = 0;
  currentPage = visibleAt(0);
  mode = UI_NORMAL;
  lastActivity = millis();
  markAll();

  if (profileWarnings() > 0) {
    LOG_PRINTF("Profile loaded with %u anomalies (see above).\n", profileWarnings());
  }
  return true;
}

void uiBegin() {
  clearPending();
  memset(colDirty, 0, sizeof(colDirty));
  lastActivity = millis();
  mode = UI_ERROR;
}
